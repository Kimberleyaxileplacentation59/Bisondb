#pragma once

#include "core/error.hpp"
#include "core/net/socket.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bisondb::client {

// An { ok:false } response from the server, carrying its error code.
class ServerError : public Error {
  public:
    ServerError(std::string code, const std::string& message)
        : Error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

struct FindOptions {
    std::size_t limit = 0;
    std::size_t skip = 0;
    // When true, return exactly one server response without following
    // truncated batches.
    bool singleBatch = false;
};

// One client = one connection = one outstanding request. NOT thread-safe:
// use one BisonClient per thread. Transport failures throw net::NetError;
// { ok:false } responses throw ServerError.
class BisonClient {
  public:
    static BisonClient connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    // Sends one request document and returns the (ok:true) response payload.
    Value command(Value request);

    void ping();
    Value serverStatus();
    std::vector<std::string> listCollections();
    bool dropCollection(const std::string& coll);
    std::vector<ObjectId> insert(const std::string& coll, const std::vector<Value>& documents);
    // Follows truncated responses (skipNext) unless options.singleBatch.
    std::vector<Value> find(const std::string& coll, const Value& filter,
                            const FindOptions& options = {});
    std::size_t deleteMany(const std::string& coll, const Value& filter);
    bool updateOne(const std::string& coll, const Value& filter, const Value& update);
    int64_t createIndex(const std::string& coll, const std::string& field);
    void dropIndex(const std::string& coll, const std::string& field);
    std::vector<std::string> listIndexes(const std::string& coll);
    Value explain(const std::string& coll, const Value& filter, std::size_t limit = 0);
    void compact(const std::string& coll);
    void shutdownServer();

  private:
    explicit BisonClient(net::TcpSocket socket) : socket_(std::move(socket)) {}

    net::TcpSocket socket_;
};

} // namespace bisondb::client
