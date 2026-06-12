#pragma once

#include <string>

namespace bisondb::shell {

// ANSI palette used by the colorizer (also useful for callers printing
// errors/summaries).
namespace ansi {
inline constexpr const char* kReset = "\x1b[0m";
inline constexpr const char* kCyan = "\x1b[36m";
inline constexpr const char* kGreen = "\x1b[32m";
inline constexpr const char* kYellow = "\x1b[33m";
inline constexpr const char* kMagenta = "\x1b[35m";
inline constexpr const char* kDim = "\x1b[2m";
inline constexpr const char* kRed = "\x1b[31m";
} // namespace ansi

// Colorizes the output of toJson(..., pretty): keys cyan, strings green,
// numbers yellow, true/false/null magenta, $oid/$date payload strings dim.
// A thin token scanner over the printed text, not a printer fork. Returns
// the input untouched when color is false.
std::string colorizeJson(const std::string& prettyJson, bool color);

// Removes ANSI escape sequences (for tests and non-TTY assertions).
std::string stripAnsi(const std::string& text);

// Console shim (the only platform-specific code in the shell).
bool stdoutIsTty();
bool stdinIsTty();
// Returns whether ANSI/VT sequences will be honored (always true on POSIX).
bool enableVtProcessing();
// Switches the console to UTF-8 output (SetConsoleOutputCP on Windows;
// no-op true on POSIX). False means the caller should fall back to ASCII.
bool enableUtf8Output();
std::string homeDirectory();

} // namespace bisondb::shell
