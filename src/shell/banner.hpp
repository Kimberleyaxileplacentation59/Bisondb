#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace bisondb::shell {

enum class ColorMode { None, Basic16, Xterm256, TrueColor };

// Injectable environment for detectColorMode so the policy is unit-testable
// without touching the real TTY or process environment.
struct ColorProbe {
    bool isTty = false;
    bool noColorFlag = false; // --no-color
    bool vtEnabled = false;   // Windows VT processing successfully enabled
    std::function<const char*(const char*)> getEnv = [](const char*) -> const char* {
        return nullptr;
    };
};

// None when not a TTY, --no-color, or NO_COLOR is set; TrueColor when
// COLORTERM is "truecolor"/"24bit" (or VT processing was enabled on
// Windows); Xterm256 when TERM contains "256color"; Basic16 otherwise.
ColorMode detectColorMode(const ColorProbe& probe);

// Probe wired to the real environment/console shims.
ColorProbe systemProbe(bool noColorFlag);

// The 6-line BISONDB logo (no colors, no tagline). Exposed for tests.
std::string_view bannerLogo();

// Full banner: gradient-colored logo plus the dim tagline
//   ~  the prairie is open  ~        v<version> · <role>
// With asciiOnly (BISONDB_ASCII=1 or codepage setup failure) a plain
// ASCII box is rendered instead.
std::string renderBanner(ColorMode mode, std::string_view version, std::string_view role,
                         bool asciiOnly = false);

// True when the ASCII fallback should be used: BISONDB_ASCII=1 or the
// console could not be switched to UTF-8 output.
bool wantAsciiBanner(const ColorProbe& probe, bool utf8Ok);

} // namespace bisondb::shell
