#include "core/value.hpp"

namespace bisondb {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string_view typeName(Type t) {
    switch (t) {
    case Type::Double: return "Double";
    case Type::String: return "String";
    case Type::Document: return "Document";
    case Type::Array: return "Array";
    case Type::ObjectId: return "ObjectId";
    case Type::Bool: return "Bool";
    case Type::DateTime: return "DateTime";
    case Type::Null: return "Null";
    case Type::Int32: return "Int32";
    case Type::Int64: return "Int64";
    case Type::Decimal128: return "Decimal128";
    }
    return "Unknown";
}

std::string ObjectId::toHex() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(24);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

ObjectId ObjectId::fromHex(std::string_view hex) {
    if (hex.size() != 24) {
        throw TypeError("ObjectId hex string must be exactly 24 characters");
    }
    ObjectId oid;
    for (std::size_t i = 0; i < 12; ++i) {
        int hi = hexDigit(hex[2 * i]);
        int lo = hexDigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            throw TypeError("ObjectId hex string contains a non-hex character");
        }
        oid.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return oid;
}

const Value* Document::find(std::string_view key) const {
    for (const Entry& e : fields_) {
        if (e.first == key) {
            return &e.second;
        }
    }
    return nullptr;
}

Value* Document::find(std::string_view key) {
    for (Entry& e : fields_) {
        if (e.first == key) {
            return &e.second;
        }
    }
    return nullptr;
}

Type Value::type() const noexcept {
    switch (v_.index()) {
    case 0: return Type::Null;
    case 1: return Type::Double;
    case 2: return Type::String;
    case 3: return Type::Document;
    case 4: return Type::Array;
    case 5: return Type::ObjectId;
    case 6: return Type::Bool;
    case 7: return Type::DateTime;
    case 8: return Type::Int32;
    case 9: return Type::Int64;
    default: return Type::Decimal128;
    }
}

} // namespace bisondb
