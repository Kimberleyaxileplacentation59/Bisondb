#pragma once

#include "core/btree/pager.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include "core/shared_mutex.hpp"
#include <shared_mutex> // std::shared_lock
#include <span>
#include <string>
#include <vector>

namespace bisondb::btree {

class DuplicateKeyError : public Error {
  public:
    using Error::Error;
};

struct BTreeOptions {
    uint32_t pageSize = Pager::kDefaultPageSize;
    std::size_t cacheCapacity = Pager::kDefaultCacheCapacity;
    // When false, opening a file whose clean flag is unset throws
    // RebuildRequired (the caller should delete and rebuild the index).
    bool acceptDirty = false;
};

// On-disk B+Tree mapping encoded key bytes to small value blobs (<= 64
// bytes). Concurrency: single writer / multiple readers via a tree-level
// shared_mutex; page-level latching is out of scope. A Cursor holds the
// shared lock for its whole lifetime, so finish (or destroy) cursors before
// mutating.
class BTree {
  public:
    static constexpr std::size_t kMaxValueLength = 64;
    // Encoded field keys cap at 512; composite secondary keys add a separator
    // and a 12-byte ObjectId.
    static constexpr std::size_t kMaxKeyLength = 544;

    explicit BTree(const std::string& path, const BTreeOptions& options = {});

    bool wasCleanOnOpen() const noexcept { return pager_.wasCleanOnOpen(); }

    std::optional<std::vector<uint8_t>> get(std::span<const uint8_t> key);

    // Unique mode (allowDuplicates=false) throws DuplicateKeyError on an
    // existing key. Duplicate mode inserts exact-equal keys side by side;
    // secondary indexes avoid that entirely via composite keys.
    void insert(std::span<const uint8_t> key, std::span<const uint8_t> value,
                bool allowDuplicates = false);

    // Removes the first cell matching `key` exactly. Lazy underflow: pages
    // may stay underfull; a page is unlinked and freed only when it reaches
    // zero cells (with root collapse when the root has a single child).
    bool erase(std::span<const uint8_t> key);

    // Forward iterator over (key, value) pairs in key order. Holds the tree's
    // shared lock while alive.
    class Cursor {
      public:
        bool valid() const noexcept { return valid_; }
        std::span<const uint8_t> key() const noexcept { return {keyBuf_.data(), keyBuf_.size()}; }
        std::span<const uint8_t> value() const noexcept { return {valBuf_.data(), valBuf_.size()}; }
        void next();

      private:
        friend class BTree;
        Cursor(BTree& tree, std::shared_lock<SharedMutex> lock)
            : tree_(&tree), lock_(std::move(lock)) {}
        void loadCurrent();
        void skipEmptyLeaves();

        BTree* tree_;
        std::shared_lock<SharedMutex> lock_;
        std::vector<uint8_t> page_;
        PageId leaf_ = 0;
        uint16_t slot_ = 0;
        bool valid_ = false;
        std::vector<uint8_t> keyBuf_;
        std::vector<uint8_t> valBuf_;
    };

    // Positions at the first key >= `key`; an empty key starts at the
    // beginning of the tree.
    Cursor lowerBound(std::span<const uint8_t> key);

    // Flushes all dirty pages and marks the file clean.
    void sync();

    uint32_t pageSize() const noexcept { return pager_.pageSize(); }

  private:
    struct SplitResult {
        std::vector<uint8_t> separator;
        PageId rightPage;
    };

    void readNode(PageId id, std::vector<uint8_t>& buf);
    std::optional<SplitResult> insertRecursive(PageId pageId, std::span<const uint8_t> key,
                                               std::span<const uint8_t> value,
                                               bool allowDuplicates);
    SplitResult splitLeaf(std::vector<uint8_t>& page, PageId pageId);
    SplitResult splitInternal(std::vector<uint8_t>& page, PageId pageId);
    void fixLeafChainBeforeFree(const std::vector<std::pair<PageId, uint16_t>>& path,
                                PageId emptyLeaf);
    PageId descendRightmostLeaf(PageId start);

    Pager pager_;
    SharedMutex mutex_;
};

} // namespace bisondb::btree
