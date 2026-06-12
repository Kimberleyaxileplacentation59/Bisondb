#include "shell/banner.hpp"

#include "shell/printer.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bisondb::shell {

namespace {

// "ANSI Shadow" pixel style. Every line is exactly 54 code points wide
// (trailing spaces included) — test_banner verifies this.
constexpr std::string_view kLogo = "██████╗ ██╗███████╗ ██████╗ ███╗   ██╗██████╗ ██████╗ \n"
                                   "██╔══██╗██║██╔════╝██╔═══██╗████╗  ██║██╔══██╗██╔══██╗\n"
                                   "██████╔╝██║███████╗██║   ██║██╔██╗ ██║██║  ██║██████╔╝\n"
                                   "██╔══██╗██║╚════██║██║   ██║██║╚██╗██║██║  ██║██╔══██╗\n"
                                   "██████╔╝██║███████║╚██████╔╝██║ ╚████║██████╔╝██████╔╝\n"
                                   "╚═════╝ ╚═╝╚══════╝ ╚═════╝ ╚═╝  ╚═══╝╚═════╝ ╚═════╝ \n";

struct Rgb {
    int r, g, b;
};

// Dusk gradient, sky plum down to horizon amber.
constexpr std::array<Rgb, 6> kTrueColors = {{{183, 156, 216},
                                             {201, 139, 176},
                                             {214, 123, 130},
                                             {230, 138, 92},
                                             {243, 177, 78},
                                             {236, 155, 60}}};
constexpr std::array<int, 6> kXterm256 = {183, 175, 168, 209, 215, 214};
// 16-color: bright magenta (95) for the sky half, bright yellow (93) below.
constexpr std::array<int, 6> kBasic16 = {95, 95, 95, 93, 93, 93};

std::string lineColor(ColorMode mode, std::size_t line) {
    switch (mode) {
    case ColorMode::TrueColor: {
        const Rgb& c = kTrueColors[line];
        return "\033[38;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" +
               std::to_string(c.b) + "m";
    }
    case ColorMode::Xterm256: return "\033[38;5;" + std::to_string(kXterm256[line]) + "m";
    case ColorMode::Basic16: return "\033[" + std::to_string(kBasic16[line]) + "m";
    case ColorMode::None: return "";
    }
    return "";
}

std::vector<std::string_view> logoLines() {
    std::vector<std::string_view> lines;
    std::string_view rest = kLogo;
    while (!rest.empty()) {
        std::size_t nl = rest.find('\n');
        lines.push_back(rest.substr(0, nl));
        rest.remove_prefix(nl + 1);
    }
    return lines;
}

std::string asciiBanner(std::string_view version, std::string_view role) {
    std::string title = "BisonDB v" + std::string(version) + " (" + std::string(role) + ")";
    std::string border = "+" + std::string(title.size() + 2, '-') + "+";
    return border + "\n| " + title + " |\n" + border + "\n";
}

bool envEquals(const ColorProbe& probe, const char* name, std::string_view expected) {
    const char* value = probe.getEnv(name);
    return value != nullptr && expected == value;
}

} // namespace

ColorMode detectColorMode(const ColorProbe& probe) {
    if (!probe.isTty || probe.noColorFlag || probe.getEnv("NO_COLOR") != nullptr) {
        return ColorMode::None;
    }
    if (envEquals(probe, "COLORTERM", "truecolor") || envEquals(probe, "COLORTERM", "24bit")) {
        return ColorMode::TrueColor;
    }
    if (const char* term = probe.getEnv("TERM");
        term != nullptr && std::strstr(term, "256color") != nullptr) {
        return ColorMode::Xterm256;
    }
    if (probe.vtEnabled) {
        // Windows Terminal and modern conhost handle 24-bit color once VT
        // processing is on.
        return ColorMode::TrueColor;
    }
    return ColorMode::Basic16;
}

ColorProbe systemProbe(bool noColorFlag) {
    ColorProbe probe;
    probe.isTty = stdoutIsTty();
    probe.noColorFlag = noColorFlag;
    probe.vtEnabled = enableVtProcessing();
    probe.getEnv = [](const char* name) -> const char* { return std::getenv(name); };
    return probe;
}

std::string_view bannerLogo() {
    return kLogo;
}

bool wantAsciiBanner(const ColorProbe& probe, bool utf8Ok) {
    return envEquals(probe, "BISONDB_ASCII", "1") || !utf8Ok;
}

std::string renderBanner(ColorMode mode, std::string_view version, std::string_view role,
                         bool asciiOnly) {
    if (asciiOnly) {
        return asciiBanner(version, role);
    }
    std::string out;
    std::vector<std::string_view> lines = logoLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lineColor(mode, i);
        out += lines[i];
        if (mode != ColorMode::None) {
            out += "\033[0m";
        }
        out += "\n";
    }
    std::string tagline = "   ~  the prairie is open  ~        v" + std::string(version) +
                          " \xC2\xB7 " + std::string(role);
    if (mode != ColorMode::None) {
        out += "\033[2m" + tagline + "\033[0m\n";
    } else {
        out += tagline + "\n";
    }
    return out;
}

} // namespace bisondb::shell
