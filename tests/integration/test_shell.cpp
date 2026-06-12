// Shell integration tests: a real in-process server plus the Shell REPL
// driven through stringstreams (scripted mode, --eval, history).

#include "server/server.hpp"
#include "shell/banner.hpp"
#include "shell/printer.hpp"
#include "shell/repl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace bisondb;
using namespace bisondb::shell;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

struct ShellFixture {
    fs::path dir;
    std::unique_ptr<server::Server> srv;

    explicit ShellFixture(const std::string& name) {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("bisondb_sh_" + name + "_" + std::to_string(rd()));
        fs::remove_all(dir);
        server::ServerConfig config;
        config.dir = dir.string();
        config.port = 0;
        config.threads = 8;
        config.quiet = true;
        srv = std::make_unique<server::Server>(std::move(config));
        srv->start();
    }
    ~ShellFixture() {
        srv->stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    ShellConfig config() {
        ShellConfig c;
        c.host = "127.0.0.1";
        c.port = srv->port();
        c.color = false;
        c.interactive = false;
        return c;
    }
};

} // namespace

TEST_CASE("scripted session: insertMany, find, createIndex, explain, deleteMany",
          "[integration][shell]") {
    ShellFixture fx("script");
    std::istringstream script(R"(
db.students.insertMany([{name: 'ada', cgpa: 3.9}, {name: 'bob', cgpa: 2.1},
                        {name: 'eve', cgpa: 3.7},])
db.students.find({cgpa: {$gt: 3.5}}).limit(10)
db.students.count()
db.students.createIndex('cgpa')
db.students.find({cgpa: {$gt: 3.5}}).explain()
db.students.updateOne({name: 'bob'}, {$set: {cgpa: 2.5}})
db.students.deleteMany({cgpa: {$lt: 3.0}})
db.students.count()
show collections
db.students.getIndexes()
)");
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    Shell shell(fx.config(), in, out, err);
    int rc = shell.runScript(script);
    INFO("stderr: " << err.str());
    REQUIRE(rc == 0);

    std::string text = out.str();
    REQUIRE_THAT(text, ContainsSubstring("\"insertedCount\": 3"));
    REQUIRE_THAT(text, ContainsSubstring("\"ada\""));
    REQUIRE_THAT(text, ContainsSubstring("\"eve\""));
    REQUIRE_THAT(text, ContainsSubstring("returned 2 in"));
    REQUIRE_THAT(text, ContainsSubstring("\"plan\": \"index_range\""));
    REQUIRE_THAT(text, ContainsSubstring("index_range on \"cgpa\" — examined 2, returned 2"));
    REQUIRE_THAT(text, ContainsSubstring("\"matched\": true"));
    REQUIRE_THAT(text, ContainsSubstring("\"deletedCount\": 1"));
    REQUIRE_THAT(text, ContainsSubstring("students"));
    REQUIRE_THAT(text, ContainsSubstring("_id\ncgpa"));
    // counts before and after the delete
    REQUIRE_THAT(text, ContainsSubstring("\n3\n"));
    REQUIRE_THAT(text, ContainsSubstring("\n2\n"));
}

TEST_CASE("eval mode splits on top-level semicolons and reports rc", "[integration][shell]") {
    ShellFixture fx("eval");
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    Shell shell(fx.config(), in, out, err);
    int rc =
        shell.evalString("db.t.insertOne({msg: 'a;b'}); db.t.count(); db.t.find({msg: 'a;b'})");
    REQUIRE(rc == 0);
    REQUIRE_THAT(out.str(), ContainsSubstring("\n1\n"));
    REQUIRE_THAT(out.str(), ContainsSubstring("a;b"));

    // First failure stops execution with rc 1; the insert after it must not run.
    std::ostringstream out2;
    Shell shell2(fx.config(), in, out2, err);
    REQUIRE(shell2.evalString("db.t.bogus(); db.t.insertOne({x: 1})") == 1);
    std::ostringstream out3;
    Shell shell3(fx.config(), in, out3, err);
    shell3.evalString("db.t.count()");
    REQUIRE(out3.str() == "1\n"); // still only one doc
}

TEST_CASE("script file mode via -f", "[integration][shell]") {
    ShellFixture fx("file");
    fs::create_directories(fx.dir); // the server creates it lazily
    fs::path scriptPath = fx.dir / "script.bsh";
    {
        std::ofstream f(scriptPath);
        f << "db.c.insertOne({v: 7})\n";
        f << "db.c.find({v: {$gte: 7}})\n";
        f << "exit\n";
        f << "db.c.insertOne({v: 8})\n"; // never reached
    }
    std::ifstream script(scriptPath);
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    Shell shell(fx.config(), in, out, err);
    REQUIRE(shell.runScript(script) == 0);
    REQUIRE_THAT(out.str(), ContainsSubstring("\"v\": 7"));
    REQUIRE_THAT(out.str(), !ContainsSubstring("\"v\": 8"));
}

