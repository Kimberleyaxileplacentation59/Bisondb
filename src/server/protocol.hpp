#pragma once

#include "core/error.hpp"
#include "core/net/socket.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <optional>

namespace bisondb::server {

inline constexpr std::size_t kMaxMessageSize = 16 * 1024 * 1024; // 16 MiB
inline constexpr std::size_t kMinMessageSize = 5;                // empty BSON doc

// The frame itself is unusable (bad length prefix): the connection must be
// closed because the byte stream can no longer be trusted.
class FrameError : public Error {
  public:
    using Error::Error;
};

// Reads one `u32 LE length | BSON document` frame. Returns nullopt on a
// clean close between frames. Throws FrameError for length violations and
// BsonParseError for a well-framed but malformed payload (the stream stays
// in sync, so the connection can keep serving after an error response).
std::optional<Value> readFrame(net::TcpSocket& socket,
                               std::size_t maxMessageSize = kMaxMessageSize);

// Encodes and writes one frame; throws FrameError when the encoded document
// exceeds maxMessageSize.
void writeFrame(net::TcpSocket& socket, const Value& document,
                std::size_t maxMessageSize = kMaxMessageSize);

} // namespace bisondb::server
