#include "core/net/tls.hpp"

#include "core/platform.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mutex>
#include <psa/crypto.h>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    // <wincrypt.h> after <windows.h>
    #include <fstream>
    #include <wincrypt.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace bisondb::net {
namespace {

// Mbed-TLS 3.6's TLS 1.3 stack relies on PSA crypto, which must be initialized
// once per process before any handshake.
void ensurePsaInit() {
    static std::once_flag flag;
    static psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    std::call_once(flag, [] { status = psa_crypto_init(); });
    if (status != PSA_SUCCESS) {
        throw TlsError(TlsError::Kind::Config,
                       "psa_crypto_init failed (" + std::to_string(status) + ")");
    }
}

std::string mbedMessage(int ret) {
    char buf[160];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return std::string(buf) + " (-0x" + [&] {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%04X", static_cast<unsigned>(-ret));
        return std::string(hex);
    }() + ")";
}

std::string toHexLower(const unsigned char* data, std::size_t len) {
    static const char* d = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i] = d[data[i] >> 4];
        out[2 * i + 1] = d[data[i] & 0xF];
    }
    return out;
}

// Normalize a user-supplied fingerprint: strip ':' / whitespace, lowercase.
std::string normalizeFingerprint(std::string_view in) {
    std::string out;
    for (char c : in) {
        if (c == ':' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string sha256Hex(const unsigned char* der, std::size_t len) {
    unsigned char digest[32];
    mbedtls_sha256(der, len, digest, 0 /* = SHA-256, not 224 */);
    return toHexLower(digest, sizeof(digest));
}

// Load the OS trust store into `chain` (best effort) for TlsVerify::System.
void loadSystemTrust(mbedtls_x509_crt* chain) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) {
        return;
    }
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        mbedtls_x509_crt_parse_der(chain, ctx->pbCertEncoded, ctx->cbCertEncoded);
    }
    CertCloseStore(store, 0);
#else
    static const char* kBundles[] = {
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
        "/etc/ssl/cert.pem",                  // macOS/BSD
    };
    for (const char* path : kBundles) {
        if (mbedtls_x509_crt_parse_file(chain, path) == 0) {
            return;
        }
    }
    mbedtls_x509_crt_parse_path(chain, "/etc/ssl/certs");
#endif
}

std::string fmtUtc(std::time_t t) {
    std::tm tm{};
#if defined(BISONDB_PLATFORM_WINDOWS)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm);
    return buf;
}

} // namespace

// ─── TlsContextImpl ───────────────────────────────────────────────────────────

class TlsContextImpl {
  public:
    bool isServer = false;
    TlsClientOptions clientOpts;

    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt cert; // server: own chain; client: trust anchors
    mbedtls_pk_context pkey;

    TlsContextImpl() {
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&cert);
        mbedtls_pk_init(&pkey);
    }
    ~TlsContextImpl() {
        mbedtls_pk_free(&pkey);
        mbedtls_x509_crt_free(&cert);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_ssl_config_free(&conf);
    }
    TlsContextImpl(const TlsContextImpl&) = delete;
    TlsContextImpl& operator=(const TlsContextImpl&) = delete;

    void seedRng() {
        int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                        reinterpret_cast<const unsigned char*>("bisondb-tls"), 11);
        if (ret != 0) {
            throw TlsError(TlsError::Kind::Config, "RNG seed failed: " + mbedMessage(ret));
        }
    }
};

TlsContext::TlsContext(std::unique_ptr<TlsContextImpl> impl) : impl_(std::move(impl)) {}
TlsContext::~TlsContext() = default;

std::shared_ptr<TlsContext> TlsContext::serverFromPem(const std::string& certPem,
                                                      const std::string& keyPem) {
    ensurePsaInit();
    auto impl = std::make_unique<TlsContextImpl>();
    impl->isServer = true;
    impl->seedRng();

    // mbedtls PEM parsing requires a trailing NUL counted in the length.
    int ret = mbedtls_x509_crt_parse(
        &impl->cert, reinterpret_cast<const unsigned char*>(certPem.c_str()), certPem.size() + 1);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "cannot parse certificate: " + mbedMessage(ret));
    }
    ret = mbedtls_pk_parse_key(&impl->pkey, reinterpret_cast<const unsigned char*>(keyPem.c_str()),
                               keyPem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &impl->drbg);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "cannot parse private key: " + mbedMessage(ret));
    }

    ret = mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "config defaults failed: " + mbedMessage(ret));
    }
    mbedtls_ssl_conf_min_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    // TLS 1.3 server handshakes hit an internal error in this Mbed-TLS 3.6
    // build; cap at 1.2 (ECDHE + AES-GCM) until the 1.3 config is sorted.
    mbedtls_ssl_conf_max_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->drbg);
    ret = mbedtls_ssl_conf_own_cert(&impl->conf, &impl->cert, &impl->pkey);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "own cert failed: " + mbedMessage(ret));
    }
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

