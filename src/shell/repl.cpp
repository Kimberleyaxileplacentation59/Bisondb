#include "shell/repl.hpp"

#include "core/json_writer.hpp"
#include "core/secure_input.hpp"
#include "shell/printer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace bisondb::shell {

namespace {

constexpr const char* kHelpText = R"(Statements:
  db.<coll>.insertOne(<doc>)            db.<coll>.insertMany([<doc>, ...])
  db.<coll>.find(<filter>?)             chainable: .limit(n) .skip(n) .explain()
  db.<coll>.count(<filter>?)            db.<coll>.deleteMany(<filter>)
  db.<coll>.updateOne(<filter>, {$set: {...}})
  db.<coll>.createIndex("field")        also accepts ({field: 1})
  db.<coll>.dropIndex("field")          db.<coll>.getIndexes()
  db.<coll>.drop()                      db.<coll>.compact()
  show collections                      show status
  auth <...>                            accounts (see 'auth help')
  help                                  exit | quit

Filters: {field: literal}, $eq $ne $gt $gte $lt $lte $in, $and $or, dotted
paths. JSON is relaxed: unquoted keys, single quotes, trailing commas.
)";

constexpr const char* kAuthHelpText = R"(Auth:
  auth login [<user>]                 prompt for password and log in
  auth logout                         end the current session
  auth whoami                         show the authenticated user and roles
  auth passwd [<user>]                change a password (self needs old pw)
  auth create-user <user> [<role>..]  admin: create a user (default role: read)
  auth list-users                     admin: list users and roles
  auth bootstrap <user>               first-run: create the first admin
)";

std::string formatMs(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", ms);
    return buf;
}

std::string joinRoles(const std::vector<std::string>& roles) {
    std::string out;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        out += (i ? "," : "") + roles[i];
    }
    return out.empty() ? "(none)" : out;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string word;
    while (iss >> word) {
        out.push_back(word);
    }
    return out;
}

} // namespace

std::vector<std::string> loadHistory(const std::string& path, std::size_t cap) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    if (out.size() > cap) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(cap));
    }
    return out;
}

void appendHistory(const std::string& path, const std::string& statement, std::size_t cap) {
    if (path.empty() || statement.empty()) {
        return;
    }
    // Multi-line statements are stored flattened so one line = one entry.
    std::string flat = statement;
    for (char& c : flat) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    std::vector<std::string> entries = loadHistory(path, cap);
    entries.push_back(flat);
    if (entries.size() > cap) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(cap));
    }
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& e : entries) {
        out << e << "\n";
    }
}

Shell::Shell(ShellConfig config, std::istream& in, std::ostream& out, std::ostream& err)
    : config_(std::move(config)), in_(in), out_(out), err_(err) {}

client::BisonClient& Shell::client() {
    if (!client_) {
        client_ = client::BisonClient::connect(config_.host, config_.port, config_.tls,
                                               config_.connectTimeoutMs);
    }
    if (!autoAuthDone_) {
        autoAuthDone_ = true; // attempt once; failures surface to the caller
        autoAuthenticate();
    }
    return *client_;
}

void Shell::autoAuthenticate() {
    // Detect setup mode first so we can guide the user instead of failing.
    try {
        Value status = client_->serverStatus();
        if (const Value* sec = status.asDocument().find("security"); sec && sec->is<Document>()) {
            const Value* sm = sec->asDocument().find("setupMode");
            if (sm && sm->is<bool>() && sm->get<bool>()) {
                err_ << "server is in SETUP MODE — no users exist yet.\n"
                        "create the first admin with:  auth bootstrap <username>\n";
                return;
            }
        }
    } catch (...) {
        // Ignore; a real connection problem surfaces on the actual command.
    }

    if (config_.token.empty() && config_.username.empty()) {
        return; // nothing configured (e.g. --no-auth server)
    }
    if (!config_.token.empty()) {
        client_->authenticateToken(config_.token);
        return;
    }
    std::string password = config_.password;
    if (password.empty()) {
        if (!config_.interactive) {
            throw client::AuthError("AuthRequired",
                                    "password required (set BISONDB_PASSWORD or use --token)");
        }
        password = promptPassword("Password for " + config_.username + ": ");
    }
    client_->authenticate(config_.username, password);
}

std::string Shell::promptPassword(const std::string& prompt) {
    return readPasswordFromTty(prompt);
}

void Shell::printError(const std::string& code, const std::string& what) {
    err_ << (config_.color ? ansi::kRed : "") << "E[" << code << "] " << what
         << (config_.color ? ansi::kReset : "") << "\n";
}

void Shell::printValue(const Value& v) {
    out_ << colorizeJson(toJson(v, JsonMode::Relaxed, /*pretty=*/true), config_.color) << "\n";
}

