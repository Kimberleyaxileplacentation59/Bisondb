#include "core/store/collection.hpp"

#include "core/bson_decoder.hpp"
#include "core/platform.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace bisondb::store {

namespace {

constexpr uint8_t kPut = 1;
constexpr uint8_t kDel = 2;
constexpr std::size_t kRecordHeader = 5; // u8 type + u32 len

void fsyncFile(std::FILE* f) {
    if (std::fflush(f) != 0) {
        throw StoreError("fflush failed");
    }
#if defined(BISONDB_PLATFORM_WINDOWS)
    if (_commit(_fileno(f)) != 0) {
        throw StoreError("_commit failed");
    }
#else
    if (fsync(fileno(f)) != 0) {
        throw StoreError("fsync failed");
    }
#endif
}

void seekTo(std::FILE* f, uint64_t offset) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

uint64_t tellPos(std::FILE* f) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    return static_cast<uint64_t>(_ftelli64(f));
#else
    return static_cast<uint64_t>(ftello(f));
#endif
}

} // namespace

CollectionLog::CollectionLog(const std::string& dbdir, const std::string& name) {
    std::filesystem::create_directories(dbdir);
    path_ = (std::filesystem::path(dbdir) / (name + ".log")).string();
    file_ = std::fopen(path_.c_str(), "r+b");
    if (file_ == nullptr) {
        file_ = std::fopen(path_.c_str(), "w+b");
    }
    if (file_ == nullptr) {
        throw StoreError("cannot open collection log: " + path_);
    }
}

CollectionLog::~CollectionLog() {
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
    }
}

uint64_t CollectionLog::appendPut(const std::vector<uint8_t>& bsonDoc) {
    std::lock_guard lock(ioMutex_);
    std::fseek(file_, 0, SEEK_END);
    uint64_t offset = tellPos(file_);
    uint8_t header[kRecordHeader];
    header[0] = kPut;
    uint32_t len = static_cast<uint32_t>(bsonDoc.size());
    header[1] = static_cast<uint8_t>(len);
    header[2] = static_cast<uint8_t>(len >> 8);
    header[3] = static_cast<uint8_t>(len >> 16);
    header[4] = static_cast<uint8_t>(len >> 24);
    if (std::fwrite(header, 1, kRecordHeader, file_) != kRecordHeader ||
        std::fwrite(bsonDoc.data(), 1, bsonDoc.size(), file_) != bsonDoc.size()) {
        throw StoreError("failed appending PUT record");
    }
    return offset;
}

void CollectionLog::appendDelete(const ObjectId& oid) {
    std::lock_guard lock(ioMutex_);
    std::fseek(file_, 0, SEEK_END);
    uint8_t rec[kRecordHeader + 12];
    rec[0] = kDel;
    rec[1] = 12;
    rec[2] = 0;
    rec[3] = 0;
    rec[4] = 0;
    std::memcpy(rec + kRecordHeader, oid.bytes.data(), 12);
    if (std::fwrite(rec, 1, sizeof(rec), file_) != sizeof(rec)) {
        throw StoreError("failed appending DEL record");
    }
}

Value CollectionLog::readDocumentAt(uint64_t offset) {
    std::lock_guard lock(ioMutex_);
    return readDocumentAtLocked(offset);
}

Value CollectionLog::readDocumentAtLocked(uint64_t offset) {
    seekTo(file_, offset);
    uint8_t header[kRecordHeader];
    if (std::fread(header, 1, kRecordHeader, file_) != kRecordHeader || header[0] != kPut) {
        throw StoreError("no PUT record at offset " + std::to_string(offset));
    }
    uint32_t len = static_cast<uint32_t>(header[1]) | (static_cast<uint32_t>(header[2]) << 8) |
                   (static_cast<uint32_t>(header[3]) << 16) |
                   (static_cast<uint32_t>(header[4]) << 24);
    std::vector<uint8_t> payload(len);
    if (std::fread(payload.data(), 1, len, file_) != len) {
        throw StoreError("truncated PUT record at offset " + std::to_string(offset));
    }
    return decodeDocument(payload);
}

void CollectionLog::replay(const ReplayFn& fn) {
    std::lock_guard lock(ioMutex_);
    std::fseek(file_, 0, SEEK_END);
    uint64_t end = tellPos(file_);
    uint64_t pos = 0;
    std::vector<uint8_t> payload;
    while (pos + kRecordHeader <= end) {
        seekTo(file_, pos);
        uint8_t header[kRecordHeader];
        if (std::fread(header, 1, kRecordHeader, file_) != kRecordHeader) {
            break;
        }
        uint32_t len = static_cast<uint32_t>(header[1]) | (static_cast<uint32_t>(header[2]) << 8) |
                       (static_cast<uint32_t>(header[3]) << 16) |
                       (static_cast<uint32_t>(header[4]) << 24);
        if (pos + kRecordHeader + len > end) {
            break; // torn tail: the log is valid up to the last whole record
        }
        payload.resize(len);
        if (len > 0 && std::fread(payload.data(), 1, len, file_) != len) {
            break;
        }
        if (header[0] == kPut) {
            Value doc = decodeDocument(payload);
            ObjectId oid = requireDocId(doc);
            fn(true, pos, oid, &payload);
        } else if (header[0] == kDel && len == 12) {
            ObjectId oid;
            std::memcpy(oid.bytes.data(), payload.data(), 12);
            fn(false, pos, oid, nullptr);
        } else {
            throw StoreError("corrupt record type at offset " + std::to_string(pos));
        }
        pos += kRecordHeader + len;
    }
}