std::shared_ptr<TlsContext> TlsContext::serverFromFiles(const std::string& certFile,
                                                        const std::string& keyFile) {
    ensurePsaInit();
    auto impl = std::make_unique<TlsContextImpl>();
    impl->isServer = true;
    impl->seedRng();

    int ret = mbedtls_x509_crt_parse_file(&impl->cert, certFile.c_str());
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config,
                       "cannot read cert file " + certFile + ": " + mbedMessage(ret));
    }
    ret = mbedtls_pk_parse_keyfile(&impl->pkey, keyFile.c_str(), nullptr, mbedtls_ctr_drbg_random,
                                   &impl->drbg);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config,
                       "cannot read key file " + keyFile + ": " + mbedMessage(ret));
    }
    ret = mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "config defaults failed: " + mbedMessage(ret));
    }
    mbedtls_ssl_conf_min_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    // TLS 1.3 server handshakes hit an internal error in this Mbed-TLS 3.6
    // build; cap at 1.2 (ECDHE + AES-GCM) until the 1.3 config is sorted.
    mbedtls_ssl_conf_max_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->drbg);
    ret = mbedtls_ssl_conf_own_cert(&impl->conf, &impl->cert, &impl->pkey);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "own cert failed: " + mbedMessage(ret));
    }
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

std::shared_ptr<TlsContext> TlsContext::client(const TlsClientOptions& options) {
    ensurePsaInit();
    auto impl = std::make_unique<TlsContextImpl>();
    impl->isServer = false;
    impl->clientOpts = options;
    impl->seedRng();

    int ret = mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "config defaults failed: " + mbedMessage(ret));
    }
    mbedtls_ssl_conf_min_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    // TLS 1.3 server handshakes hit an internal error in this Mbed-TLS 3.6
    // build; cap at 1.2 (ECDHE + AES-GCM) until the 1.3 config is sorted.
    mbedtls_ssl_conf_max_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->drbg);

    switch (options.verify) {
    case TlsVerify::System:
        loadSystemTrust(&impl->cert);
        mbedtls_ssl_conf_ca_chain(&impl->conf, &impl->cert, nullptr);
        mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        break;
    case TlsVerify::CaFile: {
        int r = mbedtls_x509_crt_parse_file(&impl->cert, options.caFile.c_str());
        if (r != 0) {
            throw TlsError(TlsError::Kind::Config,
                           "cannot read CA file " + options.caFile + ": " + mbedMessage(r));
        }
        mbedtls_ssl_conf_ca_chain(&impl->conf, &impl->cert, nullptr);
        mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        break;
    }
    case TlsVerify::Pin:
    case TlsVerify::Insecure:
        // Chain verification is bypassed; Pin re-checks the fingerprint
        // after the handshake, Insecure checks nothing.
        mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_NONE);
        break;
    }
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

// ─── TlsStreamImpl ────────────────────────────────────────────────────────────

class TlsStreamImpl {
  public:
    std::shared_ptr<TlsContext> ctx;
    TcpSocket sock;
    mbedtls_ssl_context ssl;
    bool handshakeDone = false;

    explicit TlsStreamImpl(std::shared_ptr<TlsContext> c, TcpSocket s)
        : ctx(std::move(c)), sock(std::move(s)) {
        mbedtls_ssl_init(&ssl);
    }
    ~TlsStreamImpl() {
        if (handshakeDone) {
            mbedtls_ssl_close_notify(&ssl);
        }
        mbedtls_ssl_free(&ssl);
    }
    TlsStreamImpl(const TlsStreamImpl&) = delete;
    TlsStreamImpl& operator=(const TlsStreamImpl&) = delete;

