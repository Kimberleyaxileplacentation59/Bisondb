#include "shell/printer.hpp"

#include "core/platform.hpp"

#include <cctype>
#include <cstdlib>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #define WIN32_LEAN_AND_MEAN
    #include <io.h>
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace bisondb::shell {

namespace {

bool isJsonNumberChar(char c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

} // namespace

std::string colorizeJson(const std::string& text, bool color) {
    if (!color) {
        return text;
    }
    std::string out;
    out.reserve(text.size() * 2);
    std::string lastKey;
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '"') {
            // Scan the whole string token (escapes included).
            std::size_t end = i + 1;
            bool escaped = false;
            std::string content;
            while (end < text.size()) {
                char s = text[end];
                if (escaped) {
                    escaped = false;
                } else if (s == '\\') {
                    escaped = true;
                } else if (s == '"') {
                    break;
                }
                content.push_back(s);
                ++end;
            }
            std::string token = text.substr(i, end - i + 1);
            // A string followed by ':' is a key.
            std::size_t after = end + 1;
            while (after < text.size() && text[after] == ' ') {
                ++after;
            }
            bool isKey = after < text.size() && text[after] == ':';
            if (isKey) {
                out += ansi::kCyan;
                out += token;
                out += ansi::kReset;
                lastKey = content;
            } else {
                bool dim = lastKey == "$oid" || lastKey == "$date";
                out += dim ? ansi::kDim : ansi::kGreen;
                out += token;
                out += ansi::kReset;
            }
            i = end + 1;
            continue;
        }
        if (isJsonNumberChar(c) && (c == '-' || std::isdigit(static_cast<unsigned char>(c)))) {
            std::size_t end = i;
            while (end < text.size() && isJsonNumberChar(text[end])) {
                ++end;
            }
            out += ansi::kYellow;
            out += text.substr(i, end - i);
            out += ansi::kReset;
            i = end;
            continue;
        }
        if (c == 't' && text.compare(i, 4, "true") == 0) {
            out += ansi::kMagenta;
            out += "true";
            out += ansi::kReset;
            i += 4;
            continue;
        }
        if (c == 'f' && text.compare(i, 5, "false") == 0) {
            out += ansi::kMagenta;
            out += "false";
            out += ansi::kReset;
            i += 5;
            continue;
        }
        if (c == 'n' && text.compare(i, 4, "null") == 0) {
            out += ansi::kMagenta;
            out += "null";
            out += ansi::kReset;
            i += 4;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::string stripAnsi(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && !std::isalpha(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            ++i; // the final letter
            continue;
        }
        out.push_back(text[i++]);
    }
    return out;
}

#if defined(BISONDB_PLATFORM_WINDOWS)

bool stdoutIsTty() {
    return _isatty(_fileno(stdout)) != 0;
}
bool stdinIsTty() {
    return _isatty(_fileno(stdin)) != 0;
}

bool enableVtProcessing() {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
    return false;
}

bool enableUtf8Output() {
    return SetConsoleOutputCP(CP_UTF8) != 0;
}

std::string homeDirectory() {
    const char* profile = std::getenv("USERPROFILE");
    return profile != nullptr ? profile : ".";
}

#else

bool stdoutIsTty() {
    return isatty(1) != 0;
}
bool stdinIsTty() {
    return isatty(0) != 0;
}
bool enableVtProcessing() {
    return true;
}
bool enableUtf8Output() {
    return true;
}

std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : ".";
}

#endif

} // namespace bisondb::shell
