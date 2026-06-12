#include "client/client.hpp"

#include "server/protocol.hpp"

namespace bisondb::client {

namespace {

Document docArg(const std::string& cmd, const std::string& coll) {
    return Document{{"cmd", Value(cmd)}, {"coll", Value(coll)}};
}

} // namespace

BisonClient BisonClient::connect(const std::string& host, uint16_t port, int timeoutMs) {
    net::TcpSocket socket = net::connectTcp(host, port, timeoutMs);
    return BisonClient(std::move(socket));
}

Value BisonClient::command(Value request) {
    server::writeFrame(socket_, request);
    std::optional<Value> response = server::readFrame(socket_);
    if (!response.has_value()) {
        throw net::NetError(net::NetError::Kind::Closed, "server closed the connection");
    }
    const Document& doc = response->asDocument();
    const Value* ok = doc.find("ok");
    if (ok != nullptr && ok->is<bool>() && ok->get<bool>()) {
        return std::move(*response);
    }
    std::string code = "Internal";
    std::string message = "malformed error response";
    if (const Value* err = doc.find("error"); err != nullptr && err->is<Document>()) {
        if (const Value* c = err->asDocument().find("code"); c && c->is<std::string>()) {
            code = c->get<std::string>();
        }
        if (const Value* m = err->asDocument().find("message"); m && m->is<std::string>()) {
            message = m->get<std::string>();
        }
    }
    throw ServerError(code, message);
}

void BisonClient::ping() { command(Value(Document{{"cmd", Value("ping")}})); }

Value BisonClient::serverStatus() {
    return command(Value(Document{{"cmd", Value("serverStatus")}}));
}

std::vector<std::string> BisonClient::listCollections() {
    Value resp = command(Value(Document{{"cmd", Value("listCollections")}}));
    std::vector<std::string> out;
    for (const Value& v : resp.asDocument().find("collections")->asArray()) {
        out.push_back(v.get<std::string>());
    }
    return out;
}

bool BisonClient::dropCollection(const std::string& coll) {
    Value resp = command(Value(docArg("dropCollection", coll)));
    return resp.asDocument().find("dropped")->get<bool>();
}

std::vector<ObjectId> BisonClient::insert(const std::string& coll,
                                          const std::vector<Value>& documents) {
    Document req = docArg("insert", coll);
    req.append("documents", Value(Array(documents.begin(), documents.end())));
    Value resp = command(Value(std::move(req)));
    std::vector<ObjectId> ids;
    for (const Value& v : resp.asDocument().find("insertedIds")->asArray()) {
        ids.push_back(v.get<ObjectId>());
    }
    return ids;
}

std::vector<Value> BisonClient::find(const std::string& coll, const Value& filter,
                                     const FindOptions& options) {
    std::vector<Value> out;
    std::size_t skip = options.skip;
    while (true) {
        Document req = docArg("find", coll);
        req.append("filter", filter);
        if (options.limit != 0) {
            req.append("limit", Value(static_cast<int64_t>(options.limit - out.size())));
        }
        req.append("skip", Value(static_cast<int64_t>(skip)));
        Value resp = command(Value(std::move(req)));
        const Document& payload = resp.asDocument();
        for (const Value& doc : payload.find("documents")->asArray()) {
            out.push_back(doc);
        }
        const Value* truncated = payload.find("truncated");
        bool more = truncated != nullptr && truncated->is<bool>() && truncated->get<bool>();
        if (!more || options.singleBatch) {
            return out;
        }
        if (options.limit != 0 && out.size() >= options.limit) {
            return out;
        }
        skip = static_cast<std::size_t>(payload.find("skipNext")->get<int64_t>());
    }
}

std::size_t BisonClient::deleteMany(const std::string& coll, const Value& filter) {
    Document req = docArg("deleteMany", coll);
    req.append("filter", filter);
    Value resp = command(Value(std::move(req)));
    return static_cast<std::size_t>(resp.asDocument().find("deletedCount")->get<int64_t>());
}

bool BisonClient::updateOne(const std::string& coll, const Value& filter, const Value& update) {
    Document req = docArg("updateOne", coll);
    req.append("filter", filter);
    req.append("update", update);
    Value resp = command(Value(std::move(req)));
    return resp.asDocument().find("matched")->get<bool>();
}

int64_t BisonClient::createIndex(const std::string& coll, const std::string& field) {
    Document req = docArg("createIndex", coll);
    req.append("field", Value(field));
    Value resp = command(Value(std::move(req)));
    return resp.asDocument().find("docsIndexed")->get<int64_t>();
}

void BisonClient::dropIndex(const std::string& coll, const std::string& field) {
    Document req = docArg("dropIndex", coll);
    req.append("field", Value(field));
    command(Value(std::move(req)));
}

std::vector<std::string> BisonClient::listIndexes(const std::string& coll) {
    Value resp = command(Value(docArg("listIndexes", coll)));
    std::vector<std::string> out;
    for (const Value& v : resp.asDocument().find("indexes")->asArray()) {
        out.push_back(v.get<std::string>());
    }
    return out;
}

Value BisonClient::explain(const std::string& coll, const Value& filter, std::size_t limit) {
    Document req = docArg("explain", coll);
    req.append("filter", filter);
    if (limit != 0) {
        req.append("limit", Value(static_cast<int64_t>(limit)));
    }
    Value resp = command(Value(std::move(req)));
    return *resp.asDocument().find("plan");
}

void BisonClient::compact(const std::string& coll) { command(Value(docArg("compact", coll))); }

void BisonClient::shutdownServer() { command(Value(Document{{"cmd", Value("shutdown")}})); }

} // namespace bisondb::client
