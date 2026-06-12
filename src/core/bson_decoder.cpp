#include "core/bson_decoder.hpp"

#include "core/utf8.hpp"

#include <bit>
#include <limits>
#include <string>

namespace bisondb {

namespace {

using detail::isValidUtf8;

constexpr int kMaxDepth = 200;

// Bounds-checked cursor over the input span. Offsets in errors are absolute
// positions in the original input.
class Reader {
  public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    std::size_t offset() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return data_.size() - pos_; }

    [[noreturn]] void fail(const std::string& reason) const { fail(pos_, reason); }
    [[noreturn]] static void fail(std::size_t offset, const std::string& reason) {
        throw BsonParseError(offset, reason);
    }

    uint8_t readByte(const char* what) {
        require(1, what);
        return data_[pos_++];
    }

    int32_t readInt32(const char* what) {
        require(4, what);
        uint32_t u = static_cast<uint32_t>(data_[pos_]) |
                     (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                     (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                     (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return static_cast<int32_t>(u);
    }

    int64_t readInt64(const char* what) {
        require(8, what);
        uint64_t u = 0;
        for (int k = 7; k >= 0; --k) {
            u = (u << 8) | data_[pos_ + static_cast<std::size_t>(k)];
        }
        pos_ += 8;
        return static_cast<int64_t>(u);
    }

    double readDouble() {
        return std::bit_cast<double>(static_cast<uint64_t>(readInt64("double value")));
    }

    void readBytes(uint8_t* dst, std::size_t n, const char* what) {
        require(n, what);
        for (std::size_t k = 0; k < n; ++k) {
            dst[k] = data_[pos_ + k];
        }
        pos_ += n;
    }

    // NUL-terminated key string. The terminator must appear before `limit`.
    std::string readCString(std::size_t limit) {
        std::size_t start = pos_;
        while (pos_ < limit) {
            if (data_[pos_] == 0) {
                std::string s(reinterpret_cast<const char*>(data_.data() + start), pos_ - start);
                ++pos_;
                if (!isValidUtf8(s)) {
                    fail(start, "element key is not valid UTF-8");
                }
                return s;
            }
            ++pos_;
        }
        fail(start, "unterminated cstring key");
    }

  private:
    void require(std::size_t n, const char* what) const {
        if (remaining() < n) {
            fail(pos_, std::string("unexpected end of input reading ") + what);
        }
    }

    std::span<const uint8_t> data_;
    std::size_t pos_ = 0;
};

Value decodeDocumentBody(Reader& r, int depth, bool asArray);

Value decodeElementValue(Reader& r, uint8_t typeByte, std::size_t typeOffset, std::size_t docEnd,
                         int depth) {
    switch (typeByte) {
    case 0x01: return Value(r.readDouble());
    case 0x02: {
        std::size_t lenOffset = r.offset();
        int32_t len = r.readInt32("string length");
        if (len < 1) {
            Reader::fail(lenOffset, "string length must be >= 1, got " + std::to_string(len));
        }
        std::size_t n = static_cast<std::size_t>(len);
        if (r.offset() + n > docEnd) {
            Reader::fail(lenOffset, "string extends past end of document");
        }
        std::string s(n - 1, '\0');
        if (n > 1) {
            r.readBytes(reinterpret_cast<uint8_t*>(s.data()), n - 1, "string bytes");
        }
        if (r.readByte("string terminator") != 0) {
            Reader::fail(r.offset() - 1, "string is not NUL-terminated");
        }
        if (!isValidUtf8(s)) {
            Reader::fail(lenOffset + 4, "string is not valid UTF-8");
        }
        return Value(std::move(s));
    }
    case 0x03: return decodeDocumentBody(r, depth + 1, /*asArray=*/false);
    case 0x04: return decodeDocumentBody(r, depth + 1, /*asArray=*/true);
    case 0x07: {
        ObjectId oid;
        r.readBytes(oid.bytes.data(), oid.bytes.size(), "ObjectId");
        return Value(oid);
    }
    case 0x08: {
        std::size_t at = r.offset();
        uint8_t b = r.readByte("bool value");
        if (b > 1) {
            Reader::fail(at, "bool byte must be 0 or 1, got " + std::to_string(b));
        }
        return Value(b == 1);
    }
    case 0x09: return Value(DateTime{r.readInt64("DateTime value")});
    case 0x0A: return Value();
    case 0x10: return Value(r.readInt32("Int32 value"));
    case 0x12: return Value(r.readInt64("Int64 value"));
    case 0x13: {
        Decimal128 d;
        r.readBytes(d.bytes.data(), d.bytes.size(), "Decimal128");
        return Value(d);
    }
    default: {
        static constexpr char hex[] = "0123456789abcdef";
        Reader::fail(typeOffset, std::string("unknown BSON type byte 0x") + hex[typeByte >> 4] +
                                     hex[typeByte & 0x0F]);
    }
    }
}

Value decodeDocumentBody(Reader& r, int depth, bool asArray) {
    if (depth >= kMaxDepth) {
        r.fail("nesting depth exceeds limit of " + std::to_string(kMaxDepth));
    }
    std::size_t docStart = r.offset();
    int32_t totalSize = r.readInt32("document size");
    if (totalSize < 5) {
        Reader::fail(docStart, "document size must be >= 5, got " + std::to_string(totalSize));
    }
    if (static_cast<std::size_t>(totalSize) > r.remaining() + 4) {
        Reader::fail(docStart,
                     "document size " + std::to_string(totalSize) + " exceeds available bytes");
    }
    std::size_t docEnd = docStart + static_cast<std::size_t>(totalSize);

    Document doc;
    Array arr;
    // Elements must fill [docStart+4, docEnd-1) exactly, then the terminator.
    while (r.offset() < docEnd - 1) {
        std::size_t typeOffset = r.offset();
        uint8_t typeByte = r.readByte("element type");
        if (typeByte == 0) {
            Reader::fail(typeOffset, "premature document terminator before declared size");
        }
        std::string key = r.readCString(docEnd - 1);
        Value v = decodeElementValue(r, typeByte, typeOffset, docEnd, depth);
        if (r.offset() > docEnd - 1) {
            Reader::fail(typeOffset, "element extends past declared document size");
        }
        if (asArray) {
            // Array index keys ("0", "1", ...) are regenerated on encode; the
            // stored keys are intentionally ignored here.
            arr.push_back(std::move(v));
        } else {
            doc.append(std::move(key), std::move(v));
        }
    }
    if (r.readByte("document terminator") != 0) {
        Reader::fail(docEnd - 1, "missing document terminator");
    }
    if (asArray) {
        return Value(std::move(arr));
    }
    return Value(std::move(doc));
}

} // namespace

Value decodeDocument(std::span<const uint8_t> data) {
    DecodeResult res = decodeOne(data);
    if (res.bytesConsumed != data.size()) {
        throw BsonParseError(res.bytesConsumed, "trailing bytes after document");
    }
    return std::move(res.document);
}

DecodeResult decodeOne(std::span<const uint8_t> data) {
    Reader r(data);
    Value doc = decodeDocumentBody(r, 0, /*asArray=*/false);
    return DecodeResult{std::move(doc), r.offset()};
}

} // namespace bisondb
