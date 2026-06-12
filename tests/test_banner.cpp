#include "shell/banner.hpp"
#include "shell/printer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <map>
#include <string>
#include <vector>

using namespace bisondb::shell;
using Catch::Matchers::ContainsSubstring;

namespace {

// Fake environment for detectColorMode.
ColorProbe probeWith(bool isTty, bool noColorFlag, bool vtEnabled,
                     std::map<std::string, std::string> env) {
    ColorProbe p;
    p.isTty = isTty;
    p.noColorFlag = noColorFlag;
    p.vtEnabled = vtEnabled;
    static std::map<std::string, std::string> stash; // outlives the probe
    stash = std::move(env);
    p.getEnv = [](const char* name) -> const char* {
        auto it = stash.find(name);
        return it == stash.end() ? nullptr : it->second.c_str();
    };
    return p;
}

std::size_t codePoints(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        lines.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

} // namespace

TEST_CASE("detectColorMode policy", "[banner]") {
    SECTION("not a TTY -> None") {
        REQUIRE(detectColorMode(probeWith(false, false, true, {{"COLORTERM", "truecolor"}})) ==
                ColorMode::None);
    }
    SECTION("--no-color -> None") {
        REQUIRE(detectColorMode(probeWith(true, true, true, {})) == ColorMode::None);
    }
    SECTION("NO_COLOR env -> None") {
        REQUIRE(detectColorMode(probeWith(true, false, true, {{"NO_COLOR", "1"}})) ==
                ColorMode::None);
    }
    SECTION("COLORTERM truecolor / 24bit -> TrueColor") {
        REQUIRE(detectColorMode(probeWith(true, false, false, {{"COLORTERM", "truecolor"}})) ==
                ColorMode::TrueColor);
        REQUIRE(detectColorMode(probeWith(true, false, false, {{"COLORTERM", "24bit"}})) ==
                ColorMode::TrueColor);
    }
    SECTION("TERM with 256color -> Xterm256") {
        REQUIRE(detectColorMode(probeWith(true, false, false, {{"TERM", "xterm-256color"}})) ==
                ColorMode::Xterm256);
    }
    SECTION("VT processing enabled (Windows) -> TrueColor") {
        REQUIRE(detectColorMode(probeWith(true, false, true, {})) == ColorMode::TrueColor);
    }
    SECTION("plain TTY -> Basic16") {
        REQUIRE(detectColorMode(probeWith(true, false, false, {{"TERM", "vt100"}})) ==
                ColorMode::Basic16);
    }
}

TEST_CASE("logo is six lines of equal display width", "[banner]") {
    std::vector<std::string_view> lines = splitLines(bannerLogo());
    REQUIRE(lines.size() == 6);
    std::size_t width = codePoints(lines[0]);
    REQUIRE(width == 54);
    for (std::string_view line : lines) {
        REQUIRE(codePoints(line) == width);
    }
}

TEST_CASE("rendered banner golden: None and TrueColor", "[banner]") {
    std::string plain = renderBanner(ColorMode::None, "0.1.0", "shell");
    std::vector<std::string_view> lines = splitLines(plain);
    REQUIRE(lines.size() == 7); // 6 logo + tagline
    REQUIRE_THAT(plain, ContainsSubstring("the prairie is open"));
    REQUIRE_THAT(plain, ContainsSubstring("v0.1.0"));
    REQUIRE_THAT(plain, ContainsSubstring("shell"));
    REQUIRE(plain.find('\033') == std::string::npos);

    std::string colored = renderBanner(ColorMode::TrueColor, "0.1.0", "server");
    // First line uses the lavender truecolor escape; tagline is dim.
    REQUIRE(colored.rfind("\033[38;2;183;156;216m", 0) == 0);
    REQUIRE_THAT(colored, ContainsSubstring("\033[38;2;236;155;60m")); // line 6 deep amber
    REQUIRE_THAT(colored, ContainsSubstring("\033[2m"));
    REQUIRE_THAT(colored, ContainsSubstring("server"));
    // Stripping ANSI gives exactly the plain rendering (modulo role text).
    REQUIRE(stripAnsi(colored) == renderBanner(ColorMode::None, "0.1.0", "server"));

    std::string x256 = renderBanner(ColorMode::Xterm256, "0.1.0", "shell");
    REQUIRE(x256.rfind("\033[38;5;183m", 0) == 0);
}

TEST_CASE("ASCII fallback contains no bytes >= 0x80", "[banner]") {
    std::string ascii = renderBanner(ColorMode::TrueColor, "0.1.0", "shell", /*asciiOnly=*/true);
    for (char c : ascii) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }
    REQUIRE_THAT(ascii, ContainsSubstring("BisonDB v0.1.0 (shell)"));
    REQUIRE_THAT(ascii, ContainsSubstring("+--"));
}

TEST_CASE("wantAsciiBanner policy", "[banner]") {
    REQUIRE(wantAsciiBanner(probeWith(true, false, false, {{"BISONDB_ASCII", "1"}}), true));
    REQUIRE(wantAsciiBanner(probeWith(true, false, false, {}), /*utf8Ok=*/false));
    REQUIRE_FALSE(wantAsciiBanner(probeWith(true, false, false, {}), true));
}
