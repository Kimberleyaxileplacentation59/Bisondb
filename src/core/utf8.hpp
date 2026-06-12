#pragma once

#include <cstdint>
#include <string_view>

namespace bisondb::detail {

// Strict UTF-8 validation: rejects truncated sequences, overlong encodings,
// UTF-16 surrogate code points, and code points above U+10FFFF.
inline bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        uint8_t b0 = static_cast<uint8_t>(s[i]);
        std::size_t len = 0;
        uint32_t cp = 0;
        if (b0 < 0x80) {
            ++i;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            uint8_t b = static_cast<uint8_t>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += len;
    }
    return true;
}

} // namespace bisondb::detail
