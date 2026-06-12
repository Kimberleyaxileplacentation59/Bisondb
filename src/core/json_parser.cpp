#include "core/json_parser.hpp"

#include "core/datetime_util.hpp"
#include "core/decimal128.hpp"
#include "core/utf8.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace bisondb {

namespace {

constexpr int kMaxDepth = 200;

class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parseTopLevel(bool requireEnd) {
        skipWhitespace();
        Value v = parseValue(0);
        if (requireEnd) {
            skipWhitespace();
            if (pos_ != text_.size()) {
                fail("trailing characters after JSON value");
            }
        }
        return v;
    }

    std::size_t consumed() const noexcept { return pos_; }

  private:
    [[noreturn]] void fail(const std::string& reason) const {
        throw JsonParseError(line_, column_, reason);
    }

    bool atEnd() const noexcept { return pos_ >= text_.size(); }

    char peek() const {
        if (atEnd()) {
            fail("unexpected end of input");
        }
        return text_[pos_];
    }

    char advance() {
        char c = peek();
        ++pos_;
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void expect(char c, const char* what) {
        if (atEnd() || peek() != c) {
            fail(std::string("expected ") + what);
        }
        advance();
    }

    void skipWhitespace() {
        while (!atEnd()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else {
                break;
            }
        }
    }

    bool consumeKeyword(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            advance();
        }
        return true;
    }

