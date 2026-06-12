#include "core/decimal128.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace bisondb {

namespace {

constexpr int64_t kExponentBias = 6176;
constexpr int64_t kMaxExponent = 6111;  // unbiased
constexpr int64_t kMinExponent = -6176; // unbiased
constexpr int kMaxDigits = 34;

// 128-bit unsigned integer as four little-endian 32-bit limbs; enough
// arithmetic for coefficient <-> decimal-digit conversion.
using Limbs = std::array<uint32_t, 4>;

bool isZero(const Limbs& x) {
    return x[0] == 0 && x[1] == 0 && x[2] == 0 && x[3] == 0;
}

void mul10Add(Limbs& x, uint32_t digit) {
    uint64_t carry = digit;
    for (uint32_t& limb : x) {
        uint64_t t = static_cast<uint64_t>(limb) * 10 + carry;
        limb = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
}

uint32_t divMod10(Limbs& x) {
    uint64_t rem = 0;
    for (int i = 3; i >= 0; --i) {
        uint64_t cur = (rem << 32) | x[static_cast<std::size_t>(i)];
        x[static_cast<std::size_t>(i)] = static_cast<uint32_t>(cur / 10);
        rem = cur % 10;
    }
    return static_cast<uint32_t>(rem);
}

std::string digitsOf(Limbs x) {
    if (isZero(x)) {
        return "0";
    }
    std::string out;
    while (!isZero(x)) {
        out.push_back(static_cast<char>('0' + divMod10(x)));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Formats sign/digits/exponent per the Extended JSON decimal128 rules:
// scientific notation when exponent > 0 or adjusted exponent < -6, plain
// decimal otherwise.
std::string formatDecimal(bool negative, const std::string& digits, int64_t exponent) {
    std::string out;
    if (negative) {
        out.push_back('-');
    }
    int64_t n = static_cast<int64_t>(digits.size());
    int64_t adjusted = exponent + n - 1;
    if (exponent > 0 || adjusted < -6) {
        out.push_back(digits[0]);
        if (n > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        out.push_back('E');
        out.push_back(adjusted >= 0 ? '+' : '-');
        out += std::to_string(adjusted >= 0 ? adjusted : -adjusted);
    } else if (exponent == 0) {
        out += digits;
    } else {
        int64_t pointPos = n + exponent; // exponent < 0 here
        if (pointPos > 0) {
            out.append(digits, 0, static_cast<std::size_t>(pointPos));
            out.push_back('.');
            out.append(digits, static_cast<std::size_t>(pointPos), std::string::npos);
        } else {
            out += "0.";
            out.append(static_cast<std::size_t>(-pointPos), '0');
            out += digits;
        }
    }
    return out;
}

} // namespace

std::string decimal128ToString(const Decimal128& value) {
    uint64_t low = 0;
    uint64_t high = 0;
    for (int i = 7; i >= 0; --i) {
        low = (low << 8) | value.bytes[static_cast<std::size_t>(i)];
        high = (high << 8) | value.bytes[static_cast<std::size_t>(i) + 8];
    }
    bool negative = (high >> 63) != 0;

    if ((high & 0x7800'0000'0000'0000ULL) == 0x7800'0000'0000'0000ULL) {
        if ((high & 0x7C00'0000'0000'0000ULL) == 0x7C00'0000'0000'0000ULL) {
            return "NaN"; // sign and payload are not rendered
        }
        return negative ? "-Infinity" : "Infinity";
    }

    int64_t biasedExponent;
    Limbs coeff{};
    if ((high & 0x6000'0000'0000'0000ULL) == 0x6000'0000'0000'0000ULL) {
        // Combination field starts with 11: the implied significand exceeds
        // 2^113, which is non-canonical; the value is zero.
        biasedExponent = static_cast<int64_t>((high >> 47) & 0x3FFF);
    } else {
        biasedExponent = static_cast<int64_t>((high >> 49) & 0x3FFF);
        uint64_t coeffHigh = high & 0x0001'FFFF'FFFF'FFFFULL; // 49 bits
        coeff = {static_cast<uint32_t>(low), static_cast<uint32_t>(low >> 32),
                 static_cast<uint32_t>(coeffHigh), static_cast<uint32_t>(coeffHigh >> 32)};
        // 10^34 - 1 == 0x0001ED09BEAD87C0'378D8E63FFFFFFFF; larger significands
        // are non-canonical and read as zero.
        constexpr uint64_t kMaxHigh = 0x0001'ED09'BEAD'87C0ULL;
        constexpr uint64_t kMaxLow = 0x378D'8E63'FFFF'FFFFULL;
        if (coeffHigh > kMaxHigh || (coeffHigh == kMaxHigh && low > kMaxLow)) {
            coeff = {};
        }
    }
    return formatDecimal(negative, digitsOf(coeff), biasedExponent - kExponentBias);
}

Decimal128 decimal128FromString(std::string_view text) {
    std::size_t pos = 0;
    auto fail = [&](const char* why) -> Decimal128 {
        throw TypeError(std::string("invalid decimal128 string \"") + std::string(text) +
                        "\": " + why);
    };

    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        ++pos;
    }

    auto matchesIgnoreCase = [&](std::string_view word) {
        if (text.size() - pos != word.size()) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            char a = text[pos + i];
            char b = word[i];
            if (a != b && a != (b ^ 0x20)) {
                return false;
            }
        }
        return true;
    };
    Decimal128 out;
    if (matchesIgnoreCase("nan")) {
        out.bytes[15] = 0x7C;
        return out;
    }
    if (matchesIgnoreCase("inf") || matchesIgnoreCase("infinity")) {
        out.bytes[15] = negative ? 0xF8 : 0x78;
        return out;
    }

    std::string digits;
    int64_t exponent = 0;
    bool sawDigit = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        digits.push_back(text[pos++]);
        sawDigit = true;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            digits.push_back(text[pos++]);
            --exponent;
            sawDigit = true;
        }
    }
    if (!sawDigit) {
        return fail("no digits");
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        bool expNegative = false;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            expNegative = text[pos] == '-';
            ++pos;
        }
        if (pos >= text.size()) {
            return fail("missing exponent digits");
        }
        int64_t e = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            e = e * 10 + (text[pos++] - '0');
            if (e > 1'000'000'000) { // far outside the representable range
                e = 1'000'000'000;
            }
        }
        exponent += expNegative ? -e : e;
    }
    if (pos != text.size()) {
        return fail("trailing characters");
    }

    // Normalize: strip leading zeros, then fold excess trailing zeros into the
    // exponent so up to 34 significant digits remain.
    std::size_t firstNonZero = digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos) {
        digits = "0";
    } else {
        digits.erase(0, firstNonZero);
    }
    while (digits.size() > kMaxDigits && digits.back() == '0') {
        digits.pop_back();
        ++exponent;
    }
    if (digits.size() > kMaxDigits) {
        return fail("more than 34 significant digits");
    }

    // Clamp the exponent into range where this is value-preserving.
    if (digits == "0") {
        exponent = std::clamp(exponent, kMinExponent, kMaxExponent);
    }
    while (exponent > kMaxExponent && digits.size() < kMaxDigits) {
        digits.push_back('0');
        --exponent;
    }
    while (exponent < kMinExponent && digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        ++exponent;
    }
    if (exponent > kMaxExponent) {
        return fail("exponent overflow");
    }
    if (exponent < kMinExponent) {
        return fail("exponent underflow");
    }

    Limbs coeff{};
    for (char c : digits) {
        mul10Add(coeff, static_cast<uint32_t>(c - '0'));
    }
    uint64_t low = coeff[0] | (static_cast<uint64_t>(coeff[1]) << 32);
    uint64_t high = coeff[2] | (static_cast<uint64_t>(coeff[3]) << 32);
    high |= static_cast<uint64_t>(exponent + kExponentBias) << 49;
    if (negative) {
        high |= 0x8000'0000'0000'0000ULL;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        out.bytes[i] = static_cast<uint8_t>(low >> (8 * i));
        out.bytes[i + 8] = static_cast<uint8_t>(high >> (8 * i));
    }
    return out;
}

} // namespace bisondb
