#include "core/btree/btree.hpp"

#include "core/btree/key_codec.hpp"
#include "core/btree/node.hpp"

#include <cassert>
#include <utility>

namespace bisondb::btree {

namespace {

using Bytes = std::vector<uint8_t>;

struct LeafEntry {
    Bytes key;
    Bytes value;
};

struct InternalEntry {
    Bytes key;
    PageId child; // subtree with keys < key
};

Bytes toBytes(std::span<const uint8_t> s) { return Bytes(s.begin(), s.end()); }

// Transient in-memory form of a node; mutations parse the page, edit the
// vector, and re-serialize. Reads never need this — they binary-search the
// slot array in place.
std::vector<LeafEntry> parseLeaf(const Node& n) {
    std::vector<LeafEntry> out;
    out.reserve(n.cellCount());
    for (uint16_t i = 0; i < n.cellCount(); ++i) {
        out.push_back({toBytes(n.keyAt(i)), toBytes(n.valueAt(i))});
    }
    return out;
}

std::vector<InternalEntry> parseInternal(const Node& n) {
    std::vector<InternalEntry> out;
    out.reserve(n.cellCount());
    for (uint16_t i = 0; i < n.cellCount(); ++i) {
        out.push_back({toBytes(n.keyAt(i)), n.childAt(i)});
    }
    return out;
}

std::size_t leafBytes(const std::vector<LeafEntry>& entries) {
    std::size_t total = 0;
    for (const LeafEntry& e : entries) {
        total += 2 + Node::leafCellSize(e.key.size(), e.value.size());
    }
    return total;
}

std::size_t internalBytes(const std::vector<InternalEntry>& entries) {
    std::size_t total = 0;
    for (const InternalEntry& e : entries) {
        total += 2 + Node::internalCellSize(e.key.size());
    }
    return total;
}

bool serializeLeaf(Node& n, const std::vector<LeafEntry>& entries, PageId rightSibling) {
    n.init(Node::kLeaf);
    n.setRight(rightSibling);
    for (uint16_t i = 0; i < entries.size(); ++i) {
        const LeafEntry& e = entries[i];
        if (!n.insertLeafCell(i, {e.key.data(), e.key.size()}, {e.value.data(), e.value.size()})) {
            return false;
        }
    }
    return true;
}

bool serializeInternal(Node& n, const std::vector<InternalEntry>& entries, PageId rightmost) {
    n.init(Node::kInternal);
    n.setRight(rightmost);
    for (uint16_t i = 0; i < entries.size(); ++i) {
        const InternalEntry& e = entries[i];
        if (!n.insertInternalCell(i, {e.key.data(), e.key.size()}, e.child)) {
            return false;
        }
    }
    return true;
}

// Split index such that the left part holds roughly half the bytes, with at
// least one entry on each side.
template <typename E>
std::size_t splitPoint(const std::vector<E>& entries, std::size_t totalBytes,
                       std::size_t (*entrySize)(const E&)) {
    std::size_t acc = 0;
    for (std::size_t i = 0; i + 1 < entries.size(); ++i) {
        acc += entrySize(entries[i]);
        if (acc * 2 >= totalBytes) {
            return i + 1;
        }
    }
    return entries.size() - 1;
}

std::size_t leafEntrySize(const LeafEntry& e) {
    return 2 + Node::leafCellSize(e.key.size(), e.value.size());
}
std::size_t internalEntrySize(const InternalEntry& e) {
    return 2 + Node::internalCellSize(e.key.size());
}

} // namespace

BTree::BTree(const std::string& path, const BTreeOptions& options)
    : pager_(path, options.pageSize, options.cacheCapacity) {
    if (!pager_.wasCleanOnOpen() && !options.acceptDirty) {
        throw RebuildRequired("index file was not closed cleanly: " + path);
    }
    if (pager_.rootPage() == 0) {
        PageId root = pager_.allocPage();
        std::vector<uint8_t> buf(pager_.pageSize());
        Node n(buf.data(), pager_.pageSize());
        n.init(Node::kLeaf);
        pager_.writePage(root, buf.data());
        pager_.setRootPage(root);
    }
}

void BTree::readNode(PageId id, std::vector<uint8_t>& buf) {
    buf.resize(pager_.pageSize());
    pager_.readPage(id, buf.data());
}

std::optional<std::vector<uint8_t>> BTree::get(std::span<const uint8_t> key) {
    std::shared_lock lock(mutex_);
    std::vector<uint8_t> buf;
    PageId cur = pager_.rootPage();
    while (true) {
        readNode(cur, buf);
        Node n(buf.data(), pager_.pageSize());
        if (n.isLeaf()) {
            uint16_t i = n.lowerBound(key);
            if (i < n.cellCount() && Node::compareKeys(n.keyAt(i), key) == 0) {
                return toBytes(n.valueAt(i));
            }
            return std::nullopt;
        }
        uint16_t pos = n.upperBound(key);
        cur = pos < n.cellCount() ? n.childAt(pos) : n.right();
    }
}

std::optional<BTree::SplitResult> BTree::insertRecursive(PageId pageId,
                                                         std::span<const uint8_t> key,
                                                         std::span<const uint8_t> value,
                                                         bool allowDuplicates) {
    std::vector<uint8_t> buf;
    readNode(pageId, buf);
    Node n(buf.data(), pager_.pageSize());

    if (n.isLeaf()) {
        uint16_t i = allowDuplicates ? n.upperBound(key) : n.lowerBound(key);
        if (!allowDuplicates && i < n.cellCount() && Node::compareKeys(n.keyAt(i), key) == 0) {
            throw DuplicateKeyError("key already exists in unique index");
        }
        if (n.insertLeafCell(i, key, value)) {
            pager_.writePage(pageId, buf.data());
            return std::nullopt;
        }
        // Page full: rebuild as entry list, insert, split into two pages.
        std::vector<LeafEntry> entries = parseLeaf(n);
        entries.insert(entries.begin() + i, LeafEntry{toBytes(key), toBytes(value)});
        std::size_t total = leafBytes(entries);
        std::size_t sp = splitPoint(entries, total, leafEntrySize);

        PageId rightId = pager_.allocPage();
        std::vector<LeafEntry> rightEntries(entries.begin() + static_cast<std::ptrdiff_t>(sp),
                                            entries.end());
        entries.resize(sp);

        std::vector<uint8_t> rightBuf(pager_.pageSize());
        Node rightNode(rightBuf.data(), pager_.pageSize());
        bool ok = serializeLeaf(rightNode, rightEntries, n.right());
        Node leftNode(buf.data(), pager_.pageSize());
        ok = serializeLeaf(leftNode, entries, rightId) && ok;
        if (!ok) {
            throw Error("internal error: leaf split does not fit page");
        }
        pager_.writePage(rightId, rightBuf.data());
        pager_.writePage(pageId, buf.data());
        return SplitResult{rightEntries.front().key, rightId};
    }

    uint16_t pos = n.upperBound(key);
    PageId child = pos < n.cellCount() ? n.childAt(pos) : n.right();
    std::optional<SplitResult> split = insertRecursive(child, key, value, allowDuplicates);
    if (!split) {
        return std::nullopt;
    }

    // Child split into (child, separator, rightPage): insert the separator at
    // `pos` keeping `child` to its left, and point the next child slot at the
    // new right page.
    readNode(pageId, buf); // re-read: the recursion may have evicted our copy
    Node node(buf.data(), pager_.pageSize());
    std::vector<InternalEntry> entries = parseInternal(node);
    PageId rightmost = node.right();
    entries.insert(entries.begin() + pos, InternalEntry{split->separator, child});
    if (static_cast<std::size_t>(pos) + 1 < entries.size()) {
        entries[pos + 1].child = split->rightPage;
    } else {
        rightmost = split->rightPage;
    }

    if (serializeInternal(node, entries, rightmost)) {
        pager_.writePage(pageId, buf.data());
        return std::nullopt;
    }

    // Internal overflow: promote a median, split the entry list.
    std::size_t total = internalBytes(entries);
    std::size_t m = splitPoint(entries, total, internalEntrySize);
    if (m >= entries.size()) {
        m = entries.size() - 1;
    }
    Bytes promoted = entries[m].key;
    PageId leftRightmost = entries[m].child;

    std::vector<InternalEntry> rightEntries(entries.begin() + static_cast<std::ptrdiff_t>(m) + 1,
                                            entries.end());
    entries.resize(m);

    PageId rightId = pager_.allocPage();
    std::vector<uint8_t> rightBuf(pager_.pageSize());
    Node rightNode(rightBuf.data(), pager_.pageSize());
    bool ok = serializeInternal(rightNode, rightEntries, rightmost);
    Node leftNode(buf.data(), pager_.pageSize());
    ok = serializeInternal(leftNode, entries, leftRightmost) && ok;
    if (!ok) {
        throw Error("internal error: internal split does not fit page");
    }
    pager_.writePage(rightId, rightBuf.data());
    pager_.writePage(pageId, buf.data());
    return SplitResult{std::move(promoted), rightId};
}

void BTree::insert(std::span<const uint8_t> key, std::span<const uint8_t> value,
                   bool allowDuplicates) {
    if (key.size() > kMaxKeyLength) {
        throw KeyTooLong("key is " + std::to_string(key.size()) + " bytes (max " +
                         std::to_string(kMaxKeyLength) + ")");
    }
    if (value.size() > kMaxValueLength) {
        throw Error("value exceeds " + std::to_string(kMaxValueLength) + " bytes");
    }
    if (Node::leafCellSize(key.size(), value.size()) + 2 + Node::kHeaderSize > pager_.pageSize()) {
        throw KeyTooLong("key/value pair does not fit a page of " +
                         std::to_string(pager_.pageSize()) + " bytes");
    }
    std::unique_lock lock(mutex_);
    std::optional<SplitResult> split =
        insertRecursive(pager_.rootPage(), key, value, allowDuplicates);
    if (split) {
        PageId oldRoot = pager_.rootPage();
        PageId newRoot = pager_.allocPage();
        std::vector<uint8_t> buf(pager_.pageSize());
        Node n(buf.data(), pager_.pageSize());
        std::vector<InternalEntry> entries{InternalEntry{split->separator, oldRoot}};
        if (!serializeInternal(n, entries, split->rightPage)) {
            throw Error("internal error: new root does not fit page");
        }
        pager_.writePage(newRoot, buf.data());
        pager_.setRootPage(newRoot);
    }
}

void BTree::fixLeafChainBeforeFree(const std::vector<std::pair<PageId, uint16_t>>& path,
                                   PageId emptyLeaf) {
    // Find the deepest ancestor where we did not take the leftmost branch;
    // the chain predecessor is the rightmost leaf of the subtree just left of
    // the branch we took. If every step was leftmost, emptyLeaf is the head
    // of the chain and nothing points at it.
    for (std::size_t i = path.size(); i-- > 0;) {
        auto [pageId, childPos] = path[i];
        if (childPos == 0) {
            continue;
        }
        std::vector<uint8_t> buf;
        readNode(pageId, buf);
        Node n(buf.data(), pager_.pageSize());
        PageId pred = n.childAt(static_cast<uint16_t>(childPos - 1));
        PageId predLeaf = descendRightmostLeaf(pred);

        std::vector<uint8_t> leafBuf;
        readNode(emptyLeaf, leafBuf);
        Node empty(leafBuf.data(), pager_.pageSize());
        PageId after = empty.right();

        std::vector<uint8_t> predBuf;
        readNode(predLeaf, predBuf);
        Node p(predBuf.data(), pager_.pageSize());
        p.setRight(after);
        pager_.writePage(predLeaf, predBuf.data());
        return;
    }
}

PageId BTree::descendRightmostLeaf(PageId start) {
    std::vector<uint8_t> buf;
    PageId cur = start;
    while (true) {
        readNode(cur, buf);
        Node n(buf.data(), pager_.pageSize());
        if (n.isLeaf()) {
            return cur;
        }
        cur = n.right();
    }
}

bool BTree::erase(std::span<const uint8_t> key) {
    std::unique_lock lock(mutex_);
    std::vector<std::pair<PageId, uint16_t>> path; // (page, child position taken)
    std::vector<uint8_t> buf;
    PageId cur = pager_.rootPage();
    while (true) {
        readNode(cur, buf);
        Node n(buf.data(), pager_.pageSize());
        if (n.isLeaf()) {
            break;
        }
        uint16_t pos = n.upperBound(key);
        path.emplace_back(cur, pos);
        cur = pos < n.cellCount() ? n.childAt(pos) : n.right();
    }

    Node leaf(buf.data(), pager_.pageSize());
    uint16_t i = leaf.lowerBound(key);
    if (i >= leaf.cellCount() || Node::compareKeys(leaf.keyAt(i), key) != 0) {
        return false;
    }
    leaf.removeCell(i);
    if (leaf.cellCount() > 0 || path.empty()) {
        // Lazy underflow: underfull pages are fine; an empty root leaf simply
        // means an empty tree.
        pager_.writePage(cur, buf.data());
        return true;
    }

    // The leaf is empty and is not the root: unlink it from the sibling chain
    // and from its parent, freeing childless ancestors as we go.
    fixLeafChainBeforeFree(path, cur);
    PageId removed = cur;
    pager_.freePage(removed);

    for (std::size_t level = path.size(); level-- > 0;) {
        auto [pageId, childPos] = path[level];
        std::vector<uint8_t> nodeBuf;
        readNode(pageId, nodeBuf);
        Node n(nodeBuf.data(), pager_.pageSize());
        std::vector<InternalEntry> entries = parseInternal(n);
        PageId rightmost = n.right();

        if (childPos < entries.size()) {
            entries.erase(entries.begin() + childPos);
        } else if (!entries.empty()) {
            rightmost = entries.back().child;
            entries.pop_back();
        } else {
            // Passthrough node lost its only child: free it and keep
            // unlinking at the next level (or reset the tree at the root).
            if (level == 0) {
                Node root(nodeBuf.data(), pager_.pageSize());
                root.init(Node::kLeaf);
                pager_.writePage(pageId, nodeBuf.data());
                return true;
            }
            pager_.freePage(pageId);
            continue;
        }
        if (!serializeInternal(n, entries, rightmost)) {
            throw Error("internal error: node rewrite does not fit page");
        }
        pager_.writePage(pageId, nodeBuf.data());
        break;
    }

    // Root collapse: a rootful of nothing but a rightmost child pointer is
    // replaced by that child.
    while (true) {
        PageId rootId = pager_.rootPage();
        readNode(rootId, buf);
        Node root(buf.data(), pager_.pageSize());
        if (root.isLeaf() || root.cellCount() > 0) {
            break;
        }
        PageId onlyChild = root.right();
        pager_.freePage(rootId);
        pager_.setRootPage(onlyChild);
    }
    return true;
}

BTree::Cursor BTree::lowerBound(std::span<const uint8_t> key) {
    std::shared_lock lock(mutex_);
    Cursor c(*this, std::move(lock));
    std::vector<uint8_t> buf;
    PageId cur = pager_.rootPage();
    while (true) {
        readNode(cur, buf);
        Node n(buf.data(), pager_.pageSize());
        if (n.isLeaf()) {
            c.page_ = buf;
            c.leaf_ = cur;
            c.slot_ = n.lowerBound(key);
            break;
        }
        uint16_t pos = n.upperBound(key);
        cur = pos < n.cellCount() ? n.childAt(pos) : n.right();
    }
    c.skipEmptyLeaves();
    c.loadCurrent();
    return c;
}

void BTree::Cursor::skipEmptyLeaves() {
    while (true) {
        Node n(page_.data(), tree_->pager_.pageSize());
        if (slot_ < n.cellCount()) {
            return;
        }
        PageId next = n.right();
        if (next == 0) {
            valid_ = false;
            leaf_ = 0;
            return;
        }
        tree_->readNode(next, page_);
        leaf_ = next;
        slot_ = 0;
    }
}

void BTree::Cursor::loadCurrent() {
    if (leaf_ == 0) {
        valid_ = false;
        return;
    }
    Node n(page_.data(), tree_->pager_.pageSize());
    if (slot_ >= n.cellCount()) {
        valid_ = false;
        return;
    }
    auto k = n.keyAt(slot_);
    auto v = n.valueAt(slot_);
    keyBuf_.assign(k.begin(), k.end());
    valBuf_.assign(v.begin(), v.end());
    valid_ = true;
}

void BTree::Cursor::next() {
    if (!valid_) {
        return;
    }
    ++slot_;
    skipEmptyLeaves();
    loadCurrent();
}

void BTree::sync() {
    std::unique_lock lock(mutex_);
    pager_.flushAll();
}

} // namespace bisondb::btree