    Value parseValue(int depth) {
        if (depth >= kMaxDepth) {
            fail("nesting depth exceeds limit of 200");
        }
        char c = peek();
        switch (c) {
        case '{': return parseObject(depth);
        case '[': return parseArray(depth);
        case '"': return Value(parseString());
        case 't':
            if (consumeKeyword("true")) {
                return Value(true);
            }
            fail("invalid literal");
        case 'f':
            if (consumeKeyword("false")) {
                return Value(false);
            }
            fail("invalid literal");
        case 'n':
            if (consumeKeyword("null")) {
                return Value();
            }
            fail("invalid literal");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber();
            }
            fail(std::string("unexpected character '") + c + "'");
        }
    }

    Value parseObject(int depth) {
        std::size_t startLine = line_;
        std::size_t startColumn = column_;
        expect('{', "'{'");
        Document doc;
        skipWhitespace();
        if (!atEnd() && peek() == '}') {
            advance();
            return Value(std::move(doc));
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected string key");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':', "':' after object key");
            skipWhitespace();
            Value v = parseValue(depth + 1);
            doc.append(std::move(key), std::move(v));
            skipWhitespace();
            char c = peek();
            if (c == ',') {
                advance();
                continue;
            }
            if (c == '}') {
                advance();
                break;
            }
            fail("expected ',' or '}' in object");
        }
        return foldExtendedJson(std::move(doc), startLine, startColumn);
    }

    Value parseArray(int depth) {
        expect('[', "'['");
        Array arr;
        skipWhitespace();
        if (!atEnd() && peek() == ']') {
            advance();
            return Value(std::move(arr));
        }
        while (true) {
            skipWhitespace();
            arr.push_back(parseValue(depth + 1));
            skipWhitespace();
            char c = peek();
            if (c == ',') {
                advance();
                continue;
            }
            if (c == ']') {
                advance();
                break;
            }
            fail("expected ',' or ']' in array");
        }
        return Value(std::move(arr));
    }

    uint32_t parseHex4() {
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = advance();
            cp <<= 4;
            if (c >= '0' && c <= '9') {
                cp |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                cp |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                cp |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid \\u escape: expected 4 hex digits");
            }
        }
        return cp;
    }

    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string parseString() {
        expect('"', "'\"'");
        std::string out;
        while (true) {
            char c = advance();
            if (c == '"') {
                break;
            }
            if (static_cast<uint8_t>(c) < 0x20) {
                fail("unescaped control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            char e = advance();
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = parseHex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate: must be followed by \uDC00-\uDFFF.
                    if (atEnd() || peek() != '\\') {
                        fail("unpaired UTF-16 high surrogate");
                    }
                    advance();
                    if (atEnd() || peek() != 'u') {
                        fail("unpaired UTF-16 high surrogate");
                    }
                    advance();
                    uint32_t low = parseHex4();
                    if (low < 0xDC00 || low > 0xDFFF) {
                        fail("invalid UTF-16 low surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("unpaired UTF-16 low surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("invalid escape sequence");
            }
        }
        if (!detail::isValidUtf8(out)) {
            fail("string is not valid UTF-8");
        }
        return out;
    }

    Value parseNumber() {
        std::size_t start = pos_;
        bool isInteger = true;
        if (peek() == '-') {
            advance();
        }
        if (atEnd() || peek() < '0' || peek() > '9') {
            fail("invalid number: expected digit");
        }
        if (peek() == '0') {
            advance();
        } else {
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                advance();
            }
        }
        if (!atEnd() && peek() == '.') {
            isInteger = false;
            advance();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("invalid number: expected digit after '.'");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                advance();
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            isInteger = false;
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("invalid number: expected exponent digit");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                advance();
            }
        }
        std::string_view token = text_.substr(start, pos_ - start);
        if (isInteger) {
            int64_t i = 0;
            auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), i);
            if (ec == std::errc() && p == token.data() + token.size()) {
                if (i >= std::numeric_limits<int32_t>::min() &&
                    i <= std::numeric_limits<int32_t>::max()) {
                    return Value(static_cast<int32_t>(i));
                }
                return Value(i);
            }
            // Falls through for integers outside the int64 range.
        }
        double d = 0;
        auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), d);
        if (ec != std::errc{} && ec != std::errc::result_out_of_range) {
            fail("invalid number");
        }
        (void)p;
        return Value(d);
    }

    // ---- Extended JSON v2 wrapper folding -----------------------------------

    [[noreturn]] void failAt(std::size_t line, std::size_t column, const std::string& reason) {
        throw JsonParseError(line, column, reason);
    }

    Value foldExtendedJson(Document doc, std::size_t line, std::size_t column) {
        if (doc.size() != 1) {
            return Value(std::move(doc));
        }
        const std::string& key = doc[0].first;
        Value& inner = doc[0].second;
        if (key == "$oid") {
            if (!inner.is<std::string>()) {
                failAt(line, column, "$oid requires a string value");
            }
            try {
                return Value(ObjectId::fromHex(inner.get<std::string>()));
            } catch (const TypeError& e) {
                failAt(line, column, e.what());
            }
        }
        if (key == "$numberInt") {
            return Value(parseWrappedInt<int32_t>(inner, "$numberInt", line, column));
        }
        if (key == "$numberLong") {
            return Value(parseWrappedInt<int64_t>(inner, "$numberLong", line, column));
        }
        if (key == "$numberDouble") {
            if (!inner.is<std::string>()) {
                failAt(line, column, "$numberDouble requires a string value");
            }
            const std::string& s = inner.get<std::string>();
            if (s == "NaN") {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            if (s == "Infinity") {
                return Value(std::numeric_limits<double>::infinity());
            }
            if (s == "-Infinity") {
                return Value(-std::numeric_limits<double>::infinity());
            }
            double d = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
            if ((ec != std::errc{} && ec != std::errc::result_out_of_range) ||
                p != s.data() + s.size() || s.empty()) {
                failAt(line, column, "$numberDouble requires a numeric string");
            }
            return Value(d);
        }
        if (key == "$numberDecimal") {
            if (!inner.is<std::string>()) {
                failAt(line, column, "$numberDecimal requires a string value");
            }
            try {
                return Value(decimal128FromString(inner.get<std::string>()));
            } catch (const TypeError& e) {
                failAt(line, column, e.what());
            }
        }
        if (key == "$date") {
            if (inner.is<std::string>()) {
                if (auto ms = detail::parseDateTimeIso(inner.get<std::string>())) {
                    return Value(DateTime{*ms});
                }
                failAt(line, column, "$date requires an ISO-8601 datetime string");
            }
            // {"$date": {"$numberLong": "..."}} has already been folded to Int64.
            if (inner.is<int64_t>()) {
                return Value(DateTime{inner.get<int64_t>()});
            }
            if (inner.is<int32_t>()) {
                return Value(DateTime{inner.get<int32_t>()});
            }
            failAt(line, column, "$date requires an ISO-8601 string or {\"$numberLong\": ...}");
        }
        // Unknown $-keys stay as plain documents.
        return Value(std::move(doc));
    }

    template <typename T>
    T parseWrappedInt(const Value& inner, const char* wrapper, std::size_t line,
                      std::size_t column) {
        if (!inner.is<std::string>()) {
            failAt(line, column, std::string(wrapper) + " requires a string value");
        }
        const std::string& s = inner.get<std::string>();
        T i = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), i);
        if (ec != std::errc{} || p != s.data() + s.size() || s.empty()) {
            failAt(line, column, std::string(wrapper) + " requires an in-range integer string");
        }
        return i;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

} // namespace

Value parseJson(std::string_view text) {
    Parser p(text);
    return p.parseTopLevel(/*requireEnd=*/true);
}

Value parseJsonOne(std::string_view text, std::size_t& bytesConsumed) {
    Parser p(text);
    Value v = p.parseTopLevel(/*requireEnd=*/false);
    bytesConsumed = p.consumed();
    return v;
}

} // namespace bisondb
