// Integration tests for the authentication layer: a real Server in-process,
// exercised through BisonClient. Covers bootstrap, the handshake/state machine,
// role enforcement, token resume/expiry/logout, and security regressions.
#include "client/client.hpp"
#include "core/crypto/crypto.hpp"
#include "server/auth_session.hpp"
#include "server/server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <thread>

using namespace bisondb;
using bisondb::client::AuthError;
using bisondb::client::BisonClient;
using bisondb::client::Credentials;
namespace fs = std::filesystem;

namespace {

void setEnv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
void clearEnv(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

crypto::KdfParams fastKdf() {
    crypto::KdfParams p;
    p.memoryKiB = 64;
    p.passes = 1;
    p.lanes = 1;
    return p;
}

struct AuthFixture {
    fs::path dir;
    std::unique_ptr<server::Server> srv;

    AuthFixture(const std::string& name, bool withAdmin, std::int64_t ttlSeconds = 3600) {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("bisondb_auth_it_" + name + "_" + std::to_string(rd()));
        fs::remove_all(dir);
        server::ServerConfig config;
        config.dir = dir.string();
        config.port = 0;
        config.threads = 4;
        config.quiet = true;
        config.throttleAuth = false; // no real sleeps in tests
        config.kdf = fastKdf();
        config.tokenTtlSeconds = ttlSeconds;
        if (withAdmin) {
            config.initAdminUser = "root";
            setEnv("BISONDB_ADMIN_PASSWORD", "rootpw");
        }
        srv = std::make_unique<server::Server>(std::move(config));
        srv->start();
        if (withAdmin) {
            clearEnv("BISONDB_ADMIN_PASSWORD");
        }
    }

    ~AuthFixture() {
        if (srv) {
            srv->stop();
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    uint16_t port() const { return srv->port(); }
    BisonClient raw() { return BisonClient::connect("127.0.0.1", port(), 2000); }
    Credentials rootCreds() { return Credentials{"root", "rootpw", ""}; }
};

} // namespace

TEST_CASE("setup mode: bootstrap the first admin", "[integration][auth]") {
    AuthFixture fx("setup", /*withAdmin=*/false);
    REQUIRE(fx.srv->inSetupMode());

    // Pre-bootstrap: data commands are blocked.
    {
        BisonClient c = fx.raw();
        REQUIRE_THROWS_AS(c.listCollections(), AuthError);
    }
    // Wrong bootstrap token is rejected.
    {
        BisonClient c = fx.raw();
        REQUIRE_THROWS_AS(c.bootstrapAdmin("not-the-token", "admin", "adminpw"), AuthError);
    }
    // Correct token creates the admin and leaves the connection authenticated.
    {
        std::string bt = fx.srv->bootstrapToken();
        REQUIRE_FALSE(bt.empty());
        BisonClient c = fx.raw();
        auto roles = c.bootstrapAdmin(bt, "admin", "adminpw");
        REQUIRE(roles.size() == 1);
        REQUIRE(roles[0] == "admin");
        REQUIRE(c.createCollection("things"));
    }
    // Setup mode has ended; the token no longer works.
    REQUIRE_FALSE(fx.srv->inSetupMode());
    {
        BisonClient c = fx.raw();
        REQUIRE_THROWS_AS(c.listCollections(), AuthError); // needs real login now
    }
}

TEST_CASE("init-admin seeds an admin from the environment", "[integration][auth]") {
    AuthFixture fx("initadmin", /*withAdmin=*/true);
    REQUIRE_FALSE(fx.srv->inSetupMode());

    // Good credentials -> full CRUD.
    BisonClient c = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 2000);
    REQUIRE(c.authenticated());
    REQUIRE(c.currentUser() == "root");
    REQUIRE(c.createCollection("users"));
    auto ids = c.insert("users", {Value(Document{{"name", Value("ada")}})});
    REQUIRE(ids.size() == 1);
    REQUIRE(c.find("users", Value(Document{})).size() == 1);
}

TEST_CASE("bad credentials never reveal whether a user exists", "[integration][auth]") {
    AuthFixture fx("enum", /*withAdmin=*/true);

    BisonClient c1 = fx.raw();
    BisonClient c2 = fx.raw();
    std::string msgWrongPw, msgUnknownUser, codeWrongPw, codeUnknownUser;
    try {
        c1.authenticate("root", "WRONG");
    } catch (const AuthError& e) {
        codeWrongPw = e.code();
        msgWrongPw = e.what();
    }
    try {
        c2.authenticate("ghost", "whatever");
    } catch (const AuthError& e) {
        codeUnknownUser = e.code();
        msgUnknownUser = e.what();
    }
    REQUIRE(codeWrongPw == "AuthFailed");
    REQUIRE(codeUnknownUser == "AuthFailed");
    REQUIRE(msgWrongPw == msgUnknownUser); // identical: no enumeration
}

TEST_CASE("unauthenticated connection is blocked from data commands", "[integration][auth]") {
    AuthFixture fx("gate", /*withAdmin=*/true);
    BisonClient c = fx.raw();

    // ping and serverStatus are allowed pre-auth.
    REQUIRE_NOTHROW(c.ping());
    Value status = c.serverStatus();
    const Document& sec = status.asDocument().find("security")->asDocument();
    REQUIRE(sec.find("auth")->get<bool>() == true);
    REQUIRE(sec.find("tls")->get<bool>() == false);
    // Pre-auth serverStatus must not leak op counters.
    REQUIRE(status.asDocument().find("opCounters") == nullptr);

    // Everything else requires auth.
    REQUIRE_THROWS_AS(c.listCollections(), AuthError);
    REQUIRE_THROWS_AS(c.insert("x", {Value(Document{})}), AuthError);
}

TEST_CASE("role enforcement: read vs readWrite vs admin", "[integration][auth]") {
    AuthFixture fx("roles", /*withAdmin=*/true);
    BisonClient admin = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 2000);

    admin.createUser("reader", "readerpw", {"read"});
    admin.createUser("writer", "writerpw", {"readWrite"});
    admin.createCollection("data");
    admin.insert("data", {Value(Document{{"v", Value(int32_t{1})}})});

    // read role: find OK, insert Forbidden.
    BisonClient reader =
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"reader", "readerpw", ""}, 2000);
    REQUIRE(reader.find("data", Value(Document{})).size() == 1);
    try {
        reader.insert("data", {Value(Document{{"v", Value(int32_t{2})}})});
        FAIL("read user should not be able to insert");
    } catch (const AuthError& e) {
        REQUIRE(e.code() == "Forbidden");
    }
    // read role cannot manage users.
    REQUIRE_THROWS_AS(reader.createUser("x", "y", {"read"}), AuthError);

    // readWrite role: insert OK, user management Forbidden.
    BisonClient writer =
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"writer", "writerpw", ""}, 2000);
    REQUIRE(writer.insert("data", {Value(Document{{"v", Value(int32_t{3})}})}).size() == 1);
    REQUIRE_THROWS_AS(writer.listUsers(), AuthError);

    // admin: listUsers works and never includes password material.
    auto users = admin.listUsers();
    REQUIRE(users.size() == 3); // root, reader, writer
}

