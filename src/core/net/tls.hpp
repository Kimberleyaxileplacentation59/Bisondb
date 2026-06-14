#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// TLS wrapper — the ONLY place the engine touches a TLS library (Mbed-TLS).
//
// TlsStream layers a TLS 1.2/1.3 session over an owned TcpSocket and exposes the
// SAME sendAll/recvExact interface as TcpSocket, so the framing code above it is
// unchanged. A TlsContext holds the shared configuration (server cert+key, or
// client trust/verification policy) and can mint many streams.
//
// SECURITY: private keys are never logged. Verification is secure by default
// (CaFile/System verify the chain AND the hostname); Insecure must be chosen
// explicitly and warned about by the caller.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/error.hpp"
#include "core/net/socket.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace bisondb::net {

// TLS handshake / verification failures, distinct from NetError so callers can
// tell "the connection works but the peer isn't trusted" from a socket error.
class TlsError : public Error {
  public:
    enum class Kind {
        Config,       // bad cert/key/CA file, misconfiguration
        Handshake,    // protocol-level handshake failure (incl. plaintext peer)
        Verification, // chain/hostname/pin verification failed
        Io,           // transport error during TLS
    };
    TlsError(Kind kind, const std::string& message) : Error(message), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

  private:
    Kind kind_;
};

// Client-side certificate verification policy.
enum class TlsVerify {
    System,   // verify against the OS trust store + hostname (secure default)
    CaFile,   // verify against a specific CA / self-signed PEM + hostname
    Pin,      // accept exactly the cert whose SHA-256 fingerprint matches
    Insecure, // skip all verification (DANGEROUS, dev-only, must be loud)
};

struct TlsClientOptions {
    TlsVerify verify = TlsVerify::System;
    std::string caFile;       // PEM path, for CaFile
    std::string pinSha256Hex; // lowercase hex (colons optional), for Pin
    std::string hostname;     // SNI + hostname verification target
};

class TlsContextImpl; // opaque (owns all mbedtls state)

// Shared, immutable-after-construction TLS configuration.
class TlsContext {
  public:
    // Server context from PEM cert (chain) + private key files.
    static std::shared_ptr<TlsContext> serverFromFiles(const std::string& certFile,
                                                       const std::string& keyFile);
    // Server context from in-memory PEM (used by --tls-self-signed).
    static std::shared_ptr<TlsContext> serverFromPem(const std::string& certPem,
                                                     const std::string& keyPem);
    // Client context.
    static std::shared_ptr<TlsContext> client(const TlsClientOptions& options);

    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    TlsContextImpl& impl() noexcept { return *impl_; }

  private:
    explicit TlsContext(std::unique_ptr<TlsContextImpl> impl);
    std::unique_ptr<TlsContextImpl> impl_;
};

class TlsStreamImpl; // opaque

// A TLS session over an owned TcpSocket. Move-only, like TcpSocket. Implements
// the net::Stream interface so it drops into the framing/server/client code.
class TlsStream final : public Stream {
  public:
    // Wrap an accepted socket and run the server handshake (blocking; honors
    // the socket's recv timeout so a stalled peer is bounded). Throws TlsError.
    static TlsStream accept(std::shared_ptr<TlsContext> ctx, TcpSocket sock);
    // Wrap a connected socket and run the client handshake, verifying per the
    // context's policy. Throws TlsError (Verification on a trust failure).
    static TlsStream connect(std::shared_ptr<TlsContext> ctx, TcpSocket sock);

    ~TlsStream() override;
    TlsStream(TlsStream&&) noexcept;
    TlsStream& operator=(TlsStream&&) noexcept;
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Same contract as TcpSocket's equivalents (net::Stream interface).
    void sendAll(std::span<const uint8_t> data) override;
    RecvStatus recvExact(std::span<uint8_t> buf) override;
    void setRecvTimeout(int milliseconds) override;
    void shutdownBoth() noexcept override;
    bool isLoopbackPeer() const override;

    // SHA-256 of the peer's certificate (lowercase hex), for pin display/logs.
    std::string peerFingerprint() const;

  private:
    explicit TlsStream(std::unique_ptr<TlsStreamImpl> impl);
    std::unique_ptr<TlsStreamImpl> impl_;
};

// A PEM cert + key pair.
struct CertKeyPem {
    std::string certPem;
    std::string keyPem;
};

// Generate a self-signed certificate (EC P-256) valid for `days`, CN=`cn`,
// with a subjectAltName of `cn` so hostname verification works.
CertKeyPem generateSelfSigned(const std::string& cn, int days);

// SHA-256 fingerprint (lowercase hex, no separators) of a PEM certificate.
std::string certFingerprintSha256(const std::string& certPem);

// Write a PEM private key to `path`, restricted to the owner: created with mode
// 0600 on POSIX (no world/group access, even briefly); best-effort on Windows.
// Throws TlsError on failure.
void writePrivateKeyFile(const std::string& path, const std::string& keyPem);

} // namespace bisondb::net
