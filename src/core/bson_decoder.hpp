#pragma once

#include "core/value.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bisondb {

// Decodes a single BSON document that must occupy the entire span. Trailing
// bytes after the document are an error. Throws BsonParseError on any
// malformed input.
Value decodeDocument(std::span<const uint8_t> data);

struct DecodeResult {
    Value document;
    std::size_t bytesConsumed = 0;
};

// Streaming-friendly variant: decodes one document from the front of the span
// and reports how many bytes it consumed, so concatenated dump files can be
// read in a loop.
DecodeResult decodeOne(std::span<const uint8_t> data);

} // namespace bisondb
