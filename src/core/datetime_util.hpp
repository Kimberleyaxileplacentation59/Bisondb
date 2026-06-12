#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bisondb::detail {

// Civil-calendar conversions (proleptic Gregorian), after Howard Hinnant's
// public-domain date algorithms.

inline int64_t daysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  // [0, 399]
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

struct CivilDate {
    int64_t year;
    int month;
    int day;
};

inline CivilDate civilFromDays(int64_t z) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                      // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                      // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
    int64_t m = mp + (mp < 10 ? 3 : -9);                   // [1, 12]
    return CivilDate{y + (m <= 2), static_cast<int>(m), static_cast<int>(d)};
}

inline int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Formats ms-since-epoch as "YYYY-MM-DDTHH:MM:SS(.mmm)?Z", omitting the
// millisecond part when zero. Returns nullopt when the year falls outside
// [1970, 9999].
inline std::optional<std::string> formatDateTimeIso(int64_t msSinceEpoch) {
    int64_t days = floorDiv(msSinceEpoch, 86'400'000);
    int64_t msod = msSinceEpoch - days * 86'400'000;
    CivilDate cd = civilFromDays(days);
    if (cd.year < 1970 || cd.year > 9999) {
        return std::nullopt;
    }
    int ms = static_cast<int>(msod % 1000);
    int sec = static_cast<int>(msod / 1000 % 60);
    int min = static_cast<int>(msod / 60'000 % 60);
    int hour = static_cast<int>(msod / 3'600'000);

    std::string out(ms == 0 ? 20 : 24, '0');
    auto put = [&out](std::size_t at, int width, int64_t value) {
        for (int k = width - 1; k >= 0; --k) {
            out[at + static_cast<std::size_t>(k)] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
    };
    put(0, 4, cd.year);
    out[4] = '-';
    put(5, 2, cd.month);
    out[7] = '-';
    put(8, 2, cd.day);
    out[10] = 'T';
    put(11, 2, hour);
    out[13] = ':';
    put(14, 2, min);
    out[16] = ':';
    put(17, 2, sec);
    if (ms != 0) {
        out[19] = '.';
        put(20, 3, ms);
    }
    out.back() = 'Z';
    return out;
}

// Parses "YYYY-MM-DDTHH:MM:SS(.f+)?(Z|+HH:MM|-HH:MM|+HHMM|-HHMM)". Fractional
// digits beyond milliseconds are truncated. Returns nullopt on syntax errors
// or out-of-range fields.
inline std::optional<int64_t> parseDateTimeIso(std::string_view s) {
    std::size_t pos = 0;
    auto digits = [&](int count, int64_t& out) -> bool {
        out = 0;
        for (int k = 0; k < count; ++k) {
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
                return false;
            }
            out = out * 10 + (s[pos++] - '0');
        }
        return true;
    };
    auto expect = [&](char c) -> bool { return pos < s.size() && s[pos++] == c; };

    int64_t year, month, day, hour, min, sec;
    if (!digits(4, year) || !expect('-') || !digits(2, month) || !expect('-') || !digits(2, day) ||
        !expect('T') || !digits(2, hour) || !expect(':') || !digits(2, min) || !expect(':') ||
        !digits(2, sec)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        return std::nullopt;
    }
    int64_t ms = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int seen = 0;
        int64_t scale = 100;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (seen < 3) {
                ms += (s[pos] - '0') * scale;
                scale /= 10;
            }
            ++seen;
            ++pos;
        }
        if (seen == 0) {
            return std::nullopt;
        }
    }
    int64_t offsetMinutes = 0;
    if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) {
        ++pos;
    } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        bool negative = s[pos++] == '-';
        int64_t oh, om;
        if (!digits(2, oh)) {
            return std::nullopt;
        }
        if (pos < s.size() && s[pos] == ':') {
            ++pos;
        }
        if (!digits(2, om)) {
            return std::nullopt;
        }
        if (oh > 23 || om > 59) {
            return std::nullopt;
        }
        offsetMinutes = (negative ? -1 : 1) * (oh * 60 + om);
    } else {
        return std::nullopt;
    }
    if (pos != s.size()) {
        return std::nullopt;
    }
    int64_t result = daysFromCivil(year, static_cast<int>(month), static_cast<int>(day));
    result = ((result * 24 + hour) * 60 + min - offsetMinutes) * 60 + sec;
    return result * 1000 + ms;
}

} // namespace bisondb::detail
