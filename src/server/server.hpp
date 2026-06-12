#pragma once

#include "core/net/socket.hpp"
#include "core/net/thread_pool.hpp"
#include "core/query/database.hpp"
#include "server/protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bisondb::server {

struct ServerConfig {
    std::string dir;
    std::string bind = "127.0.0.1";
    uint16_t port = 27027; // 0 = ephemeral, read back via Server::port()
    std::size_t threads = 0; // 0 = hardware_concurrency
    std::size_t maxConnections = 64;
    std::size_t maxMessageSize = kMaxMessageSize; // shrinkable for tests
    bool quiet = false;
};

struct ServerStats {
    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::atomic<std::size_t> connectionsCurrent{0};
    std::mutex opMutex;
    std::map<std::string, uint64_t> opCounters;

    void countOp(const std::string& cmd) {
        std::lock_guard lock(opMutex);
        ++opCounters[cmd];
    }
};

// The bisond daemon as a library (main.cpp is a thin flag-parsing wrapper,
// so integration tests run the server in-process on an ephemeral port).
class Server {
  public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds the listener and starts the acceptor thread.
    void start();

    // Bound port (after start(); resolves ephemeral port 0).
    uint16_t port() const noexcept { return port_; }

    // Signal-handler-safe stop trigger: stops accepting and wakes the
    // shutdown path. The full drain happens in stop()/waitUntilStopped().
    void requestStop() noexcept;

    // Blocks until requestStop(), then drains and syncs (used by main()).
    void waitUntilStopped();

    // Full graceful shutdown: stop accepting, close idle connections after a
    // grace period, drain workers, sync all collections. Idempotent.
    void stop();

    query::Database& database() noexcept { return db_; }
    const ServerConfig& config() const noexcept { return config_; }
    ServerStats& stats() noexcept { return stats_; }

  private:
    void acceptLoop();
    void serveConnection(net::TcpSocket socket, uint64_t connId);
    void log(const std::string& line);

    ServerConfig config_;
    query::Database db_;
    ServerStats stats_;
    std::unique_ptr<net::TcpListener> listener_;
    std::unique_ptr<net::ThreadPool> pool_;
    std::thread acceptor_;
    uint16_t port_ = 0;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> stopped_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;

    // Live connections, for the forced-close pass during shutdown.
    std::mutex connMutex_;
    std::unordered_map<uint64_t, net::TcpSocket*> connections_;
    std::atomic<uint64_t> nextConnId_{1};
};

// Command dispatch (commands.cpp). Returns the response document; never
// throws — every engine exception is translated to an { ok:false, error }
// response in one place.
Value dispatchCommand(Server& server, const Value& request, const net::TcpSocket& peer);

} // namespace bisondb::server
