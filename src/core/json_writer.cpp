#include "core/json_writer.hpp"

#include "core/datetime_util.hpp"
#include "core/decimal128.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace bisondb {

namespace {

void appendEscaped(std::string& out, std::string_view s) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (char c : s) {
        uint8_t b = static_cast<uint8_t>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (b < 0x20) {
            out += "\\u00";
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0x0F]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

// Shortest round-trip formatting; integral doubles get a ".0" suffix so the
// double-ness survives a JSON round trip.
std::string doubleToString(double d) {
    if (std::isnan(d)) {
        return "NaN";
    }
    if (std::isinf(d)) {
        return d < 0 ? "-Infinity" : "Infinity";
    }
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, end);
    if (s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }
    return s;
}

class Writer {
  public:
    Writer(JsonMode mode, bool pretty) : mode_(mode), pretty_(pretty) {}

    std::string take() && { return std::move(out_); }

    void writeValue(const Value& v) {
        switch (v.type()) {
        case Type::Null: out_ += "null"; break;
        case Type::Bool: out_ += v.get<bool>() ? "true" : "false"; break;
        case Type::String: appendEscaped(out_, v.get<std::string>()); break;
        case Type::Document: writeDocument(v.asDocument()); break;
        case Type::Array: writeArray(v.asArray()); break;
        case Type::Int32: writeInt32(v.get<int32_t>()); break;
        case Type::Int64: writeInt64(v.get<int64_t>()); break;
        case Type::Double: writeDouble(v.get<double>()); break;
        case Type::ObjectId: writeWrapped("$oid", v.get<ObjectId>().toHex()); break;
        case Type::DateTime: writeDateTime(v.get<DateTime>()); break;
        case Type::Decimal128:
            writeWrapped("$numberDecimal", decimal128ToString(v.get<Decimal128>()));
            break;
        }
    }

  private:
    void newlineIndent(int depth) {
        if (pretty_) {
            out_.push_back('\n');
            out_.append(static_cast<std::size_t>(depth) * 2, ' ');
        }
    }

    void writeKey(std::string_view key) {
        appendEscaped(out_, key);
        out_.push_back(':');
        if (pretty_) {
            out_.push_back(' ');
        }
    }

    void writeDocument(const Document& doc) {
        if (doc.empty()) {
            out_ += "{}";
            return;
        }
        out_.push_back('{');
        ++depth_;
        bool first = true;
        for (const auto& [key, field] : doc) {
            if (!first) {
                out_.push_back(',');
            }
            first = false;
            newlineIndent(depth_);
            writeKey(key);
            writeValue(field);
        }
        --depth_;
        newlineIndent(depth_);
        out_.push_back('}');
    }

    void writeArray(const Array& arr) {
        if (arr.empty()) {
            out_ += "[]";
            return;
        }
        out_.push_back('[');
        ++depth_;
        bool first = true;
        for (const Value& v : arr) {
            if (!first) {
                out_.push_back(',');
            }
            first = false;
            newlineIndent(depth_);
            writeValue(v);
        }
        --depth_;
        newlineIndent(depth_);
        out_.push_back(']');
    }

    // {"$key": "text"} wrapper, written inline regardless of pretty mode.
    void writeWrapped(std::string_view key, std::string_view text) {
        out_.push_back('{');
        appendEscaped(out_, key);
        out_.push_back(':');
        if (pretty_) {
            out_.push_back(' ');
        }
        appendEscaped(out_, text);
        out_.push_back('}');
    }

    void writeInt32(int32_t i) {
        if (mode_ == JsonMode::Canonical) {
            writeWrapped("$numberInt", std::to_string(i));
        } else {
            out_ += std::to_string(i);
        }
    }

    void writeInt64(int64_t i) {
        if (mode_ == JsonMode::Canonical) {
            writeWrapped("$numberLong", std::to_string(i));
        } else {
            out_ += std::to_string(i);
        }
    }

    void writeDouble(double d) {
        if (mode_ == JsonMode::Canonical || std::isnan(d) || std::isinf(d)) {
            writeWrapped("$numberDouble", doubleToString(d));
        } else {
            out_ += doubleToString(d);
        }
    }

    void writeDateTime(DateTime dt) {
        if (mode_ == JsonMode::Relaxed) {
            if (auto iso = detail::formatDateTimeIso(dt.msSinceEpoch)) {
                writeWrapped("$date", *iso);
                return;
            }
        }
        // Canonical mode, or relaxed with the year outside 1970-9999:
        // {"$date": {"$numberLong": "<ms>"}}
        out_.push_back('{');
        appendEscaped(out_, "$date");
        out_.push_back(':');
        if (pretty_) {
            out_.push_back(' ');
        }
        writeWrapped("$numberLong", std::to_string(dt.msSinceEpoch));
        out_.push_back('}');
    }

    JsonMode mode_;
    bool pretty_;
    int depth_ = 0;
    std::string out_;
};

} // namespace

std::string toJson(const Value& v, JsonMode mode, bool pretty) {
    Writer w(mode, pretty);
    w.writeValue(v);
    return std::move(w).take();
}

} // namespace bisondb