std::unordered_map<std::string, uint64_t> CollectionLog::buildOffsetMap() {
    std::unordered_map<std::string, uint64_t> map;
    replay([&map](bool isPut, uint64_t offset, const ObjectId& oid, const std::vector<uint8_t>*) {
        std::string key(reinterpret_cast<const char*>(oid.bytes.data()), 12);
        if (isPut) {
            map[key] = offset;
        } else {
            map.erase(key);
        }
    });
    return map;
}

void CollectionLog::sync() {
    std::lock_guard lock(ioMutex_);
    fsyncFile(file_);
}

uint64_t CollectionLog::sizeBytes() {
    std::lock_guard lock(ioMutex_);
    std::fseek(file_, 0, SEEK_END);
    return tellPos(file_);
}

std::unordered_map<uint64_t, uint64_t>
CollectionLog::compact(const std::vector<uint64_t>& liveOffsets) {
    std::lock_guard lock(ioMutex_);
    std::string tmpPath = path_ + ".compact";
    std::FILE* out = std::fopen(tmpPath.c_str(), "w+b");
    if (out == nullptr) {
        throw StoreError("cannot create compaction file: " + tmpPath);
    }
    std::unordered_map<uint64_t, uint64_t> remap;
    try {
        uint64_t newPos = 0;
        for (uint64_t off : liveOffsets) {
            seekTo(file_, off);
            uint8_t header[kRecordHeader];
            if (std::fread(header, 1, kRecordHeader, file_) != kRecordHeader || header[0] != kPut) {
                throw StoreError("compaction: bad live offset " + std::to_string(off));
            }
            uint32_t len =
                static_cast<uint32_t>(header[1]) | (static_cast<uint32_t>(header[2]) << 8) |
                (static_cast<uint32_t>(header[3]) << 16) | (static_cast<uint32_t>(header[4]) << 24);
            std::vector<uint8_t> payload(len);
            if (std::fread(payload.data(), 1, len, file_) != len) {
                throw StoreError("compaction: truncated record");
            }
            if (std::fwrite(header, 1, kRecordHeader, out) != kRecordHeader ||
                std::fwrite(payload.data(), 1, len, out) != len) {
                throw StoreError("compaction: write failed");
            }
            remap[off] = newPos;
            newPos += kRecordHeader + len;
        }
        fsyncFile(out);
        std::fclose(out);
        out = nullptr;
        std::fclose(file_);
        file_ = nullptr;
        std::filesystem::rename(tmpPath, path_);
        file_ = std::fopen(path_.c_str(), "r+b");
        if (file_ == nullptr) {
            throw StoreError("cannot reopen compacted log: " + path_);
        }
    } catch (...) {
        if (out != nullptr) {
            std::fclose(out);
        }
        std::filesystem::remove(tmpPath);
        if (file_ == nullptr) {
            file_ = std::fopen(path_.c_str(), "r+b");
        }
        throw;
    }
    return remap;
}

ObjectId requireDocId(const Value& doc) {
    const Value* id = doc.asDocument().find("_id");
    if (id == nullptr || !id->is<ObjectId>()) {
        throw StoreError("document _id must be an ObjectId");
    }
    return id->get<ObjectId>();
}

ObjectId generateObjectId() {
    static std::atomic<uint32_t> counter{[] {
        std::random_device rd;
        return rd();
    }()};
    static const uint64_t randomBits = [] {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) | rd();
    }();

    ObjectId oid;
    auto secs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    oid.bytes[0] = static_cast<uint8_t>(secs >> 24);
    oid.bytes[1] = static_cast<uint8_t>(secs >> 16);
    oid.bytes[2] = static_cast<uint8_t>(secs >> 8);
    oid.bytes[3] = static_cast<uint8_t>(secs);
    for (int i = 0; i < 5; ++i) {
        oid.bytes[4 + i] = static_cast<uint8_t>(randomBits >> (8 * i));
    }
    uint32_t c = counter.fetch_add(1, std::memory_order_relaxed);
    oid.bytes[9] = static_cast<uint8_t>(c >> 16);
    oid.bytes[10] = static_cast<uint8_t>(c >> 8);
    oid.bytes[11] = static_cast<uint8_t>(c);
    return oid;
}

const Value* lookupPath(const Value& doc, const std::string& path) {
    const Value* cur = &doc;
    std::size_t start = 0;
    while (true) {
        if (!cur->is<Document>()) {
            return nullptr;
        }
        std::size_t dot = path.find('.', start);
        std::string_view segment(path.data() + start,
                                 (dot == std::string::npos ? path.size() : dot) - start);
        cur = cur->asDocument().find(segment);
        if (cur == nullptr || dot == std::string::npos) {
            return cur;
        }
        start = dot + 1;
    }
}

} // namespace bisondb::store
