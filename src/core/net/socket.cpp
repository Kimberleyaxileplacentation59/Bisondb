#include "core/net/socket.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>

#if defined(BISONDB_PLATFORM_WINDOWS)
// clang-format off
    #include <winsock2.h>
    #include <ws2tcpip.h>
// clang-format on
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace bisondb::net {

namespace {

#if defined(BISONDB_PLATFORM_WINDOWS)

using OsSocket = SOCKET;
using SockLen = int;
OsSocket osSock(NativeSocket s) {
    return static_cast<SOCKET>(s);
}

// Process-wide Winsock guard: WSAStartup on first socket use, WSACleanup at
// process exit (static destruction).
void ensureSocketsInit() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        static struct WsaGuard {
            WsaGuard() {
                WSADATA data;
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                    throw NetError(NetError::Kind::OsError, "WSAStartup failed");
                }
            }
            ~WsaGuard() { WSACleanup(); }
        } guard;
    });
}

int lastError() {
    return WSAGetLastError();
}
bool errIsTimeout(int code) {
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
}
bool errIsClosed(int code) {
    return code == WSAECONNRESET || code == WSAECONNABORTED || code == WSAESHUTDOWN ||
           code == WSAENOTSOCK || code == WSAEINTR;
}

std::string errorMessage(int code) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = n > 0 ? std::string(buf, n) : "unknown error";
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

void closeNative(NativeSocket s) {
    closesocket(osSock(s));
}

#else

using OsSocket = int;
using SockLen = socklen_t;
OsSocket osSock(NativeSocket s) {
    return s;
}
void ensureSocketsInit() {}
int lastError() {
    return errno;
}
bool errIsTimeout(int code) {
    return code == EAGAIN || code == EWOULDBLOCK;
}
bool errIsClosed(int code) {
    return code == ECONNRESET || code == EPIPE || code == EBADF || code == EINTR;
}
std::string errorMessage(int code) {
    return std::strerror(code);
}
void closeNative(NativeSocket s) {
    ::close(s);
}

#endif

// The single place mapping platform error codes (WSAGetLastError vs errno)
// onto NetError kinds.
[[noreturn]] void throwOsError(const std::string& what) {
    int code = lastError();
    NetError::Kind kind = NetError::Kind::OsError;
    if (errIsTimeout(code)) {
        kind = NetError::Kind::Timeout;
    } else if (errIsClosed(code)) {
        kind = NetError::Kind::Closed;
    }
    throw NetError(kind, what + ": " + errorMessage(code) + " (" + std::to_string(code) + ")");
}

void setBlocking(NativeSocket s, bool blocking) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    u_long mode = blocking ? 0 : 1;
    if (ioctlsocket(osSock(s), FIONBIO, &mode) != 0) {
        throwOsError("ioctlsocket(FIONBIO)");
    }
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) < 0) {
        throwOsError("fcntl(O_NONBLOCK)");
    }
#endif
}

} // namespace

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocket;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidSocket;
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (handle_ != kInvalidSocket) {
        closeNative(handle_);
        handle_ = kInvalidSocket;
    }
}

void TcpSocket::shutdownBoth() noexcept {
    if (handle_ != kInvalidSocket) {
#if defined(BISONDB_PLATFORM_WINDOWS)
        shutdown(osSock(handle_), SD_BOTH);
#else
        shutdown(osSock(handle_), SHUT_RDWR);
#endif
    }
}

void TcpSocket::sendAll(std::span<const uint8_t> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = send(osSock(handle_), reinterpret_cast<const char*>(data.data() + sent),
                      static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            throwOsError("send");
        }
        sent += static_cast<std::size_t>(n);
    }
}

RecvStatus TcpSocket::recvExact(std::span<uint8_t> buf) {
    std::size_t got = 0;
    while (got < buf.size()) {
        auto n = recv(osSock(handle_), reinterpret_cast<char*>(buf.data() + got),
                      static_cast<int>(buf.size() - got), 0);
        if (n == 0) {
            if (got == 0) {
                return RecvStatus::Closed;
            }
            throw NetError(NetError::Kind::Closed, "connection closed mid-message (" +
                                                       std::to_string(got) + "/" +
                                                       std::to_string(buf.size()) + " bytes)");
        }
        if (n < 0) {
            throwOsError("recv");
        }
        got += static_cast<std::size_t>(n);
    }
    return RecvStatus::Complete;
}

