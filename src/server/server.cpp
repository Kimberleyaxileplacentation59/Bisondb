#include "server/server.hpp"

#include "core/json_writer.hpp"

#include <chrono>
#include <iostream>

namespace bisondb::server {

namespace {

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return std::to_string(ms.count());
}

} // namespace

Server::Server(ServerConfig config) : config_(std::move(config)), db_(config_.dir) {}

Server::~Server() { stop(); }

void Server::log(const std::string& line) {
    if (!config_.quiet) {
        std::cerr << "[" << timestamp() << "] " << line << "\n";
    }
}

void Server::start() {
    listener_ = std::make_unique<net::TcpListener>(config_.bind, config_.port);
    port_ = listener_->port();
    std::size_t threads =
        config_.threads != 0 ? config_.threads : std::thread::hardware_concurrency();
    pool_ = std::make_unique<net::ThreadPool>(threads);
    acceptor_ = std::thread([this] { acceptLoop(); });
    log("info  bisond listening on " + config_.bind + ":" + std::to_string(port_) + " dir=" +
        config_.dir + " threads=" + std::to_string(threads));
}

void Server::acceptLoop() {
    while (!stopRequested_.load()) {
        net::TcpSocket sock;
        try {
            sock = listener_->accept();
        } catch (const net::NetError&) {
            break; // listener closed (shutdown) or fatal accept error
        }
        if (stopRequested_.load()) {
            break;
        }
        if (stats_.connectionsCurrent.load() >= config_.maxConnections) {
            try {
                Value busy(Document{
                    {"ok", Value(false)},
                    {"error", Value(Document{{"code", Value("ServerBusy")},
                                             {"message", Value("connection limit reached")}})}});
                writeFrame(sock, busy, config_.maxMessageSize);
            } catch (...) {
            }
            continue; // socket closes via RAII
        }
        uint64_t connId = nextConnId_.fetch_add(1);
        stats_.connectionsCurrent.fetch_add(1);
        // The pool owns the connection for its whole lifetime.
        auto shared = std::make_shared<net::TcpSocket>(std::move(sock));
        pool_->submit([this, shared, connId]() mutable {
            serveConnection(std::move(*shared), connId);
        });
    }
}

void Server::serveConnection(net::TcpSocket socket, uint64_t connId) {
    {
        std::lock_guard lock(connMutex_);
        connections_[connId] = &socket;
    }
    log("info  conn=" + std::to_string(connId) + " accepted");

    // Strictly sequential: read one request, write one response, repeat.
    while (!stopRequested_.load()) {
        std::optional<Value> request;
        try {
            request = readFrame(socket, config_.maxMessageSize);
        } catch (const FrameError& e) {
            // Byte stream out of sync: best-effort error frame, then close.
            try {
                Value err(Document{
                    {"ok", Value(false)},
                    {"error", Value(Document{{"code", Value("TooLarge")},
                                             {"message", Value(std::string(e.what()))}})}});
                writeFrame(socket, err, config_.maxMessageSize);
            } catch (...) {
            }
            break;
        } catch (const BsonParseError& e) {
            // Framed but malformed payload: report and keep serving.
            try {
                Value err(Document{
                    {"ok", Value(false)},
                    {"error", Value(Document{{"code", Value("ParseError")},
                                             {"message", Value(std::string(e.what()))}})}});
                writeFrame(socket, err, config_.maxMessageSize);
                continue;
            } catch (...) {
                break;
            }
        } catch (const net::NetError&) {
            break; // timeout / reset / closed mid-frame
        }
        if (!request.has_value()) {
            break; // clean disconnect between requests
        }

        auto begun = std::chrono::steady_clock::now();
        Value response = dispatchCommand(*this, *request, socket);
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - begun)
                              .count();
        std::string cmd = "?";
        if (request->is<Document>()) {
            if (const Value* c = request->asDocument().find("cmd"); c && c->is<std::string>()) {
                cmd = c->get<std::string>();
            }
        }
        log("info  conn=" + std::to_string(connId) + " cmd=" + cmd + " durationMs=" +
            std::to_string(durationMs));
        try {
            writeFrame(socket, response, config_.maxMessageSize);
        } catch (...) {
            break;
        }
    }

    {
        std::lock_guard lock(connMutex_);
        connections_.erase(connId);
    }
    stats_.connectionsCurrent.fetch_sub(1);
    log("info  conn=" + std::to_string(connId) + " closed");
}

void Server::requestStop() noexcept {
    bool expected = false;
    if (!stopRequested_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (listener_) {
        listener_->close(); // unblocks accept()
    }
    stopCv_.notify_all();
}

void Server::waitUntilStopped() {
    std::unique_lock lock(stopMutex_);
    stopCv_.wait(lock, [this] { return stopRequested_.load(); });
    lock.unlock();
    stop();
}

void Server::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    requestStop();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    // Grace period for in-flight responses, then shut idle readers down so
    // their blocking recv calls return.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard lock(connMutex_);
        for (auto& [id, sock] : connections_) {
            sock->shutdownBoth();
        }
    }
    if (pool_) {
        pool_->stop();
    }
    db_.syncAll();
    log("info  bisond stopped");
}

} // namespace bisondb::server
