#include "core/bson_encoder.hpp"
#include "core/btree/btree.hpp"
#include "core/btree/key_codec.hpp"
#include "core/query/query.hpp"
#include "core/version.hpp"
#include "server/server.hpp"

#include <chrono>
#include <filesystem>
#include <functional>

namespace bisondb::server {

namespace {

class CommandError : public Error {
  public:
    CommandError(std::string code, const std::string& message)
        : Error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

Value okResponse(Document payload = {}) {
    Document d{{"ok", Value(true)}};
    for (auto& [k, v] : payload) {
        d.append(k, std::move(v));
    }
    return Value(std::move(d));
}

Value errorResponse(const std::string& code, const std::string& message) {
    return Value(
        Document{{"ok", Value(false)},
                 {"error", Value(Document{{"code", Value(code)}, {"message", Value(message)}})}});
}

// ---- argument validation -------------------------------------------------

const Value& require(const Document& req, const char* field) {
    const Value* v = req.find(field);
    if (v == nullptr) {
        throw CommandError("BadRequest", std::string("missing required field \"") + field + "\"");
    }
    return *v;
}

const std::string& requireString(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<std::string>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be a string");
    }
    return v.get<std::string>();
}

const Document& requireDoc(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<Document>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be a document");
    }
    return v.asDocument();
}

const Array& requireArray(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<Array>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be an array");
    }
    return v.asArray();
}

std::size_t optionalCount(const Document& req, const char* field, std::size_t fallback) {
    const Value* v = req.find(field);
    if (v == nullptr) {
        return fallback;
    }
    int64_t n = 0;
    if (v->is<int32_t>()) {
        n = v->get<int32_t>();
    } else if (v->is<int64_t>()) {
        n = v->get<int64_t>();
    } else {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be an integer");
    }
    if (n < 0) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be >= 0");
    }
    return static_cast<std::size_t>(n);
}

query::IndexedCollection& collOf(Server& server, const Document& req) {
    return server.database().collection(requireString(req, "coll"));
}

// ---- handlers --------------------------------------------------------------

Value cmdInsert(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    const Array& docs = requireArray(req, "documents");
    if (docs.empty()) {
        throw CommandError("BadRequest", "\"documents\" must not be empty");
    }
    Array insertedIds;
    for (const Value& doc : docs) {
        if (!doc.is<Document>()) {
            throw CommandError("BadRequest", "\"documents\" entries must be documents");
        }
        insertedIds.push_back(Value(coll.insert(doc)));
    }
    return okResponse(Document{{"insertedIds", Value(std::move(insertedIds))},
                               {"insertedCount", Value(static_cast<int64_t>(docs.size()))}});
}

Value cmdFind(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    query::FindOptions opts;
    opts.limit = optionalCount(req, "limit", 0);
    opts.skip = optionalCount(req, "skip", 0);
    const Document& filter = requireDoc(req, "filter");
    std::vector<Value> docs = engine.find(Value(filter), opts);

    // One response must fit in maxMessageSize: include documents while a
    // byte budget lasts and tell the client where to resume (skipNext) when
    // truncated. Real cursors are out of scope (documented in protocol.md).
    std::size_t budget = server.config().maxMessageSize - 4096;
    Array included;
    std::size_t usedBytes = 0;
    bool truncated = false;
    for (Value& doc : docs) {
        std::size_t size = encodeDocument(doc).size();
        if (!included.empty() && usedBytes + size > budget) {
            truncated = true;
            break;
        }
        usedBytes += size;
        included.push_back(std::move(doc));
    }
    Document payload{{"documents", Value(std::move(included))}};
    std::size_t count = payload.find("documents")->asArray().size();
    payload.append("count", Value(static_cast<int64_t>(count)));
    if (truncated) {
        payload.append("truncated", Value(true));
        payload.append("skipNext", Value(static_cast<int64_t>(opts.skip + count)));
    }
    return okResponse(std::move(payload));
}

Value cmdDeleteMany(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    std::size_t n = engine.deleteMany(Value(requireDoc(req, "filter")));
    return okResponse(Document{{"deletedCount", Value(static_cast<int64_t>(n))}});
}

Value cmdUpdateOne(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    bool matched =
        engine.updateOne(Value(requireDoc(req, "filter")), Value(requireDoc(req, "update")));
    return okResponse(Document{{"matched", Value(matched)}, {"modified", Value(matched)}});
}

Value cmdCreateIndex(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::IndexBuildStats stats = coll.createIndex(requireString(req, "field"));
    return okResponse(Document{{"built", Value(true)},
                               {"docsIndexed", Value(static_cast<int64_t>(stats.indexed))},
                               {"docsSkipped", Value(static_cast<int64_t>(stats.skipped))}});
}

Value cmdExplain(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    query::FindOptions opts;
    opts.limit = optionalCount(req, "limit", 0);
    query::ExplainResult result = engine.explain(Value(requireDoc(req, "filter")), opts);
    return okResponse(Document{{"plan", result.toValue()}});
}