void TcpSocket::setRecvTimeout(int milliseconds) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    DWORD value = static_cast<DWORD>(milliseconds);
    if (setsockopt(osSock(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        throwOsError("setsockopt(SO_RCVTIMEO)");
    }
#else
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    if (setsockopt(osSock(handle_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        throwOsError("setsockopt(SO_RCVTIMEO)");
    }
#endif
}

void TcpSocket::setNoDelay(bool on) {
    int value = on ? 1 : 0;
    if (setsockopt(osSock(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        throwOsError("setsockopt(TCP_NODELAY)");
    }
}

bool TcpSocket::isLoopbackPeer() const {
    sockaddr_in addr{};
    SockLen len = sizeof(addr);
    if (getpeername(osSock(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return false;
    }
    if (addr.sin_family != AF_INET) {
        return false;
    }
    uint32_t host = ntohl(addr.sin_addr.s_addr);
    return (host >> 24) == 127; // 127.0.0.0/8
}

TcpListener::TcpListener(const std::string& bindAddress, uint16_t port, int backlog) {
    ensureSocketsInit();
    handle_ = static_cast<NativeSocket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (handle_ == kInvalidSocket) {
        throwOsError("socket");
    }
#if !defined(BISONDB_PLATFORM_WINDOWS)
    // POSIX: allow rebinding a TIME_WAIT port. Deliberately NOT set on
    // Windows, where SO_REUSEADDR instead allows stealing a port that
    // another process is actively listening on.
    int one = 1;
    setsockopt(osSock(handle_), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        close();
        throw NetError(NetError::Kind::OsError, "invalid bind address: " + bindAddress);
    }
    if (bind(osSock(handle_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int code = lastError();
        close();
        throw NetError(NetError::Kind::OsError, "bind " + bindAddress + ":" + std::to_string(port) +
                                                    ": " + errorMessage(code));
    }
    if (listen(osSock(handle_), backlog) != 0) {
        int code = lastError();
        close();
        throw NetError(NetError::Kind::OsError, "listen: " + errorMessage(code));
    }
    sockaddr_in bound{};
    SockLen len = sizeof(bound);
    getsockname(osSock(handle_), reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
}

TcpListener::~TcpListener() {
    close();
}

void TcpListener::close() noexcept {
    if (handle_ != kInvalidSocket) {
        closeNative(handle_);
        handle_ = kInvalidSocket;
    }
}

TcpSocket TcpListener::accept() {
    NativeSocket s = static_cast<NativeSocket>(::accept(osSock(handle_), nullptr, nullptr));
    if (s == kInvalidSocket) {
        int code = lastError();
        if (handle_ == kInvalidSocket || errIsClosed(code)) {
            throw NetError(NetError::Kind::Closed, "listener closed");
        }
        throw NetError(NetError::Kind::OsError, "accept: " + errorMessage(code));
    }
    TcpSocket sock(s);
    sock.setNoDelay(true);
    return sock;
}

TcpSocket connectTcp(const std::string& host, uint16_t port, int timeoutMs) {
    ensureSocketsInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results) != 0 ||
        results == nullptr) {
        throw NetError(NetError::Kind::OsError, "cannot resolve host: " + host);
    }

    std::string lastMessage = "no addresses";
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        NativeSocket raw =
            static_cast<NativeSocket>(socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (raw == kInvalidSocket) {
            continue;
        }
        TcpSocket sock(raw);
        try {
            // Non-blocking connect bounded by select(): the portable way to
            // get a per-attempt connect timeout.
            setBlocking(raw, false);
            int rc = connect(osSock(raw), it->ai_addr, static_cast<SockLen>(it->ai_addrlen));
            if (rc != 0) {
                int code = lastError();
#if defined(BISONDB_PLATFORM_WINDOWS)
                bool inProgress = code == WSAEWOULDBLOCK;
#else
                bool inProgress = code == EINPROGRESS;
#endif
                if (!inProgress) {
                    throw NetError(NetError::Kind::OsError, "connect: " + errorMessage(code));
                }
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(osSock(raw), &writeSet);
                timeval tv{};
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                int ready = select(static_cast<int>(raw) + 1, nullptr, &writeSet, nullptr, &tv);
                if (ready <= 0) {
                    throw NetError(NetError::Kind::Timeout,
                                   "connect timed out after " + std::to_string(timeoutMs) + "ms");
                }
                int soErr = 0;
                SockLen optLen = sizeof(soErr);
                getsockopt(osSock(raw), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr),
                           &optLen);
                if (soErr != 0) {
                    throw NetError(NetError::Kind::OsError, "connect: " + errorMessage(soErr));
                }
            }
            setBlocking(raw, true);
            sock.setNoDelay(true);
            freeaddrinfo(results);
            return sock;
        } catch (const NetError& e) {
            lastMessage = e.what();
        }
    }
    freeaddrinfo(results);
    throw NetError(NetError::Kind::OsError,
                   "cannot connect to " + host + ":" + std::to_string(port) + ": " + lastMessage);
}

} // namespace bisondb::net
