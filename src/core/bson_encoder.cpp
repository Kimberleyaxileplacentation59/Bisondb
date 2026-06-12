#include "core/bson_encoder.hpp"

#include <bit>
#include <string>
#include <string_view>

namespace bisondb {

namespace {

constexpr int kMaxDepth = 200;

void appendInt32(std::vector<uint8_t>& out, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    out.push_back(static_cast<uint8_t>(u));
    out.push_back(static_cast<uint8_t>(u >> 8));
    out.push_back(static_cast<uint8_t>(u >> 16));
    out.push_back(static_cast<uint8_t>(u >> 24));
}

void appendInt64(std::vector<uint8_t>& out, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int k = 0; k < 8; ++k) {
        out.push_back(static_cast<uint8_t>(u >> (8 * k)));
    }
}

void patchInt32(std::vector<uint8_t>& out, std::size_t at, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    out[at] = static_cast<uint8_t>(u);
    out[at + 1] = static_cast<uint8_t>(u >> 8);
    out[at + 2] = static_cast<uint8_t>(u >> 16);
    out[at + 3] = static_cast<uint8_t>(u >> 24);
}

void appendKey(std::vector<uint8_t>& out, uint8_t typeByte, std::string_view key) {
    if (key.find('\0') != std::string_view::npos) {
        throw EncodeError("document key contains an embedded NUL byte");
    }
    out.push_back(typeByte);
    out.insert(out.end(), key.begin(), key.end());
    out.push_back(0);
}

void encodeDocumentInto(std::vector<uint8_t>& out, const Value& v, int depth);

void encodeElement(std::vector<uint8_t>& out, std::string_view key, const Value& v, int depth) {
    appendKey(out, static_cast<uint8_t>(v.type()), key);
    switch (v.type()) {
    case Type::Double:
        appendInt64(out, static_cast<int64_t>(std::bit_cast<uint64_t>(v.get<double>())));
        break;
    case Type::String: {
        const std::string& s = v.get<std::string>();
        appendInt32(out, static_cast<int32_t>(s.size() + 1));
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
        break;
    }
    case Type::Document:
    case Type::Array: encodeDocumentInto(out, v, depth + 1); break;
    case Type::ObjectId: {
        const auto& bytes = v.get<ObjectId>().bytes;
        out.insert(out.end(), bytes.begin(), bytes.end());
        break;
    }
    case Type::Bool: out.push_back(v.get<bool>() ? 1 : 0); break;
    case Type::DateTime: appendInt64(out, v.get<DateTime>().msSinceEpoch); break;
    case Type::Null: break;
    case Type::Int32: appendInt32(out, v.get<int32_t>()); break;
    case Type::Int64: appendInt64(out, v.get<int64_t>()); break;
    case Type::Decimal128: {
        const auto& bytes = v.get<Decimal128>().bytes;
        out.insert(out.end(), bytes.begin(), bytes.end());
        break;
    }
    }
}

void encodeDocumentInto(std::vector<uint8_t>& out, const Value& v, int depth) {
    if (depth >= kMaxDepth) {
        throw EncodeError("nesting depth exceeds limit of " + std::to_string(kMaxDepth));
    }
    std::size_t sizeAt = out.size();
    appendInt32(out, 0); // placeholder, backpatched below
    if (v.is<Document>()) {
        for (const auto& [key, field] : v.asDocument()) {
            encodeElement(out, key, field, depth);
        }
    } else {
        const Array& arr = v.asArray();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            encodeElement(out, std::to_string(i), arr[i], depth);
        }
    }
    out.push_back(0);
    patchInt32(out, sizeAt, static_cast<int32_t>(out.size() - sizeAt));
}

} // namespace

std::vector<uint8_t> encodeDocument(const Value& doc) {
    if (!doc.is<Document>()) {
        throw TypeError("encodeDocument requires a Document value, got " +
                        std::string(typeName(doc.type())));
    }
    std::vector<uint8_t> out;
    encodeDocumentInto(out, doc, 0);
    return out;
}

} // namespace bisondb
