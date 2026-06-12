#include "core/btree/btree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bisondb::btree;

namespace {

using Bytes = std::vector<uint8_t>;

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("bisondb_btree_" + name);
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
    std::string str() const { return path.string(); }
};

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes numKey(uint32_t n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08u", n);
    return bytes(buf);
}

BTreeOptions smallPages() {
    BTreeOptions o;
    o.pageSize = 256;
    return o;
}

// Collects the whole tree through the cursor interface.
std::vector<std::pair<Bytes, Bytes>> dumpTree(BTree& t) {
    std::vector<std::pair<Bytes, Bytes>> out;
    for (auto c = t.lowerBound({}); c.valid(); c.next()) {
        out.emplace_back(Bytes(c.key().begin(), c.key().end()),
                         Bytes(c.value().begin(), c.value().end()));
    }
    return out;
}

} // namespace

TEST_CASE("btree basic point operations", "[btree]") {
    TempFile f("basic");
    BTree t(f.str(), smallPages());
    REQUIRE_FALSE(t.get(bytes("a")).has_value());
    t.insert(bytes("a"), bytes("1"));
    t.insert(bytes("b"), bytes("2"));
    REQUIRE(t.get(bytes("a")).value() == bytes("1"));
    REQUIRE(t.get(bytes("b")).value() == bytes("2"));
    REQUIRE_FALSE(t.get(bytes("c")).has_value());

    REQUIRE_THROWS_AS(t.insert(bytes("a"), bytes("x")), DuplicateKeyError);
    REQUIRE(t.erase(bytes("a")));
    REQUIRE_FALSE(t.erase(bytes("a")));
    REQUIRE_FALSE(t.get(bytes("a")).has_value());
}

TEST_CASE("leaf splits keep order and sibling links", "[btree]") {
    TempFile f("split");
    BTree t(f.str(), smallPages());
    // 256-byte pages with ~20-byte cells: a few hundred inserts force many
    // splits and at least three levels.
    for (uint32_t i = 0; i < 500; ++i) {
        t.insert(numKey(i * 7919 % 100000), numKey(i));
    }
    auto all = dumpTree(t);
    REQUIRE(all.size() == 500);
    for (std::size_t i = 0; i + 1 < all.size(); ++i) {
        REQUIRE(all[i].first < all[i + 1].first); // strict order across leaves
    }
}

TEST_CASE("sequential ascending and descending insertion", "[btree]") {
    for (bool ascending : {true, false}) {
        TempFile f(ascending ? "asc" : "desc");
        BTree t(f.str(), smallPages());
        for (uint32_t i = 0; i < 400; ++i) {
            t.insert(numKey(ascending ? i : 399 - i), numKey(i));
        }
        auto all = dumpTree(t);
        REQUIRE(all.size() == 400);
        for (uint32_t i = 0; i < 400; ++i) {
            REQUIRE(all[i].first == numKey(i));
        }
    }
}

TEST_CASE("exact node capacity boundaries", "[btree]") {
    TempFile f("capacity");
    BTree t(f.str(), smallPages());
    // Fill one leaf to the brink: cell = 2 slot + 4 + keyLen + valLen. With
    // 8-byte keys/values each cell costs 26 bytes; (256-12)/26 = 9 cells fit,
    // the 10th forces the first split.
    for (uint32_t i = 0; i < 9; ++i) {
        t.insert(numKey(i), numKey(i));
    }
    REQUIRE(dumpTree(t).size() == 9);
    t.insert(numKey(9), numKey(9));
    auto all = dumpTree(t);
    REQUIRE(all.size() == 10);
    for (uint32_t i = 0; i < 10; ++i) {
        REQUIRE(all[i].first == numKey(i));
    }
}

TEST_CASE("duplicate mode stores equal keys side by side", "[btree]") {
    TempFile f("dup");
    BTree t(f.str(), smallPages());
    t.insert(bytes("k"), bytes("1"), /*allowDuplicates=*/true);
    t.insert(bytes("k"), bytes("2"), /*allowDuplicates=*/true);
    t.insert(bytes("k"), bytes("3"), /*allowDuplicates=*/true);
    std::size_t n = 0;
    for (auto c = t.lowerBound(bytes("k")); c.valid(); c.next()) {
        REQUIRE(Bytes(c.key().begin(), c.key().end()) == bytes("k"));
        ++n;
    }
    REQUIRE(n == 3);
    REQUIRE(t.erase(bytes("k")));
    REQUIRE(t.erase(bytes("k")));
    REQUIRE(t.erase(bytes("k")));
    REQUIRE_FALSE(t.erase(bytes("k")));
}

TEST_CASE("lowerBound cursor honors range starts", "[btree]") {
    TempFile f("range");
    BTree t(f.str(), smallPages());
    for (uint32_t i = 0; i < 100; ++i) {
        t.insert(numKey(i * 2), numKey(i)); // even keys only
    }
    auto c = t.lowerBound(numKey(31)); // odd: lands on 32
    REQUIRE(c.valid());
    REQUIRE(Bytes(c.key().begin(), c.key().end()) == numKey(32));
    c.next();
    REQUIRE(Bytes(c.key().begin(), c.key().end()) == numKey(34));

    auto end = t.lowerBound(numKey(99999));
    REQUIRE_FALSE(end.valid());
}

