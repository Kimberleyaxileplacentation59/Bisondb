#include "core/platform.hpp"
#include "core/version.hpp"
#include "server/server.hpp"
#include "shell/banner.hpp"
#include "shell/printer.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace {

bisondb::server::Server* g_server = nullptr;

#if defined(BISONDB_PLATFORM_WINDOWS)
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_server != nullptr) {
            g_server->requestStop();
        }
        return TRUE;
    }
    return FALSE;
}
void installSignalHandlers() {
    SetConsoleCtrlHandler(consoleHandler, TRUE);
}
#else
extern "C" void signalHandler(int) {
    if (g_server != nullptr) {
        g_server->requestStop();
    }
}
void installSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}
#endif

int usage() {
    std::cerr << "bisond " << bisondb::version() << " - BisonDB server\n\n"
              << "Usage: bisond --dir <dbdir> [--port N] [--bind ADDR] [--threads N] [--quiet]\n\n"
              << "  --dir     database directory (required)\n"
              << "  --port    TCP port (default 27027)\n"
              << "  --bind    bind address (default 127.0.0.1)\n"
              << "  --threads worker threads (default: hardware concurrency)\n"
              << "  --quiet   suppress per-request logging\n\n"
              << "WARNING: bisond has NO authentication and NO TLS. It binds to loopback by\n"
              << "default; binding to any other address exposes the full database to the\n"
              << "network and is at the operator's own risk.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    bisondb::server::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "bisond: " << arg << " requires a value\n";
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--dir") {
            config.dir = next();
        } else if (arg == "--port") {
            config.port = static_cast<uint16_t>(std::stoi(next()));
        } else if (arg == "--bind") {
            config.bind = next();
        } else if (arg == "--threads") {
            config.threads = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--quiet") {
            config.quiet = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "bisond: unknown flag " << arg << "\n";
            return usage();
        }
    }
    if (config.dir.empty()) {
        std::cerr << "bisond: --dir is required\n";
        return usage();
    }
    if (config.bind != "127.0.0.1" && !config.quiet) {
        std::cerr << "WARNING: binding to " << config.bind
                  << " exposes the database without authentication or TLS.\n";
    }

    if (!config.quiet) {
        using namespace bisondb::shell;
        bool utf8Ok = enableUtf8Output();
        ColorProbe probe = systemProbe(/*noColorFlag=*/false);
        std::cerr << renderBanner(detectColorMode(probe), bisondb::version(), "server",
                                  wantAsciiBanner(probe, utf8Ok));
    }

    try {
        bisondb::server::Server server(std::move(config));
        g_server = &server;
        installSignalHandlers();
        server.start();
        server.waitUntilStopped();
        g_server = nullptr;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bisond: " << e.what() << "\n";
        return 2;
    }
}