    static int bioSend(void* p, const unsigned char* buf, size_t len) {
        int n = static_cast<TcpSocket*>(p)->sendRaw(buf, len);
        if (n >= 0) {
            return n;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED; // timeout/error → abort
    }
    static int bioRecv(void* p, unsigned char* buf, size_t len) {
        int n = static_cast<TcpSocket*>(p)->recvRaw(buf, len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0; // orderly close
        }
        return MBEDTLS_ERR_NET_RECV_FAILED; // timeout/error → abort
    }
};

TlsStream::TlsStream(std::unique_ptr<TlsStreamImpl> impl) : impl_(std::move(impl)) {}
TlsStream::~TlsStream() = default;
TlsStream::TlsStream(TlsStream&&) noexcept = default;
TlsStream& TlsStream::operator=(TlsStream&&) noexcept = default;

namespace {

void runHandshake(TlsStreamImpl& s) {
    int ret = mbedtls_ssl_setup(&s.ssl, &s.ctx->impl().conf);
    if (ret != 0) {
        throw TlsError(TlsError::Kind::Config, "ssl setup failed: " + mbedMessage(ret));
    }
    // BIO ctx is the TcpSocket inside this (heap-pinned) impl — stable address.
    mbedtls_ssl_set_bio(&s.ssl, &s.sock, &TlsStreamImpl::bioSend, &TlsStreamImpl::bioRecv, nullptr);

    while ((ret = mbedtls_ssl_handshake(&s.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&s.ssl);
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            std::string detail(info);
            while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) {
                detail.pop_back();
            }
            throw TlsError(TlsError::Kind::Verification,
                           "certificate verification failed: " + detail);
        }
        throw TlsError(TlsError::Kind::Handshake, "TLS handshake failed: " + mbedMessage(ret));
    }
    s.handshakeDone = true;
}

} // namespace

TlsStream TlsStream::accept(std::shared_ptr<TlsContext> ctx, TcpSocket sock) {
    auto impl = std::make_unique<TlsStreamImpl>(std::move(ctx), std::move(sock));
    runHandshake(*impl);
    return TlsStream(std::move(impl));
}

TlsStream TlsStream::connect(std::shared_ptr<TlsContext> ctx, TcpSocket sock) {
    auto impl = std::make_unique<TlsStreamImpl>(std::move(ctx), std::move(sock));
    const TlsClientOptions& opts = impl->ctx->impl().clientOpts;
    if (!opts.hostname.empty() &&
        (opts.verify == TlsVerify::System || opts.verify == TlsVerify::CaFile)) {
        mbedtls_ssl_set_hostname(&impl->ssl, opts.hostname.c_str());
    }
    runHandshake(*impl);

    if (opts.verify == TlsVerify::Pin) {
        std::string want = normalizeFingerprint(opts.pinSha256Hex);
        std::string got;
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&impl->ssl);
        if (peer != nullptr) {
            got = sha256Hex(peer->raw.p, peer->raw.len);
        }
        if (want.empty() || got != want) {
            throw TlsError(TlsError::Kind::Verification,
                           "certificate fingerprint does not match --tls-pin (server sent " + got +
                               ")");
        }
    }
    return TlsStream(std::move(impl));
}

void TlsStream::sendAll(std::span<const uint8_t> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int ret = mbedtls_ssl_write(&impl_->ssl, data.data() + sent, data.size() - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            throw NetError(NetError::Kind::OsError, "TLS write failed: " + mbedMessage(ret));
        }
        sent += static_cast<std::size_t>(ret);
    }
}

RecvStatus TlsStream::recvExact(std::span<uint8_t> buf) {
    std::size_t got = 0;
    while (got < buf.size()) {
        int ret = mbedtls_ssl_read(&impl_->ssl, buf.data() + got, buf.size() - got);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            if (got == 0) {
                return RecvStatus::Closed;
            }
            throw NetError(NetError::Kind::Closed, "TLS connection closed mid-message");
        }
        if (ret < 0) {
            throw NetError(NetError::Kind::OsError, "TLS read failed: " + mbedMessage(ret));
        }
        got += static_cast<std::size_t>(ret);
    }
    return RecvStatus::Complete;
}

void TlsStream::setRecvTimeout(int milliseconds) {
    impl_->sock.setRecvTimeout(milliseconds);
}

void TlsStream::shutdownBoth() noexcept {
    if (impl_ && impl_->handshakeDone) {
        mbedtls_ssl_close_notify(&impl_->ssl);
    }
    if (impl_) {
        impl_->sock.shutdownBoth();
    }
}

bool TlsStream::isLoopbackPeer() const {
    return impl_->sock.isLoopbackPeer();
}