TEST_CASE("token resume and logout", "[integration][auth]") {
    AuthFixture fx("token", /*withAdmin=*/true);
    BisonClient a = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 2000);
    std::string token = a.sessionToken();
    REQUIRE_FALSE(token.empty());

    // A fresh connection can resume with the token.
    BisonClient b = fx.raw();
    auto roles = b.authenticateToken(token);
    REQUIRE(roles.size() == 1);
    REQUIRE(b.currentUser() == "root");
    REQUIRE(b.listCollections().empty());

    // Logout invalidates the token everywhere.
    b.logout();
    BisonClient d = fx.raw();
    REQUIRE_THROWS_AS(d.authenticateToken(token), AuthError);
}

TEST_CASE("token expiry: transparent re-auth with password, hard fail token-only",
          "[integration][auth]") {
    AuthFixture fx("expiry", /*withAdmin=*/true, /*ttlSeconds=*/1);

    // Password client: a command after expiry transparently re-authenticates.
    BisonClient pw = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 4000);
    std::string firstToken = pw.sessionToken();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE_NOTHROW(pw.listCollections()); // succeeds via silent re-auth
    REQUIRE(pw.sessionToken() != firstToken);

    // Token-only client: nothing to refresh with -> TokenExpired surfaces.
    BisonClient seed = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 4000);
    std::string tok = seed.sessionToken();
    BisonClient tonly = fx.raw();
    tonly.authenticateToken(tok);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    try {
        tonly.listCollections();
        FAIL("expected TokenExpired");
    } catch (const AuthError& e) {
        REQUIRE(e.code() == "TokenExpired");
    }
}

