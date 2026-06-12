#pragma once

#include "core/error.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bisondb {

// BSON type tags. The enumerator values are the BSON wire-format type bytes.
enum class Type : uint8_t {
    Double = 0x01,
    String = 0x02,
    Document = 0x03,
    Array = 0x04,
    ObjectId = 0x07,
    Bool = 0x08,
    DateTime = 0x09,
    Null = 0x0A,
    Int32 = 0x10,
    Int64 = 0x12,
    Decimal128 = 0x13,
};

std::string_view typeName(Type t);

struct ObjectId {
    std::array<uint8_t, 12> bytes{};

    std::string toHex() const;
    // Throws TypeError unless the input is exactly 24 hex digits.
    static ObjectId fromHex(std::string_view hex);

    bool operator==(const ObjectId&) const = default;
};

struct DateTime {
    int64_t msSinceEpoch = 0;

    bool operator==(const DateTime&) const = default;
};

// IEEE 754-2008 decimal128, stored as the raw 16 little-endian bytes of the
// wire representation. No arithmetic is provided; see decimal128.hpp for
// string conversion.
struct Decimal128 {
    std::array<uint8_t, 16> bytes{};

    bool operator==(const Decimal128&) const = default;
};

class Value;

// BSON arrays are plain sequences of values.
using Array = std::vector<Value>;

// An ordered key -> Value map. BSON preserves insertion order, so this is a
// vector of pairs with linear find-by-key, not a sorted map.
class Document {
  public:
    using Entry = std::pair<std::string, Value>;

    Document() = default;
    Document(std::initializer_list<Entry> init);

    void append(std::string key, Value value);

    // Returns nullptr when the key is absent. First match wins if a key is
    // (pathologically) duplicated.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);

    bool contains(std::string_view key) const { return find(key) != nullptr; }

    std::size_t size() const noexcept { return fields_.size(); }
    bool empty() const noexcept { return fields_.empty(); }

    auto begin() noexcept { return fields_.begin(); }
    auto begin() const noexcept { return fields_.begin(); }
    auto end() noexcept { return fields_.end(); }
    auto end() const noexcept { return fields_.end(); }

    Entry& operator[](std::size_t i) { return fields_[i]; }
    const Entry& operator[](std::size_t i) const { return fields_[i]; }

    bool operator==(const Document&) const;

  private:
    std::vector<Entry> fields_;
};

// A single BSON value of any supported type.
class Value {
  public:
    Value() noexcept : v_(std::monostate{}) {}
    Value(std::monostate) noexcept : v_(std::monostate{}) {}
    Value(double d) noexcept : v_(d) {}
    Value(std::string s) noexcept : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(Document d) noexcept : v_(std::move(d)) {}
    Value(Array a) noexcept : v_(std::move(a)) {}
    Value(ObjectId oid) noexcept : v_(oid) {}
    Value(bool b) noexcept : v_(b) {}
    Value(DateTime dt) noexcept : v_(dt) {}
    Value(int32_t i) noexcept : v_(i) {}
    Value(int64_t i) noexcept : v_(i) {}
    Value(Decimal128 d) noexcept : v_(d) {}

    Type type() const noexcept;

    template <typename T> bool is() const noexcept { return std::holds_alternative<T>(v_); }

    template <typename T> const T& get() const {
        if (const T* p = std::get_if<T>(&v_)) {
            return *p;
        }
        throw TypeError(std::string("Value is ") + std::string(typeName(type())) +
                        ", not the requested type");
    }

    template <typename T> T& get() {
        if (T* p = std::get_if<T>(&v_)) {
            return *p;
        }
        throw TypeError(std::string("Value is ") + std::string(typeName(type())) +
                        ", not the requested type");
    }

    const Document& asDocument() const { return get<Document>(); }
    Document& asDocument() { return get<Document>(); }
    const Array& asArray() const { return get<Array>(); }
    Array& asArray() { return get<Array>(); }

    bool isNull() const noexcept { return is<std::monostate>(); }

    bool operator==(const Value&) const = default;

  private:
    // Alternative order is arbitrary but fixed; type() maps it to Type.
    using Variant = std::variant<std::monostate, double, std::string, Document, Array, ObjectId,
                                 bool, DateTime, int32_t, int64_t, Decimal128>;

    Variant v_;
};

inline Document::Document(std::initializer_list<Entry> init) : fields_(init) {}

inline void Document::append(std::string key, Value value) {
    fields_.emplace_back(std::move(key), std::move(value));
}

inline bool Document::operator==(const Document& other) const {
    return fields_ == other.fields_;
}

} // namespace bisondb
