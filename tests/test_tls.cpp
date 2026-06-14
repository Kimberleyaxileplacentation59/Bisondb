#include "core/net/socket.hpp"
#include "core/net/tls.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <vector>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using namespace bisondb;
using namespace bisondb::net;
using Catch::Matchers::ContainsSubstring;

namespace {

// A connected loopback TcpSocket pair: {server-accepted, client-connected}.
struct SocketPair {
    TcpListener listener{"127.0.0.1", 0};
    TcpSocket server;
    TcpSocket client;
    SocketPair() {
        auto accepted = std::async(std::launch::async, [&] { return listener.accept(); });
        client = connectTcp("127.0.0.1", listener.port(), 2000);
        server = accepted.get();
    }
};

std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

TEST_CASE("generateSelfSigned produces a parseable cert with a stable fingerprint", "[tls]") {
    CertKeyPem ck = generateSelfSigned("localhost", 30);
    REQUIRE_THAT(ck.certPem, ContainsSubstring("BEGIN CERTIFICATE"));
    REQUIRE_THAT(ck.keyPem, ContainsSubstring("PRIVATE KEY"));

    std::string fp = certFingerprintSha256(ck.certPem);
    REQUIRE(fp.size() == 64); // SHA-256 hex
    // Two certs differ (random key) -> different fingerprints.
    REQUIRE(certFingerprintSha256(generateSelfSigned("localhost", 30).certPem) != fp);
}

TEST_CASE("TLS handshake over loopback, data round-trips both ways", "[tls]") {
    CertKeyPem ck = generateSelfSigned("localhost", 30);
    auto serverCtx = TlsContext::serverFromPem(ck.certPem, ck.keyPem);

    TlsClientOptions opts;
    opts.verify = TlsVerify::CaFile; // trust our self-signed cert as its own CA
    // CaFile wants a path; use Pin instead here to avoid temp files.
    opts.verify = TlsVerify::Pin;
    opts.pinSha256Hex = certFingerprintSha256(ck.certPem);
    opts.hostname = "localhost";
    auto clientCtx = TlsContext::client(opts);

    SocketPair pair;
    auto serverFut = std::async(
        std::launch::async, [&] { return TlsStream::accept(serverCtx, std::move(pair.server)); });
    TlsStream client = TlsStream::connect(clientCtx, std::move(pair.client));
    TlsStream server = serverFut.get();

    // client -> server
    client.sendAll(bytes("ping over tls"));
    std::vector<uint8_t> buf(13);
    REQUIRE(server.recvExact(buf) == RecvStatus::Complete);
    REQUIRE(std::string(buf.begin(), buf.end()) == "ping over tls");

    // server -> client
    server.sendAll(bytes("pong"));
    std::vector<uint8_t> buf2(4);
    REQUIRE(client.recvExact(buf2) == RecvStatus::Complete);
    REQUIRE(std::string(buf2.begin(), buf2.end()) == "pong");

    // The client pinned the server cert; the fingerprints agree.
    REQUIRE(client.peerFingerprint() == certFingerprintSha256(ck.certPem));
}

TEST_CASE("recvExact reassembles a payload split across TLS records", "[tls]") {
    CertKeyPem ck = generateSelfSigned("localhost", 30);
    auto serverCtx = TlsContext::serverFromPem(ck.certPem, ck.keyPem);
    TlsClientOptions opts;
    opts.verify = TlsVerify::Pin;
    opts.pinSha256Hex = certFingerprintSha256(ck.certPem);
    auto clientCtx = TlsContext::client(opts);

    SocketPair pair;
    auto serverFut = std::async(
        std::launch::async, [&] { return TlsStream::accept(serverCtx, std::move(pair.server)); });
    TlsStream client = TlsStream::connect(clientCtx, std::move(pair.client));
    TlsStream server = serverFut.get();

    // Server writes the message in three separate ssl_write calls (three
    // records); the client must reassemble it with a single recvExact.
    auto writer = std::async(std::launch::async, [&] {
        server.sendAll(bytes("AAAA"));
        server.sendAll(bytes("BBBBBB"));
        server.sendAll(bytes("CC"));
    });
    std::vector<uint8_t> buf(12);
    REQUIRE(client.recvExact(buf) == RecvStatus::Complete);
    writer.get();
    REQUIRE(std::string(buf.begin(), buf.end()) == "AAAABBBBBBCC");
}

TEST_CASE("writePrivateKeyFile writes a usable key with owner-only perms", "[tls]") {
    namespace fs = std::filesystem;
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("bisondb_keyperm_" + std::to_string(rd()));
    fs::create_directories(dir);
    fs::path keyPath = dir / "key.pem";

    CertKeyPem ck = generateSelfSigned("localhost", 30);
    writePrivateKeyFile(keyPath.string(), ck.keyPem);

    // Content round-trips.
    std::ifstream in(keyPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == ck.keyPem);

#if !defined(_WIN32)
    struct stat st{};
    REQUIRE(::stat(keyPath.string().c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600); // owner rw only
#endif
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("wrong pin is rejected after the handshake", "[tls]") {
    CertKeyPem ck = generateSelfSigned("localhost", 30);
    auto serverCtx = TlsContext::serverFromPem(ck.certPem, ck.keyPem);
    TlsClientOptions opts;
    opts.verify = TlsVerify::Pin;
    opts.pinSha256Hex = std::string(64, 'a'); // wrong fingerprint
    auto clientCtx = TlsContext::client(opts);

    SocketPair pair;
    auto serverFut = std::async(std::launch::async, [&] {
        try {
            TlsStream s = TlsStream::accept(serverCtx, std::move(pair.server));
            // Keep it alive briefly so the client side reaches its pin check.
        } catch (...) {
        }
    });
    bool threw = false;
    try {
        TlsStream::connect(clientCtx, std::move(pair.client));
    } catch (const TlsError& e) {
        threw = true;
        REQUIRE(e.kind() == TlsError::Kind::Verification);
    }
    serverFut.get();
    REQUIRE(threw);
}
