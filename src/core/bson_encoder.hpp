#pragma once

#include "core/value.hpp"

#include <cstdint>
#include <vector>

namespace bisondb {

// Encodes a Document value to BSON wire format. Throws TypeError if `doc` is
// not a Document, and EncodeError for unencodable content (keys containing
// NUL bytes, nesting deeper than the decoder would accept).
std::vector<uint8_t> encodeDocument(const Value& doc);

} // namespace bisondb