void Shell::printDocs(const std::vector<Value>& docs) {
    std::size_t shown = 0;
    for (const Value& doc : docs) {
        if (config_.interactive && shown != 0 && shown % config_.pageSize == 0) {
            out_ << (config_.color ? ansi::kDim : "") << "-- more (Enter) / q --"
                 << (config_.color ? ansi::kReset : "") << std::flush;
            std::string answer;
            std::getline(in_, answer);
            out_ << "\n";
            if (answer == "q" || answer == "Q") {
                return;
            }
        }
        printValue(doc);
        ++shown;
    }
}

void Shell::printParseError(const std::string& statement, const ShellParseError& e) {
    // Locate the line containing the error offset for the caret diagnostic.
    std::size_t lineStart = 0;
    std::size_t pos = std::min(e.position(), statement.size());
    for (std::size_t i = 0; i < pos; ++i) {
        if (statement[i] == '\n') {
            lineStart = i + 1;
        }
    }
    std::size_t lineEnd = statement.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
        lineEnd = statement.size();
    }
    err_ << statement.substr(lineStart, lineEnd - lineStart) << "\n";
    err_ << std::string(pos - lineStart, ' ') << "^ " << e.what() << "\n";
}

bool Shell::executeStatement(const std::string& statement) {
    // `auth ...` meta-commands are handled outside the db-statement grammar.
    {
        std::size_t start = statement.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            std::vector<std::string> words = splitWords(statement.substr(start));
            if (!words.empty() && words[0] == "auth") {
                return handleAuth(words);
            }
        }
    }

    ShellCommand cmd;
    try {
        cmd = parseStatement(statement);
    } catch (const ShellParseError& e) {
        printParseError(statement, e);
        return false;
    }
    try {
        return execute(cmd);
    } catch (const net::NetError&) {
        // One transparent reconnect, then fail.
        dropClient();
        try {
            return execute(cmd);
        } catch (const net::NetError& e) {
            err_ << (config_.color ? ansi::kRed : "") << "E[Network] " << e.what()
                 << (config_.color ? ansi::kReset : "") << "\n"
                 << "  is bisond running at " << config_.host << ":" << config_.port << "?\n";
            if (!config_.tls.enabled) {
                err_ << "  if the server uses TLS, reconnect with --tls.\n";
            }
            dropClient();
            return false;
        } catch (const client::AuthError& e) {
            printError(e.code(), e.what());
            return false;
        } catch (const client::ServerError& e) {
            printError(e.code(), e.what());
            return false;
        }
    } catch (const client::AuthError& e) {
        printError(e.code(), e.what());
        if (e.code() == "AuthRequired") {
            err_ << "  run 'auth login <user>' first\n";
        }
        return false;
    } catch (const client::ServerError& e) {
        printError(e.code(), e.what());
        return false;
    }
}

bool Shell::handleAuth(const std::vector<std::string>& args) {
    // args[0] == "auth"
    std::string sub = args.size() > 1 ? args[1] : "";
    try {
        if (sub.empty() || sub == "help") {
            out_ << kAuthHelpText;
            return true;
        }
        if (sub == "login") {
            std::string user = args.size() > 2 ? args[2] : config_.username;
            if (user.empty()) {
                err_ << "usage: auth login <user>\n";
                return false;
            }
            std::string pw = promptPassword("Password for " + user + ": ");
            auto roles = client().authenticate(user, pw);
            out_ << "logged in as " << user << " [" << joinRoles(roles) << "]\n";
            return true;
        }
        if (sub == "logout") {
            client().logout();
            out_ << "logged out\n";
            return true;
        }
        if (sub == "whoami") {
            client::BisonClient& c = client();
            if (!c.authenticated()) {
                out_ << "not authenticated\n";
            } else {
                out_ << c.currentUser() << " [" << joinRoles(c.currentRoles()) << "]\n";
            }
            return true;
        }
        if (sub == "passwd") {
            std::string target = args.size() > 2 ? args[2] : "";
            client::BisonClient& c = client();
            bool isSelf = target.empty() || target == c.currentUser();
            std::string oldPw;
            if (isSelf) {
                oldPw = promptPassword("Current password: ");
            }
            std::string newPw = promptPassword("New password: ");
            std::string confirm = promptPassword("Confirm new password: ");
            if (newPw != confirm) {
                err_ << "passwords do not match\n";
                return false;
            }
            c.changePassword(newPw, oldPw, isSelf ? "" : target);
            out_ << "password changed\n";
            return true;
        }
        if (sub == "create-user") {
            if (args.size() < 3) {
                err_ << "usage: auth create-user <user> [<role>...]\n";
                return false;
            }
            std::string user = args[2];
            std::vector<std::string> roles(args.begin() + 3, args.end());
            if (roles.empty()) {
                roles.push_back("read");
            }
            std::string pw = promptPassword("Password for new user " + user + ": ");
            std::string confirm = promptPassword("Confirm password: ");
            if (pw != confirm) {
                err_ << "passwords do not match\n";
                return false;
            }
            client().createUser(user, pw, roles);
            out_ << "created user " << user << " [" << joinRoles(roles) << "]\n";
            return true;
        }
        if (sub == "list-users") {
            for (const client::UserInfo& u : client().listUsers()) {
                out_ << u.username << "  [" << joinRoles(u.roles) << "]"
                     << (u.disabled ? "  (disabled)" : "") << "\n";
            }
            return true;
        }
        if (sub == "bootstrap") {
            if (args.size() < 3) {
                err_ << "usage: auth bootstrap <user>   (run this in setup mode)\n";
                return false;
            }
            std::string user = args[2];
            std::string token = promptPassword("Bootstrap token: ");
            std::string pw = promptPassword("New admin password: ");
            std::string confirm = promptPassword("Confirm password: ");
            if (pw != confirm) {
                err_ << "passwords do not match\n";
                return false;
            }
            auto roles = client().bootstrapAdmin(token, user, pw);
            out_ << "created first admin " << user << " [" << joinRoles(roles) << "]\n";
            return true;
        }
        err_ << "unknown auth subcommand '" << sub << "' (try 'auth help')\n";
        return false;
    } catch (const client::AuthError& e) {
        printError(e.code(), e.what());
        return false;
    } catch (const client::ServerError& e) {
        printError(e.code(), e.what());
        return false;
    } catch (const net::NetError& e) {
        printError("Network", e.what());
        return false;
    }
}