TEST_CASE("parse errors print a caret diagnostic and do not stop interactive mode",
          "[integration][shell]") {
    ShellFixture fx("caret");
    std::istringstream in("db.students.find({cgpa: {$gt 3.5}})\nexit\n");
    std::ostringstream out;
    std::ostringstream err;
    ShellConfig cfg = fx.config();
    cfg.interactive = true;
    Shell shell(cfg, in, out, err);
    REQUIRE(shell.runInteractive() == 0);
    REQUIRE_THAT(err.str(), ContainsSubstring("^ expected ':' after key '$gt'"));
    // The caret is positioned under the offending token.
    std::string stmt = "db.students.find({cgpa: {$gt 3.5}})";
    REQUIRE_THAT(err.str(), ContainsSubstring(std::string(stmt.find("3.5"), ' ') + "^ expected"));
}

TEST_CASE("multi-line continuation and blank-line cancel", "[integration][shell]") {
    ShellFixture fx("multiline");
    std::istringstream in("db.c.insertOne({\nv: 41\n})\n"
                          "db.c.insertOne({\n\n" // blank line cancels
                          "db.c.count()\n"
                          "exit\n");
    std::ostringstream out;
    std::ostringstream err;
    ShellConfig cfg = fx.config();
    cfg.interactive = true;
    Shell shell(cfg, in, out, err);
    REQUIRE(shell.runInteractive() == 0);
    REQUIRE_THAT(out.str(), ContainsSubstring("... ")); // continuation prompt appeared
    // count printed right after the prompt: only the first insert landed.
    REQUIRE_THAT(out.str(), ContainsSubstring("bisondb> 1\n"));
}

TEST_CASE("server errors print E[code] and the session continues", "[integration][shell]") {
    ShellFixture fx("errors");
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    Shell shell(fx.config(), in, out, err);
    REQUIRE_FALSE(
        shell.executeStatement("db.c.insertMany([{_id: {$oid: '507f1f77bcf86cd799439011'}},"
                               " {_id: {$oid: '507f1f77bcf86cd799439011'}}])"));
    REQUIRE_THAT(err.str(), ContainsSubstring("E[DuplicateKey]"));
    REQUIRE(shell.executeStatement("db.c.count()"));
}

TEST_CASE("startup banner shows once interactively, never in scripted modes",
          "[integration][shell]") {
    ShellFixture fx("banner");
    std::string bannerText = renderBanner(ColorMode::None, "0.1.0", "shell");

    SECTION("interactive: banner appears exactly once, before the connect line") {
        std::istringstream in("exit\n");
        std::ostringstream out;
        std::ostringstream err;
        ShellConfig cfg = fx.config();
        cfg.interactive = true;
        cfg.bannerText = bannerText;
        Shell shell(cfg, in, out, err);
        REQUIRE(shell.runInteractive() == 0);
        std::string text = out.str();
        std::size_t first = text.find("the prairie is open");
        REQUIRE(first != std::string::npos);
        REQUIRE(text.find("the prairie is open", first + 1) == std::string::npos);
        REQUIRE(first < text.find("BisonDB")); // banner precedes the connect line
    }
    SECTION("eval/script modes never print it (bannerText unset by main)") {
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        Shell shell(fx.config(), in, out, err);
        REQUIRE(shell.evalString("db.c.count()") == 0);
        REQUIRE(out.str().find("prairie is open") == std::string::npos);
        REQUIRE(out.str().find("█") == std::string::npos); // no block chars
    }
}

TEST_CASE("history persists, reloads, and caps", "[integration][shell]") {
    std::random_device rd;
    fs::path histPath =
        fs::temp_directory_path() / ("bisondb_hist_" + std::to_string(rd()) + ".txt");
    fs::remove(histPath);

    for (int i = 0; i < 30; ++i) {
        appendHistory(histPath.string(), "db.c.find({v: " + std::to_string(i) + "})",
                      /*cap=*/20);
    }
    std::vector<std::string> loaded = loadHistory(histPath.string(), 20);
    REQUIRE(loaded.size() == 20);
    REQUIRE(loaded.front() == "db.c.find({v: 10})"); // oldest 10 trimmed
    REQUIRE(loaded.back() == "db.c.find({v: 29})");

    // Multi-line statements are flattened to one history line.
    appendHistory(histPath.string(), "db.c.insertOne({\nv: 1\n})", 20);
    REQUIRE(loadHistory(histPath.string(), 20).back() == "db.c.insertOne({ v: 1 })");
    fs::remove(histPath);
}
