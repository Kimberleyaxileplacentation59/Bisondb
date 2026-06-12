#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace bisondb {

class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Thrown by Value::get<T>() and the asDocument()/asArray() accessors on type mismatch.
class TypeError : public Error {
  public:
    using Error::Error;
};

// Thrown by the BSON decoder. Carries the byte offset where decoding failed.
class BsonParseError : public Error {
  public:
    BsonParseError(std::size_t offset, const std::string& reason)
        : Error("BSON parse error at byte " + std::to_string(offset) + ": " + reason),
          offset_(offset) {}

    std::size_t offset() const noexcept { return offset_; }

  private:
    std::size_t offset_;
};

// Thrown by the BSON encoder (e.g. keys with embedded NUL bytes, excessive nesting).
class EncodeError : public Error {
  public:
    using Error::Error;
};

// Thrown by the JSON parser. Carries the 1-based line and column of the failure.
class JsonParseError : public Error {
  public:
    JsonParseError(std::size_t line, std::size_t column, const std::string& reason)
        : Error("JSON parse error at line " + std::to_string(line) + ", column " +
                std::to_string(column) + ": " + reason),
          line_(line), column_(column) {}

    std::size_t line() const noexcept { return line_; }
    std::size_t column() const noexcept { return column_; }

  private:
    std::size_t line_;
    std::size_t column_;
};

} // namespace bisondb
