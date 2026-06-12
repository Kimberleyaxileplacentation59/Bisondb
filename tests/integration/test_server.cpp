// Integration tests: a real Server instance in-process on an ephemeral port,
// exercised through BisonClient and raw sockets.

#include "client/client.hpp"
#include "core/json_parser.hpp"
#include "server/server.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace bisondb;
using bisondb::client::BisonClient;

namespace {

namespace fs = std::filesystem;

struct ServerFixture {
    fs::path dir;
    std::unique_ptr<server::Server> srv;

    explicit ServerFixture(const std::string& name, std::size_t maxMessageSize = 0) {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("bisondb_it_" + name + "_" + std::to_string(rd()));
        fs::remove_all(dir);
        server::ServerConfig config;
        config.dir = dir.string();
        config.port = 0; // ephemeral
        config.threads = 16;
        config.quiet = true;
        if (maxMessageSize != 0) {
            config.maxMessageSize = maxMessageSize;
        }
        srv = std::make_unique<server::Server>(std::move(config));
        srv->start();
    }

    ~ServerFixture() {
        if (srv) {
            srv->stop();
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    BisonClient client() { return BisonClient::connect("127.0.0.1", srv->port(), 2000); }
};

} // namespace

TEST_CASE("end-to-end CRUD over the wire", "[integration]") {
    ServerFixture fx("crud");
    BisonClient c = fx.client();

    c.ping();
    Value status = c.serverStatus();
    REQUIRE(status.asDocument().find("name")->get<std::string>() == "BisonDB");

    std::vector<Value> docs;
    for (int i = 0; i < 50; ++i) {
        docs.push_back(parseJson(R"({"v": )" + std::to_string(i) + R"(, "tag": "x"})"));
    }
    auto ids = c.insert("things", docs);
    REQUIRE(ids.size() == 50);

    auto found = c.find("things", parseJson(R"({"v": {"$lt": 10}})"));
    REQUIRE(found.size() == 10);

    // Index changes the plan.
    Value before = c.explain("things", parseJson(R"({"v": {"$gte": 40}})"));
    REQUIRE(before.asDocument().find("plan")->get<std::string>() == "scan");
    REQUIRE(c.createIndex("things", "v") == 50);
    Value after = c.explain("things", parseJson(R"({"v": {"$gte": 40}})"));
    REQUIRE(after.asDocument().find("plan")->get<std::string>() == "index_range");
    REQUIRE(after.asDocument().find("docsExamined")->get<int64_t>() == 10);

    REQUIRE(c.updateOne("things", parseJson(R"({"v": 7})"),
                        parseJson(R"({"$set": {"tag": "seven"}})")));
    REQUIRE(c.find("things", parseJson(R"({"tag": "seven"})")).size() == 1);

    REQUIRE(c.deleteMany("things", parseJson(R"({"v": {"$lt": 25}})")) == 25);
    REQUIRE(c.find("things", parseJson("{}")).size() == 25);

    auto indexes = c.listIndexes("things");
    REQUIRE(indexes == std::vector<std::string>{"_id", "v"});
    auto collections = c.listCollections();
    REQUIRE(collections == std::vector<std::string>{"things"});
}

TEST_CASE("createCollection and dbStats", "[integration]") {
    ServerFixture fx("stats");
    BisonClient c = fx.client();

    REQUIRE(c.createCollection("empty"));
    REQUIRE_FALSE(c.createCollection("empty")); // already exists
    c.insert("filled", {parseJson(R"({"v": 1})"), parseJson(R"({"v": 2})")});
    c.createIndex("filled", "v");

    Value stats = c.dbStats();
    const Array& colls = stats.asDocument().find("collections")->asArray();
    REQUIRE(colls.size() == 2);
    for (const Value& entry : colls) {
        const Document& d = entry.asDocument();
        std::string name = d.find("name")->get<std::string>();
        int64_t count = d.find("count")->get<int64_t>();
        REQUIRE(d.find("fileSizeBytes")->get<int64_t>() > 0);
        const Array& indexes = d.find("indexes")->asArray();
        if (name == "empty") {
            REQUIRE(count == 0);
            REQUIRE(indexes.size() == 1); // _id only
        } else {
            REQUIRE(name == "filled");
            REQUIRE(count == 2);
            REQUIRE(indexes.size() == 2); // _id + v
        }
    }
    // Invalid names are rejected.
    try {
        c.createCollection("../evil");
        FAIL("expected ServerError");
    } catch (const client::ServerError& e) {
        REQUIRE(e.code() == "BadRequest");
    }
}

TEST_CASE("error responses carry codes and the connection stays usable", "[integration]") {
    ServerFixture fx("errors");
    BisonClient c = fx.client();

    try {
        c.command(parseJson(R"({"cmd": "nonsense"})"));
        FAIL("expected ServerError");
    } catch (const client::ServerError& e) {
        REQUIRE(e.code() == "UnknownCommand");
    }
    try {
        c.command(parseJson(R"({"cmd": "find", "coll": "x"})")); // missing filter
        FAIL("expected ServerError");
    } catch (const client::ServerError& e) {
        REQUIRE(e.code() == "BadRequest");
        REQUIRE(std::string(e.what()).find("filter") != std::string::npos);
    }
    try {
        c.command(parseJson(R"({"cmd": "insert", "coll": "../evil", "documents": [{}]})"));
        FAIL("expected ServerError");
    } catch (const client::ServerError& e) {
        REQUIRE(e.code() == "BadRequest");
    }
    c.ping(); // same connection still serves
}

TEST_CASE("duplicate _id maps to DuplicateKey", "[integration]") {
    ServerFixture fx("dup");
    BisonClient c = fx.client();
    Value doc = parseJson(R"({"_id": {"$oid": "507f1f77bcf86cd799439011"}, "a": 1})");
    c.insert("c", {doc});
    try {
        c.insert("c", {doc});
        FAIL("expected ServerError");
    } catch (const client::ServerError& e) {
        REQUIRE(e.code() == "DuplicateKey");
    }
}

TEST_CASE("truncated find responses are reassembled by the client", "[integration]") {
    // Shrunken message cap: ~64 KiB forces several batches.
    ServerFixture fx("trunc", 64 * 1024);
    BisonClient c = fx.client();

    std::string blob(1000, 'x');
    std::vector<Value> docs;
    for (int i = 0; i < 300; ++i) {
        docs.push_back(Value(Document{{"i", Value(int32_t{i})}, {"blob", Value(blob)}}));
    }
    // Insert in small batches to stay under the shrunken request cap too.
    for (std::size_t at = 0; at < docs.size(); at += 25) {
        std::vector<Value> batch(docs.begin() + static_cast<std::ptrdiff_t>(at),
                                 docs.begin() + static_cast<std::ptrdiff_t>(at + 25));
        c.insert("big", batch);
    }

    client::FindOptions single;
    single.singleBatch = true;
    auto oneBatch = c.find("big", parseJson("{}"), single);
    REQUIRE(oneBatch.size() < 300); // server really truncated

    auto all = c.find("big", parseJson("{}"));
    REQUIRE(all.size() == 300);
    std::set<int32_t> seen;
    for (const Value& d : all) {
        seen.insert(d.asDocument().find("i")->get<int32_t>());
    }
    REQUIRE(seen.size() == 300);

    client::FindOptions limited;
    limited.limit = 150;
    REQUIRE(c.find("big", parseJson("{}"), limited).size() == 150);
}

TEST_CASE("adversarial framing never kills the server", "[integration]") {
    ServerFixture fx("adversarial");

    auto rawSend = [&fx](const std::vector<uint8_t>& bytes) {
        net::TcpSocket s = net::connectTcp("127.0.0.1", fx.srv->port(), 2000);
        s.sendAll(bytes);
        return s;
    };
    auto le32 = [](uint32_t v) {
        return std::vector<uint8_t>{static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                                    static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    };

    SECTION("length prefix of zero") {
        net::TcpSocket s = rawSend(le32(0));
        s.setRecvTimeout(2000);
        std::vector<uint8_t> reply(4);
        // The server may reply with an error frame or just close.
        try {
            (void)s.recvExact(reply);
        } catch (const net::NetError&) {
        }
    }
    SECTION("length prefix of 17 MiB") {
        net::TcpSocket s = rawSend(le32(17 * 1024 * 1024));
        s.setRecvTimeout(2000);
        std::vector<uint8_t> reply(4);
        try {
            (void)s.recvExact(reply);
        } catch (const net::NetError&) {
        }
    }
    SECTION("valid length, garbage payload") {
        std::vector<uint8_t> frame = le32(16);
        for (int i = 0; i < 16; ++i) {
            frame.push_back(0xC7);
        }
        net::TcpSocket s = rawSend(frame);
        s.setRecvTimeout(2000);
        std::vector<uint8_t> lenBytes(4);
        REQUIRE(s.recvExact(lenBytes) == net::RecvStatus::Complete); // an error frame came back
    }
    SECTION("half a frame then close") {
        {
            net::TcpSocket s = rawSend({0x40, 0x00});
            s.shutdownBoth();
        }
    }

    // Whatever the attacker did, a healthy client still gets service.
    BisonClient healthy = fx.client();
    healthy.ping();
    healthy.insert("ok", {parseJson(R"({"fine": true})")});
    REQUIRE(healthy.find("ok", parseJson("{}")).size() == 1);
}

TEST_CASE("concurrency soak: 8 clients, mixed ops, oracle-verified", "[integration][soak]") {
    ServerFixture fx("soak");
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 1000;

    // Index the lookup key so each op is an index range instead of a full
    // collection scan: keeps the soak fast on slow debug STLs and stresses
    // concurrent index maintenance at the same time.
    fx.client().createIndex("soak", "k");

    // Each thread owns a disjoint integer key range; the oracle is merged at
    // the end (per-thread determinism, no cross-thread conflicts by design —
    // the server still interleaves them on shared collection structures).
    std::vector<std::map<int, int>> oracles(kThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&fx, &oracles, &failed, t] {
            try {
                BisonClient c = fx.client();
                std::mt19937 rng(static_cast<uint32_t>(1000 + t));
                std::map<int, int>& oracle = oracles[static_cast<std::size_t>(t)];
                int base = t * 100000;
                for (int op = 0; op < kOpsPerThread; ++op) {
                    int key = base + static_cast<int>(rng() % 500);
                    switch (rng() % 4) {
                    case 0:
                    case 1: { // upsert-ish: delete then insert
                        c.deleteMany("soak", Value(Document{{"k", Value(int32_t{key})}}));
                        int v = static_cast<int>(rng() % 1000000);
                        c.insert("soak", {Value(Document{{"k", Value(int32_t{key})},
                                                         {"v", Value(int32_t{v})}})});
                        oracle[key] = v;
                        break;
                    }
                    case 2: { // delete
                        c.deleteMany("soak", Value(Document{{"k", Value(int32_t{key})}}));
                        oracle.erase(key);
                        break;
                    }
                    default: { // read own range and spot-check
                        auto docs = c.find("soak", Value(Document{{"k", Value(int32_t{key})}}));
                        auto it = oracle.find(key);
                        if (it == oracle.end()) {
                            if (!docs.empty()) {
                                throw std::runtime_error("ghost document");
                            }
                        } else if (docs.size() != 1 ||
                                   docs[0].asDocument().find("v")->get<int32_t>() != it->second) {
                            throw std::runtime_error("oracle mismatch");
                        }
                        break;
                    }
                    }
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    REQUIRE_FALSE(failed.load());

    // Final state equals the merged oracle.
    BisonClient c = fx.client();
    auto all = c.find("soak", parseJson("{}"));
    std::map<int, int> merged;
    for (const auto& oracle : oracles) {
        merged.insert(oracle.begin(), oracle.end());
    }
    REQUIRE(all.size() == merged.size());
    for (const Value& d : all) {
        int k = d.asDocument().find("k")->get<int32_t>();
        REQUIRE(merged.at(k) == d.asDocument().find("v")->get<int32_t>());
    }
}

TEST_CASE("graceful shutdown leaves clean files", "[integration]") {
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("bisondb_it_shutdown_" + std::to_string(rd()));
    fs::remove_all(dir);
    {
        server::ServerConfig config;
        config.dir = dir.string();
        config.port = 0;
        config.threads = 8;
        config.quiet = true;
        server::Server srv(std::move(config));
        srv.start();

        BisonClient c = BisonClient::connect("127.0.0.1", srv.port(), 2000);
        std::vector<Value> docs;
        for (int i = 0; i < 200; ++i) {
            docs.push_back(Value(Document{{"i", Value(int32_t{i})}}));
        }
        c.insert("c", docs);
        c.createIndex("c", "i");
        c.shutdownServer(); // loopback peer: accepted
        srv.waitUntilStopped();
    }
    // Reopen: the _id and field indexes must load clean — RebuildRequired
    // would mean the shutdown did not sync.
    {
        btree::BTree idIdx((dir / "c._id.idx").string());
        REQUIRE(idIdx.wasCleanOnOpen());
        btree::BTree fieldIdx((dir / "c.i.idx").string());
        REQUIRE(fieldIdx.wasCleanOnOpen());
    }
    query::IndexedCollection coll(dir.string(), "c");
    REQUIRE(coll.count() == 200);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("shutdown command is refused off-loopback by design", "[integration]") {
    // All test connections are loopback, so just verify the command path
    // exists and works from loopback (the address check itself is unit-level
    // logic in TcpSocket::isLoopbackPeer).
    ServerFixture fx("shutdown_cmd");
    BisonClient c = fx.client();
    c.shutdownServer();
    fx.srv->stop();
}
