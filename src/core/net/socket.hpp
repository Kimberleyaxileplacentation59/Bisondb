#pragma once

#include "core/error.hpp"
#include "core/platform.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace bisondb::net {

// All platform #ifdefs for sockets live in this header and socket.cpp;
// application code must never touch Winsock/POSIX APIs directly.
#if defined(BISONDB_PLATFORM_WINDOWS)
using NativeSocket = std::uintptr_t; // SOCKET
inline constexpr NativeSocket kInvalidSocket = ~static_cast<NativeSocket>(0);
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
#endif

class NetError : public Error {
  public:
    enum class Kind { Closed, Timeout, OsError };

    NetError(Kind kind, const std::string& message) : Error(message), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

  private:
    Kind kind_;
};

enum class RecvStatus {
    Complete,
    // Orderly peer close before ANY byte of this read — a clean disconnect
    // between requests, not an error. A close mid-buffer still throws.
    Closed,
};

// RAII TCP socket: non-copyable, movable, closes on destruction.
class TcpSocket {
  public:
    TcpSocket() = default;
    explicit TcpSocket(NativeSocket handle) : handle_(handle) {}
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    bool valid() const noexcept { return handle_ != kInvalidSocket; }
    NativeSocket native() const noexcept { return handle_; }

    // Loops until every byte is written; throws NetError on failure.
    void sendAll(std::span<const uint8_t> data);

    // Loops until the buffer is full. Returns RecvStatus::Closed when the
    // peer closed cleanly before any byte arrived; throws NetError for
    // timeouts, mid-message closes, and OS errors.
    RecvStatus recvExact(std::span<uint8_t> buf);

    // 0 disables the timeout (blocking reads).
    void setRecvTimeout(int milliseconds);
    void setNoDelay(bool on);
    void shutdownBoth() noexcept;
    void close() noexcept;

    bool isLoopbackPeer() const;

  private:
    NativeSocket handle_ = kInvalidSocket;
};

// Listening socket. IPv4 today; the address parameter keeps the API ready
// for IPv6. SO_REUSEADDR is set on POSIX only: there it merely allows reuse
// of a TIME_WAIT port, while on Windows it permits hijacking a port another
// process is actively listening on.
class TcpListener {
  public:
    // bindAddress like "127.0.0.1" or "0.0.0.0"; port 0 picks an ephemeral
    // port (read back via port()).
    TcpListener(const std::string& bindAddress, uint16_t port, int backlog = 64);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    uint16_t port() const noexcept { return port_; }

    // Blocks. Throws NetError(Closed) when the listener was close()d (the
    // graceful-shutdown path) and NetError(OsError) otherwise.
    TcpSocket accept();

    // Unblocks a pending accept(); safe to call from another thread.
    void close() noexcept;

  private:
    NativeSocket handle_ = kInvalidSocket;
    uint16_t port_ = 0;
};

// Resolves host (name or numeric) with getaddrinfo, tries each result, and
// enforces a per-attempt connect timeout. TCP_NODELAY is enabled.
TcpSocket connectTcp(const std::string& host, uint16_t port, int timeoutMs);

} // namespace bisondb::net