Value cmdServerStatus(Server& server, const Document&) {
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - server.stats().startedAt)
                      .count();
    Document counters;
    {
        std::lock_guard lock(server.stats().opMutex);
        for (const auto& [cmd, count] : server.stats().opCounters) {
            counters.append(cmd, Value(static_cast<int64_t>(count)));
        }
    }
    return okResponse(
        Document{{"name", Value("BisonDB")},
                 {"version", Value(std::string(version()))},
                 {"protocolVersion", Value(int32_t{1})},
                 {"uptimeSec", Value(static_cast<int64_t>(uptime))},
                 {"connectionsCurrent",
                  Value(static_cast<int64_t>(server.stats().connectionsCurrent.load()))},
                 {"opCounters", Value(std::move(counters))}});
}

// ---- exception -> error code translation (the single place) ---------------

Value translateActiveException(const std::string& cmd) {
    try {
        throw;
    } catch (const CommandError& e) {
        return errorResponse(e.code(), e.what());
    } catch (const btree::DuplicateKeyError& e) {
        return errorResponse("DuplicateKey", e.what());
    } catch (const btree::KeyTooLong& e) {
        return errorResponse("TooLarge", e.what());
    } catch (const BsonParseError& e) {
        return errorResponse("CorruptData", e.what());
    } catch (const JsonParseError& e) {
        return errorResponse("ParseError", e.what());
    } catch (const btree::RebuildRequired& e) {
        return errorResponse("CorruptData", e.what());
    } catch (const query::QueryError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const store::StoreError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const TypeError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const std::exception& e) {
        return errorResponse("Internal", "command \"" + cmd + "\" failed: " + e.what());
    } catch (...) {
        return errorResponse("Internal", "command \"" + cmd + "\" failed: unknown error");
    }
}

} // namespace

Value dispatchCommand(Server& server, const Value& request, const net::TcpSocket& peer) {
    std::string cmd;
    try {
        if (!request.is<Document>()) {
            throw CommandError("BadRequest", "request must be a BSON document");
        }
        const Document& req = request.asDocument();
        cmd = requireString(req, "cmd");
        server.stats().countOp(cmd);

        if (cmd == "ping") {
            return okResponse();
        }
        if (cmd == "serverStatus") {
            return cmdServerStatus(server, req);
        }
        if (cmd == "listCollections") {
            Array names;
            for (const std::string& n : server.database().listCollections()) {
                names.push_back(Value(n));
            }
            return okResponse(Document{{"collections", Value(std::move(names))}});
        }
        if (cmd == "createCollection") {
            bool created = server.database().createCollection(requireString(req, "coll"));
            return okResponse(Document{{"created", Value(created)}});
        }
        if (cmd == "dbStats") {
            Array collections;
            for (const std::string& name : server.database().listCollections()) {
                query::IndexedCollection& coll = server.database().collection(name);
                Array indexes;
                for (const std::string& f : coll.listIndexes()) {
                    indexes.push_back(Value(f));
                }
                namespace fs = std::filesystem;
                uint64_t bytes = 0;
                fs::path dir(server.database().dir());
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    std::string file = entry.path().filename().string();
                    if (file == name + ".log" || file == name + ".meta.json" ||
                        (entry.path().extension() == ".idx" && file.rfind(name + ".", 0) == 0)) {
                        bytes += entry.file_size(ec);
                    }
                }
                collections.push_back(
                    Value(Document{{"name", Value(name)},
                                   {"count", Value(static_cast<int64_t>(coll.count()))},
                                   {"fileSizeBytes", Value(static_cast<int64_t>(bytes))},
                                   {"indexes", Value(std::move(indexes))}}));
            }
            return okResponse(Document{{"collections", Value(std::move(collections))}});
        }
        if (cmd == "dropCollection") {
            bool dropped = server.database().dropCollection(requireString(req, "coll"));
            return okResponse(Document{{"dropped", Value(dropped)}});
        }
        if (cmd == "insert") {
            return cmdInsert(server, req);
        }
        if (cmd == "find") {
            return cmdFind(server, req);
        }
        if (cmd == "deleteMany") {
            return cmdDeleteMany(server, req);
        }
        if (cmd == "updateOne") {
            return cmdUpdateOne(server, req);
        }
        if (cmd == "createIndex") {
            return cmdCreateIndex(server, req);
        }
        if (cmd == "dropIndex") {
            collOf(server, req).dropIndex(requireString(req, "field"));
            return okResponse();
        }
        if (cmd == "listIndexes") {
            Array indexes;
            for (const std::string& f : collOf(server, req).listIndexes()) {
                indexes.push_back(Value(f));
            }
            return okResponse(Document{{"indexes", Value(std::move(indexes))}});
        }
        if (cmd == "explain") {
            return cmdExplain(server, req);
        }
        if (cmd == "compact") {
            query::IndexedCollection& coll = collOf(server, req);
            std::size_t before = coll.count();
            coll.compact();
            return okResponse(Document{
                {"stats", Value(Document{{"documents", Value(static_cast<int64_t>(before))}})}});
        }
        if (cmd == "shutdown") {
            if (!peer.isLoopbackPeer()) {
                throw CommandError("BadRequest", "shutdown is only accepted from loopback");
            }
            server.requestStop();
            return okResponse();
        }
        throw CommandError("UnknownCommand", "unknown command \"" + cmd + "\"");
    } catch (...) {
        return translateActiveException(cmd);
    }
}

} // namespace bisondb::server
