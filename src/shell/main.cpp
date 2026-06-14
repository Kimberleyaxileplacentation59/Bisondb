#include "core/version.hpp"
#include "shell/banner.hpp"
#include "shell/printer.hpp"
#include "shell/repl.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int usage() {
    std::cerr << "bisonsh " << bisondb::version() << " - BisonDB interactive shell\n\n"
              << "Usage: bisonsh [--connect host:port] [--no-color] [--no-banner]\n"
              << "               [--username <user>] [--token <token>]\n"
              << "               [--tls] [--tls-ca <pem>] [--tls-pin <sha256>] [--tls-insecure]\n"
              << "               [--eval '<stmt>[; <stmt>...]'] [-f script.bsh]\n\n"
              << "Default server: 127.0.0.1:27027. With --eval, -f, or piped stdin the\n"
              << "shell runs non-interactively and exits non-zero on the first error.\n\n"
              << "Auth: pass --username (you'll be prompted for the password, or set\n"
              << "BISONDB_PASSWORD) or --token (or set BISONDB_TOKEN). NEVER pass a password\n"
              << "on the command line. Once connected, manage accounts with 'auth ...'.\n\n"
              << "TLS: --tls verifies against the system trust store; --tls-ca <pem> trusts a\n"
              << "specific (self-signed) cert; --tls-pin <sha256> pins a fingerprint;\n"
              << "--tls-insecure skips verification (dev only, warns). Without --tls the\n"
              << "connection is unencrypted.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace bisondb::shell;
    ShellConfig config;
    std::string evalText;
    std::string scriptPath;
    bool noColor = false;
    bool noBanner = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "bisonsh: " << arg << " requires a value\n";
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--connect") {
            std::string endpoint = next();
            std::size_t colon = endpoint.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "bisonsh: --connect expects host:port\n";
                return usage();
            }
            config.host = endpoint.substr(0, colon);
            config.port = static_cast<uint16_t>(std::stoi(endpoint.substr(colon + 1)));
        } else if (arg == "--no-color") {
            noColor = true;
        } else if (arg == "--no-banner") {
            noBanner = true;
        } else if (arg == "--username" || arg == "-u") {
            config.username = next();
        } else if (arg == "--token") {
            config.token = next();
        } else if (arg == "--tls") {
            config.tls.enabled = true;
        } else if (arg == "--tls-ca") {
            config.tls.enabled = true;
            config.tls.verify = bisondb::net::TlsVerify::CaFile;
            config.tls.caFile = next();
        } else if (arg == "--tls-pin") {
            config.tls.enabled = true;
            config.tls.verify = bisondb::net::TlsVerify::Pin;
            config.tls.pinSha256 = next();
        } else if (arg == "--tls-insecure") {
            config.tls.enabled = true;
            config.tls.verify = bisondb::net::TlsVerify::Insecure;
        } else if (arg == "--eval") {
            evalText = next();
        } else if (arg == "-f") {
            scriptPath = next();
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "bisonsh: unknown flag " << arg << "\n";
            return usage();
        }
    }

    // Credentials never come from argv (process lists leak them): only env.
    if (const char* pw = std::getenv("BISONDB_PASSWORD"); pw != nullptr) {
        config.password = pw;
    }
    if (config.token.empty()) {
        if (const char* tok = std::getenv("BISONDB_TOKEN"); tok != nullptr) {
            config.token = tok;
        }
    }

    bool scripted = !evalText.empty() || !scriptPath.empty() || !stdinIsTty();
    config.interactive = !scripted;
    bool utf8Ok = enableUtf8Output();
    ColorProbe probe = systemProbe(noColor);
    ColorMode mode = scripted ? ColorMode::None : detectColorMode(probe);
    config.color = mode != ColorMode::None;
    if (config.interactive) {
        config.historyPath = homeDirectory() + "/.bisonsh_history";
        if (!noBanner && stdoutIsTty()) {
            config.bannerText =
                renderBanner(mode, bisondb::version(), "shell", wantAsciiBanner(probe, utf8Ok));
        }
    }

    try {
        Shell shell(config, std::cin, std::cout, std::cerr);
        if (!evalText.empty()) {
            return shell.evalString(evalText);
        }
        if (!scriptPath.empty()) {
            std::ifstream script(scriptPath);
            if (!script) {
                std::cerr << "bisonsh: cannot open " << scriptPath << "\n";
                return 2;
            }
            return shell.runScript(script);
        }
        if (!config.interactive) {
            return shell.runScript(std::cin);
        }
        return shell.runInteractive();
    } catch (const std::exception& e) {
        std::cerr << "bisonsh: " << e.what() << "\n";
        return 2;
    }
}
