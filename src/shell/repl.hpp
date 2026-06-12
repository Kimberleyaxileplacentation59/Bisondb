#pragma once

#include "client/client.hpp"
#include "shell/parser.hpp"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bisondb::shell {

struct ShellConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 27027;
    bool color = true;
    bool interactive = true; // prompts, pager, banner
    std::string historyPath; // empty = history disabled
    std::size_t pageSize = 100;
    int connectTimeoutMs = 5000;
    // Pre-rendered startup banner (see banner.hpp); printed once at the top
    // of runInteractive. Empty = no banner (scripted modes never set it).
    std::string bannerText;
};

// History persistence helpers (exposed for tests). The file holds one
// statement per line, capped to the most recent `cap` entries.
std::vector<std::string> loadHistory(const std::string& path, std::size_t cap = 1000);
void appendHistory(const std::string& path, const std::string& statement, std::size_t cap = 1000);

// The REPL engine. Streams are injected so integration tests can drive it
// with stringstreams; `bisonsh` wires std::cin/cout/cerr.
class Shell {
  public:
    Shell(ShellConfig config, std::istream& in, std::ostream& out, std::ostream& err);

    // Interactive loop: banner, prompt, multi-line continuation, pager.
    int runInteractive();

    // Scripted mode (--eval / -f / piped stdin): execute sequentially, no
    // prompts, return 1 on the first failing statement.
    int runScript(std::istream& script);
    int evalString(const std::string& statements);

    // Parses + executes one complete statement. Returns false on error
    // (which has already been printed).
    bool executeStatement(const std::string& statement);

  private:
    client::BisonClient& client();
    void dropClient() { client_.reset(); }
    bool execute(const ShellCommand& cmd);
    void printDocs(const std::vector<Value>& docs);
    void printValue(const Value& v);
    void printParseError(const std::string& statement, const ShellParseError& e);
    void banner();

    ShellConfig config_;
    std::istream& in_;
    std::ostream& out_;
    std::ostream& err_;
    std::optional<client::BisonClient> client_;
    bool exitRequested_ = false;
};

} // namespace bisondb::shell
