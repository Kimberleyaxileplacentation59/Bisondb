#pragma once

#include "core/error.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bisondb::store {

class StoreError : public Error {
  public:
    using Error::Error;
};

// Append-only record log backing one collection: <dbdir>/<name>.log.
// Record framing: u8 type (1 = PUT, 2 = DEL) | u32 payloadLen | payload.
// PUT payload is a BSON document (which must contain an ObjectId _id);
// DEL payload is the 12 _id bytes. The log is the source of truth: replaying
// it front to back (last entry per _id wins) reconstructs the live set, and
// a trailing partial record (torn write) is ignored.
//
// This class is deliberately dumb: it appends, reads records at offsets, and
// replays. Oid -> offset mapping lives in the _id B+Tree owned by the index
// layer; the replay path exists to (re)build that index. File access is
// internally serialized: the single FILE* cursor (fseek + fread pairs) must
// not interleave even between logically concurrent readers.
class CollectionLog {
  public:
    CollectionLog(const std::string& dbdir, const std::string& name);
    ~CollectionLog();

    CollectionLog(const CollectionLog&) = delete;
    CollectionLog& operator=(const CollectionLog&) = delete;

    // Appends a PUT record; returns the record's file offset.
    uint64_t appendPut(const std::vector<uint8_t>& bsonDoc);
    // Appends a DEL record for the given _id.
    void appendDelete(const ObjectId& oid);

    // Reads and decodes the document of the PUT record at `offset`.
    Value readDocumentAt(uint64_t offset);

    // Replays the whole log. For every complete record, calls fn(isPut,
    // offset, oid, docBytesOrNull). Stops silently at a torn tail.
    using ReplayFn = std::function<void(bool isPut, uint64_t offset, const ObjectId& oid,
                                        const std::vector<uint8_t>* docBytes)>;
    void replay(const ReplayFn& fn);

    // Phase 2-style startup scan: oid -> offset of the latest PUT, with
    // deletions applied. Used to (re)build the _id index.
    std::unordered_map<std::string, uint64_t> buildOffsetMap();

    void sync();
    uint64_t sizeBytes();

    // Rewrites the log keeping only the records in `liveOffsets` (each a PUT
    // offset), in the given order. Returns old offset -> new offset so the
    // caller can rebuild indexes. The log is synced afterwards.
    std::unordered_map<uint64_t, uint64_t> compact(const std::vector<uint64_t>& liveOffsets);

    const std::string& path() const noexcept { return path_; }

  private:
    Value readDocumentAtLocked(uint64_t offset);

    std::string path_;
    std::FILE* file_ = nullptr;
    std::mutex ioMutex_;
};

// Extracts the _id ObjectId from a decoded document; throws StoreError when
// absent or not an ObjectId.
ObjectId requireDocId(const Value& doc);

// Generates a fresh ObjectId (timestamp + random + counter, MongoDB-style).
ObjectId generateObjectId();

// Dotted-path lookup over nested Documents ("address.city"). Arrays are not
// traversed. Returns nullptr when any segment is missing or non-document.
const Value* lookupPath(const Value& doc, const std::string& path);

} // namespace bisondb::store
