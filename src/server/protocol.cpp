#include "server/protocol.hpp"

#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"

#include <vector>

namespace bisondb::server {

std::optional<Value> readFrame(net::TcpSocket& socket, std::size_t maxMessageSize) {
    uint8_t lenBytes[4];
    if (socket.recvExact(lenBytes) == net::RecvStatus::Closed) {
        return std::nullopt;
    }
    uint32_t len = static_cast<uint32_t>(lenBytes[0]) | (static_cast<uint32_t>(lenBytes[1]) << 8) |
                   (static_cast<uint32_t>(lenBytes[2]) << 16) |
                   (static_cast<uint32_t>(lenBytes[3]) << 24);
    if (len < kMinMessageSize || len > maxMessageSize) {
        throw FrameError("frame length " + std::to_string(len) + " outside [" +
                         std::to_string(kMinMessageSize) + ", " +
                         std::to_string(maxMessageSize) + "]");
    }
    std::vector<uint8_t> payload(len);
    if (socket.recvExact(payload) == net::RecvStatus::Closed) {
        throw net::NetError(net::NetError::Kind::Closed, "connection closed mid-frame");
    }
    return decodeDocument(payload);
}

void writeFrame(net::TcpSocket& socket, const Value& document, std::size_t maxMessageSize) {
    std::vector<uint8_t> payload = encodeDocument(document);
    if (payload.size() > maxMessageSize) {
        throw FrameError("response of " + std::to_string(payload.size()) +
                         " bytes exceeds the message cap");
    }
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint8_t lenBytes[4] = {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                           static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24)};
    socket.sendAll(lenBytes);
    socket.sendAll(payload);
}

} // namespace bisondb::server