std::string TlsStream::peerFingerprint() const {
    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&impl_->ssl);
    if (peer == nullptr) {
        return {};
    }
    return sha256Hex(peer->raw.p, peer->raw.len);
}

// ─── cert generation ──────────────────────────────────────────────────────────

CertKeyPem generateSelfSigned(const std::string& cn, int days) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    struct Guard {
        mbedtls_entropy_context* e;
        mbedtls_ctr_drbg_context* d;
        mbedtls_pk_context* k;
        mbedtls_x509write_cert* c;
        ~Guard() {
            mbedtls_x509write_crt_free(c);
            mbedtls_pk_free(k);
            mbedtls_ctr_drbg_free(d);
            mbedtls_entropy_free(e);
        }
    } guard{&entropy, &drbg, &key, &crt};

    auto fail = [](const std::string& what, int ret) -> CertKeyPem {
        throw TlsError(TlsError::Kind::Config, what + ": " + mbedMessage(ret));
    };

    int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>("bisondb-gencert"), 15);
    if (ret != 0) {
        return fail("RNG seed", ret);
    }
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        return fail("pk setup", ret);
    }
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                              &drbg);
    if (ret != 0) {
        return fail("EC keygen", ret);
    }

    using namespace std::chrono;
    std::time_t now = system_clock::to_time_t(system_clock::now());
    std::time_t notAfterT = now + static_cast<std::time_t>(days) * 24 * 3600;
    // Normal certs start 1 day ago (clock skew). For days<0 (an already-expired
    // cert, used in tests) keep notBefore < notAfter so the only defect is age.
    std::time_t notBeforeT = days >= 0 ? now - 24 * 3600 : notAfterT - 24 * 3600;
    std::string notBefore = fmtUtc(notBeforeT);
    std::string notAfter = fmtUtc(notAfterT);

    std::string subject = "CN=" + cn;
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key); // self-signed
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str())) != 0) {
        return fail("subject name", ret);
    }
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str())) != 0) {
        return fail("issuer name", ret);
    }
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    unsigned char serial[] = {0x01};
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0) {
        return fail("serial", ret);
    }
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, notBefore.c_str(), notAfter.c_str())) !=
        0) {
        return fail("validity", ret);
    }

    char pemBuf[4096];
    std::memset(pemBuf, 0, sizeof(pemBuf));
    ret = mbedtls_x509write_crt_pem(&crt, reinterpret_cast<unsigned char*>(pemBuf), sizeof(pemBuf),
                                    mbedtls_ctr_drbg_random, &drbg);
    if (ret != 0) {
        return fail("write cert", ret);
    }
    std::string certPem(pemBuf);

    std::memset(pemBuf, 0, sizeof(pemBuf));
    ret = mbedtls_pk_write_key_pem(&key, reinterpret_cast<unsigned char*>(pemBuf), sizeof(pemBuf));
    if (ret != 0) {
        return fail("write key", ret);
    }
    std::string keyPem(pemBuf);

    return CertKeyPem{certPem, keyPem};
}

void writePrivateKeyFile(const std::string& path, const std::string& keyPem) {
#if defined(BISONDB_PLATFORM_WINDOWS)
    // NTFS inherits ACLs from the directory; tightening them needs the Win32
    // security APIs. Best-effort here: just write the file.
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw TlsError(TlsError::Kind::Config, "cannot write key file: " + path);
    }
    f << keyPem;
    if (!f) {
        throw TlsError(TlsError::Kind::Config, "failed writing key file: " + path);
    }
#else
    // Create with 0600 from the start so the key is never world/group readable.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw TlsError(TlsError::Kind::Config, "cannot write key file: " + path);
    }
    ssize_t wrote = ::write(fd, keyPem.data(), keyPem.size());
    ::fchmod(fd, S_IRUSR | S_IWUSR); // enforce even if the file pre-existed
    ::close(fd);
    if (wrote != static_cast<ssize_t>(keyPem.size())) {
        throw TlsError(TlsError::Kind::Config, "short write to key file: " + path);
    }
#endif
}

std::string certFingerprintSha256(const std::string& certPem) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(certPem.c_str()),
                                     certPem.size() + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        throw TlsError(TlsError::Kind::Config, "cannot parse certificate: " + mbedMessage(ret));
    }
    std::string fp = sha256Hex(crt.raw.p, crt.raw.len);
    mbedtls_x509_crt_free(&crt);
    return fp;
}

} // namespace bisondb::net
