#pragma once

#include "core/btree/btree.hpp"
#include "core/store/collection.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include "core/shared_mutex.hpp"
#include <shared_mutex> // std::shared_lock
#include <string>
#include <vector>

namespace bisondb::query {

struct IndexBuildStats {
    std::size_t indexed = 0;
    // Documents whose field was missing, of a non-indexable type, NaN, or
    // whose encoded key exceeded the length cap. Unlike MongoDB (which
    // indexes missing fields as null), BisonDB simply leaves them out of the
    // index; the matcher still sees them on full scans.
    std::size_t skipped = 0;
};

// A collection plus its B+Tree indexes, kept consistent under one exclusive
// lock per mutating operation.
//
// Files in <dbdir>:
//   <name>.log            append-only record log (source of truth)
//   <name>._id.idx        unique B+Tree: encodeKey(_id) -> 8-byte log offset.
//                         Replaces the startup-scan hash index when clean; a
//                         missing or dirty file triggers the Phase 2 fallback
//                         (full log replay) and a rebuild.
//   <name>.<field>.idx    duplicate-mode B+Tree: composite(field, _id) -> ""
//   <name>.meta.json      index registry
class IndexedCollection {
  public:
    IndexedCollection(const std::string& dbdir, const std::string& name);

    // Inserts a document (Document value). A missing _id is generated; an
    // existing one must be an ObjectId and unique. Returns the _id.
    ObjectId insert(Value doc);

    bool eraseById(const ObjectId& oid);

    // Replaces the document with `oid` by appending a new version (the old
    // record becomes garbage until compaction) and updating all indexes.
    void update(const ObjectId& oid, Value newDoc);

    std::optional<Value> fetch(const ObjectId& oid);

    IndexBuildStats createIndex(const std::string& field);
    bool dropIndex(const std::string& field);
    std::vector<std::string> listIndexes() const; // includes "_id"
    bool hasIndex(const std::string& field) const;

    // Index access for the query planner. The _id index is always present.
    btree::BTree& idIndex() { return *idIndex_; }
    btree::BTree* fieldIndex(const std::string& field);

    std::size_t count();

    // Rewrites the log without garbage and rebuilds every index (offsets
    // change, so rebuilding all of them is the simple correct option).
    void compact();

    void sync();

    SharedMutex& mutex() { return mutex_; }

  private:
    std::string indexPath(const std::string& field) const;
    void rebuildIdIndex();
    IndexBuildStats buildFieldIndex(btree::BTree& tree, const std::string& field);
    void loadMeta();
    void saveMeta();
    void indexDocument(const ObjectId& oid, const Value& doc, bool add);
    static std::vector<uint8_t> offsetValue(uint64_t offset);
    static uint64_t offsetFromValue(std::span<const uint8_t> v);

    std::string dbdir_;
    std::string name_;
    store::CollectionLog log_;
    std::unique_ptr<btree::BTree> idIndex_;
    std::map<std::string, std::unique_ptr<btree::BTree>> fieldIndexes_;
    std::map<std::string, IndexBuildStats> indexStats_;
    SharedMutex mutex_;

    friend class QueryEngine;
};

} // namespace bisondb::query