bool Shell::execute(const ShellCommand& cmd) {
    using clock = std::chrono::steady_clock;
    switch (cmd.verb) {
    case Verb::Help: out_ << kHelpText; return true;
    case Verb::Exit: exitRequested_ = true; return true;
    case Verb::ShowCollections: {
        for (const std::string& name : client().listCollections()) {
            out_ << name << "\n";
        }
        return true;
    }
    case Verb::ShowStatus: {
        printValue(client().serverStatus());
        return true;
    }
    case Verb::InsertOne:
    case Verb::InsertMany: {
        std::vector<Value> docs;
        if (cmd.verb == Verb::InsertOne) {
            docs.push_back(cmd.args[0]);
        } else {
            const Array& arr = cmd.args[0].asArray();
            docs.assign(arr.begin(), arr.end());
        }
        std::vector<ObjectId> ids = client().insert(cmd.coll, docs);
        Array idValues;
        for (const ObjectId& id : ids) {
            idValues.push_back(Value(id));
        }
        printValue(Value(Document{{"insertedCount", Value(static_cast<int64_t>(ids.size()))},
                                  {"insertedIds", Value(std::move(idValues))}}));
        return true;
    }
    case Verb::Find: {
        if (cmd.explain) {
            Value plan = client().explain(cmd.coll, cmd.args[0],
                                          cmd.limit ? static_cast<std::size_t>(*cmd.limit) : 0);
            printValue(plan);
            const Document& d = plan.asDocument();
            std::string planName = d.find("plan")->get<std::string>();
            std::string summary = planName;
            if (const Value* idx = d.find("index")) {
                summary += " on \"" + idx->get<std::string>() + "\"";
            }
            summary += " — examined " + std::to_string(d.find("docsExamined")->get<int64_t>()) +
                       ", returned " + std::to_string(d.find("docsReturned")->get<int64_t>());
            out_ << (config_.color ? ansi::kDim : "") << summary
                 << (config_.color ? ansi::kReset : "") << "\n";
            return true;
        }
        client::FindOptions opts;
        if (cmd.limit) {
            opts.limit = static_cast<std::size_t>(*cmd.limit);
        }
        if (cmd.skip) {
            opts.skip = static_cast<std::size_t>(*cmd.skip);
        }
        auto begun = clock::now();
        std::vector<Value> docs = client().find(cmd.coll, cmd.args[0], opts);
        double ms = std::chrono::duration<double, std::milli>(clock::now() - begun).count();
        printDocs(docs);
        out_ << (config_.color ? ansi::kDim : "") << "returned " << docs.size() << " in "
             << formatMs(ms) << " ms" << (config_.color ? ansi::kReset : "") << "\n";
        return true;
    }
    case Verb::Count: {
        Value plan = client().explain(cmd.coll, cmd.args[0]);
        out_ << plan.asDocument().find("docsReturned")->get<int64_t>() << "\n";
        return true;
    }
    case Verb::DeleteMany: {
        std::size_t n = client().deleteMany(cmd.coll, cmd.args[0]);
        printValue(Value(Document{{"deletedCount", Value(static_cast<int64_t>(n))}}));
        return true;
    }
    case Verb::UpdateOne: {
        bool matched = client().updateOne(cmd.coll, cmd.args[0], cmd.args[1]);
        printValue(Value(Document{{"matched", Value(matched)}, {"modified", Value(matched)}}));
        return true;
    }
    case Verb::CreateIndex: {
        int64_t n = client().createIndex(cmd.coll, cmd.field);
        printValue(Value(Document{{"built", Value(true)}, {"docsIndexed", Value(n)}}));
        return true;
    }
    case Verb::DropIndex: {
        client().dropIndex(cmd.coll, cmd.field);
        out_ << "dropped\n";
        return true;
    }
    case Verb::GetIndexes: {
        for (const std::string& field : client().listIndexes(cmd.coll)) {
            out_ << field << "\n";
        }
        return true;
    }
    case Verb::Drop: {
        bool dropped = client().dropCollection(cmd.coll);
        printValue(Value(Document{{"dropped", Value(dropped)}}));
        return true;
    }
    case Verb::Compact: {
        client().compact(cmd.coll);
        out_ << "compacted\n";
        return true;
    }
    }
    return false;
}