TEST_CASE("erase to empty and reuse", "[btree]") {
    TempFile f("drain");
    BTree t(f.str(), smallPages());
    for (uint32_t i = 0; i < 300; ++i) {
        t.insert(numKey(i), numKey(i));
    }
    for (uint32_t i = 0; i < 300; ++i) {
        REQUIRE(t.erase(numKey(i)));
    }
    REQUIRE(dumpTree(t).empty());
    // The tree stays usable after a full drain (root collapse path).
    for (uint32_t i = 0; i < 50; ++i) {
        t.insert(numKey(i), numKey(i));
    }
    REQUIRE(dumpTree(t).size() == 50);
}

TEST_CASE("persistence: flush, reopen, identical contents", "[btree]") {
    TempFile f("persist");
    std::vector<std::pair<Bytes, Bytes>> expected;
    {
        BTree t(f.str(), smallPages());
        for (uint32_t i = 0; i < 400; ++i) {
            t.insert(numKey(i * 13 % 9999), numKey(i));
        }
        expected = dumpTree(t);
        t.sync();
    }
    BTree t(f.str(), smallPages());
    REQUIRE(t.wasCleanOnOpen());
    REQUIRE(dumpTree(t) == expected);
}

TEST_CASE("dirty flag: skipping the clean close reports RebuildRequired", "[btree]") {
    TempFile f("crash");
    {
        BTree t(f.str(), smallPages());
        t.insert(bytes("a"), bytes("1"));
        t.sync();
    }
    {
        // Mutate without syncing, then peek at the file from a second handle
        // exactly as a post-crash opener would see it.
        BTree t(f.str(), smallPages());
        t.insert(bytes("b"), bytes("2"));
        REQUIRE_THROWS_AS(BTree(f.str(), smallPages()), RebuildRequired);
        BTreeOptions accept = smallPages();
        accept.acceptDirty = true;
        REQUIRE_NOTHROW(BTree(f.str(), accept));
    }
}

TEST_CASE("model-based fuzz against std::map", "[btree][fuzz]") {
    for (uint32_t seed : {7u, 1234u, 999983u}) {
        TempFile f("fuzz_" + std::to_string(seed));
        BTree t(f.str(), smallPages());
        std::map<Bytes, Bytes> oracle;
        std::mt19937 rng(seed);

        auto randomKey = [&rng] {
            // Tight key space so erases hit existing keys often.
            return numKey(rng() % 5000);
        };

        const int kOps = 100000;
        for (int op = 0; op < kOps; ++op) {
            switch (rng() % 10) {
            case 0:
            case 1:
            case 2:
            case 3: { // insert
                Bytes k = randomKey();
                Bytes v = numKey(rng() % 100000000);
                if (oracle.contains(k)) {
                    REQUIRE_THROWS_AS(t.insert(k, v), DuplicateKeyError);
                } else {
                    t.insert(k, v);
                    oracle[k] = v;
                }
                break;
            }
            case 4:
            case 5:
            case 6: { // erase
                Bytes k = randomKey();
                bool removed = t.erase(k);
                REQUIRE(removed == (oracle.erase(k) == 1));
                break;
            }
            case 7:
            case 8: { // get
                Bytes k = randomKey();
                auto got = t.get(k);
                auto it = oracle.find(k);
                if (it == oracle.end()) {
                    REQUIRE_FALSE(got.has_value());
                } else {
                    REQUIRE(got.has_value());
                    REQUIRE(*got == it->second);
                }
                break;
            }
            default: { // bounded range scan
                Bytes k = randomKey();
                std::size_t steps = rng() % 20;
                auto c = t.lowerBound(k);
                auto it = oracle.lower_bound(k);
                for (std::size_t s = 0; s < steps; ++s) {
                    if (it == oracle.end()) {
                        REQUIRE_FALSE(c.valid());
                        break;
                    }
                    REQUIRE(c.valid());
                    REQUIRE(Bytes(c.key().begin(), c.key().end()) == it->first);
                    REQUIRE(Bytes(c.value().begin(), c.value().end()) == it->second);
                    c.next();
                    ++it;
                }
                break;
            }
            }
        }

        // Full iteration equality at the end.
        std::vector<std::pair<Bytes, Bytes>> expected(oracle.begin(), oracle.end());
        REQUIRE(dumpTree(t) == expected);
    }
}

TEST_CASE("model fuzz with persistence in the middle", "[btree][fuzz]") {
    TempFile f("fuzz_persist");
    std::map<Bytes, Bytes> oracle;
    std::mt19937 rng(31337);
    {
        BTree t(f.str(), smallPages());
        for (int i = 0; i < 5000; ++i) {
            Bytes k = numKey(rng() % 2000);
            if (!oracle.contains(k)) {
                t.insert(k, numKey(rng() % 1000));
                oracle[k] = t.get(k).value();
            }
        }
        t.sync();
    }
    BTree t(f.str(), smallPages());
    for (int i = 0; i < 5000; ++i) {
        Bytes k = numKey(rng() % 2000);
        bool removed = t.erase(k);
        REQUIRE(removed == (oracle.erase(k) == 1));
    }
    std::vector<std::pair<Bytes, Bytes>> expected(oracle.begin(), oracle.end());
    REQUIRE(dumpTree(t) == expected);
}