TEST_CASE("changePassword: self with old password, admin reset", "[integration][auth]") {
    AuthFixture fx("passwd", /*withAdmin=*/true);
    BisonClient admin = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 2000);
    admin.createUser("bob", "bobpw", {"readWrite"});

    // Self-service: wrong old password rejected, correct one works.
    BisonClient bob =
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"bob", "bobpw", ""}, 2000);
    REQUIRE_THROWS_AS(bob.changePassword("newpw", "WRONGOLD"), AuthError);
    REQUIRE_NOTHROW(bob.changePassword("newpw", "bobpw"));

    // Old password no longer logs in; new one does.
    {
        BisonClient c = fx.raw();
        REQUIRE_THROWS_AS(c.authenticate("bob", "bobpw"), AuthError);
    }
    REQUIRE_NOTHROW(
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"bob", "newpw", ""}, 2000));

    // Admin reset (no old password needed).
    REQUIRE_NOTHROW(admin.changePassword("resetpw", "", "bob"));
    REQUIRE_NOTHROW(
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"bob", "resetpw", ""}, 2000));
}

TEST_CASE("auth backoff grows then caps", "[auth]") {
    using bisondb::server::authBackoffMs;
    REQUIRE(authBackoffMs(0) == 0);
    REQUIRE(authBackoffMs(2) == 0); // first couple are free (typos)
    REQUIRE(authBackoffMs(3) > 0);
    REQUIRE(authBackoffMs(4) > authBackoffMs(3)); // monotonic
    REQUIRE(authBackoffMs(1000) == 2000);         // capped
}

TEST_CASE("--no-auth refuses to bind to a non-loopback address", "[integration][auth]") {
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("bisondb_noauth_" + std::to_string(rd()));
    fs::remove_all(dir);
    server::ServerConfig config;
    config.dir = dir.string();
    config.port = 0;
    config.quiet = true;
    config.noAuth = true;
    config.bind = "0.0.0.0"; // non-loopback
    server::Server srv(std::move(config));
    REQUIRE_THROWS(srv.start());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("shutdown requires the admin role", "[integration][auth]") {
    AuthFixture fx("shutdown", /*withAdmin=*/true);
    BisonClient admin = BisonClient::connect("127.0.0.1", fx.port(), fx.rootCreds(), 2000);
    admin.createUser("reader", "readerpw", {"read"});

    BisonClient reader =
        BisonClient::connect("127.0.0.1", fx.port(), Credentials{"reader", "readerpw", ""}, 2000);
    try {
        reader.shutdownServer();
        FAIL("read user must not shut the server down");
    } catch (const AuthError& e) {
        REQUIRE(e.code() == "Forbidden");
    }
    // Server is still alive.
    REQUIRE_NOTHROW(admin.ping());
}