void Shell::banner() {
    try {
        Value status = client().serverStatus();
        const Document& d = status.asDocument();
        out_ << d.find("name")->get<std::string>() << " " << d.find("version")->get<std::string>()
             << " @ " << config_.host << ":" << config_.port;
        if (client_ && client_->authenticated()) {
            out_ << "  (as " << client_->currentUser() << ")";
        }
        out_ << "\n";
        // Surface the security posture: a transport lock indicator + auth state.
        if (const Value* sec = d.find("security"); sec && sec->is<Document>()) {
            const Document& s = sec->asDocument();
            bool auth = s.find("auth") && s.find("auth")->is<bool>() && s.find("auth")->get<bool>();
            const char* transport;
            const char* tColor = "";
            if (!client_ || !client_->isTls()) {
                transport = "transport: NOT encrypted (no TLS) - add --tls";
                tColor = config_.color ? ansi::kRed : "";
            } else if (client_->tlsVerified()) {
                transport = "transport: TLS, verified";
                tColor = config_.color ? ansi::kDim : "";
            } else {
                transport = "transport: TLS, ENCRYPTED but UNVERIFIED (--tls-insecure)";
                tColor = config_.color ? ansi::kRed : "";
            }
            out_ << tColor << transport << (config_.color ? ansi::kReset : "") << "\n"
                 << (config_.color ? ansi::kDim : "")
                 << (auth ? "auth: enabled" : "auth: DISABLED (--no-auth)")
                 << (config_.color ? ansi::kReset : "") << "\n";
        }
    } catch (const client::AuthError& e) {
        printError(e.code(), e.what());
    } catch (const std::exception& e) {
        err_ << "cannot reach " << config_.host << ":" << config_.port << ": " << e.what() << "\n";
    }
    out_ << "type 'help' for the statement grammar, 'auth help' for accounts\n";
}

int Shell::runInteractive() {
    if (!config_.bannerText.empty()) {
        out_ << config_.bannerText;
    }
    banner();
    std::string pending;
    while (!exitRequested_) {
        out_ << (pending.empty() ? "bisondb> " : "... ") << std::flush;
        std::string line;
        if (!std::getline(in_, line)) {
            break;
        }
        if (!pending.empty() && line.empty()) {
            // Blank line at '...' cancels the pending statement.
            pending.clear();
            continue;
        }
        pending += pending.empty() ? line : "\n" + line;
        if (pending.find_first_not_of(" \t") == std::string::npos) {
            pending.clear();
            continue;
        }
        if (needsMoreInput(pending)) {
            continue;
        }
        std::string statement = std::move(pending);
        pending.clear();
        executeStatement(statement);
        if (!config_.historyPath.empty()) {
            appendHistory(config_.historyPath, statement);
        }
    }
    return 0;
}

int Shell::runScript(std::istream& script) {
    std::string pending;
    std::string line;
    while (std::getline(script, line)) {
        pending += pending.empty() ? line : "\n" + line;
        if (pending.find_first_not_of(" \t\r\n") == std::string::npos) {
            pending.clear();
            continue;
        }
        if (needsMoreInput(pending)) {
            continue;
        }
        for (const std::string& stmt : splitStatements(pending)) {
            if (!executeStatement(stmt)) {
                return 1;
            }
            if (exitRequested_) {
                return 0;
            }
        }
        pending.clear();
    }
    if (!pending.empty() && pending.find_first_not_of(" \t\r\n") != std::string::npos) {
        err_ << "unterminated statement at end of input\n";
        return 1;
    }
    return 0;
}

int Shell::evalString(const std::string& statements) {
    for (const std::string& stmt : splitStatements(statements)) {
        if (!executeStatement(stmt)) {
            return 1;
        }
        if (exitRequested_) {
            return 0;
        }
    }
    return 0;
}

} // namespace bisondb::shell
