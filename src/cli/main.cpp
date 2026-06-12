#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"
#include "core/platform.hpp"
#include "core/version.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(BISONDB_PLATFORM_WINDOWS)
    // Required so BSON written to stdout is not mangled by LF -> CRLF
    // translation. This is the only platform-specific code in the CLI.
    #include <fcntl.h>
    #include <io.h>
#endif

namespace {

using namespace bisondb;

void setStdoutBinary() {
#if defined(BISONDB_PLATFORM_WINDOWS)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

int usage(std::ostream& os) {
    os << "bisonc " << version() << " - BSON/JSON conversion tool\n"
       << "\n"
       << "Usage:\n"
       << "  bisonc to-json <input.bson> [-o out.json] [--canonical] [--pretty]\n"
       << "  bisonc to-bson <input.json> [-o out.bson]\n"
       << "  bisonc inspect <input.bson>\n"
       << "\n"
       << "to-json reads one or more concatenated BSON documents and writes one JSON\n"
       << "document per line (JSON Lines), or indented documents with --pretty.\n"
       << "to-bson accepts a single JSON document or JSON Lines and writes\n"
       << "concatenated BSON documents. Without -o, output goes to stdout.\n";
    return os.rdbuf() == std::cout.rdbuf() ? 0 : 1;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input file: " + path);
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::runtime_error("failed reading input file: " + path);
    }
    return data;
}

struct Options {
    std::string input;
    std::optional<std::string> output;
    bool canonical = false;
    bool pretty = false;
};

Options parseOptions(std::span<char*> args, bool allowJsonFlags) {
    Options opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view a = args[i];
        if (a == "-o") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("-o requires a file argument");
            }
            opts.output = args[++i];
        } else if (allowJsonFlags && a == "--canonical") {
            opts.canonical = true;
        } else if (allowJsonFlags && a == "--pretty") {
            opts.pretty = true;
        } else if (!a.empty() && a[0] == '-') {
            throw std::runtime_error("unknown option: " + std::string(a));
        } else if (opts.input.empty()) {
            opts.input = a;
        } else {
            throw std::runtime_error("unexpected argument: " + std::string(a));
        }
    }
    if (opts.input.empty()) {
        throw std::runtime_error("missing input file");
    }
    return opts;
}

// Opens the -o file, or configures and returns stdout.
std::ostream& openOutput(const Options& opts, std::ofstream& file, bool binary) {
    if (opts.output) {
        file.open(*opts.output, binary ? std::ios::binary : std::ios::out);
        if (!file) {
            throw std::runtime_error("cannot open output file: " + *opts.output);
        }
        return file;
    }
    if (binary) {
        setStdoutBinary();
    }
    return std::cout;
}

int cmdToJson(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/true);
    std::vector<uint8_t> data = readFileBytes(opts.input);
    std::ofstream file;
    std::ostream& out = openOutput(opts, file, /*binary=*/false);
    JsonMode mode = opts.canonical ? JsonMode::Canonical : JsonMode::Relaxed;

    std::span<const uint8_t> rest(data);
    while (!rest.empty()) {
        DecodeResult res = decodeOne(rest);
        out << toJson(res.document, mode, opts.pretty) << '\n';
        rest = rest.subspan(res.bytesConsumed);
    }
    out.flush();
    return 0;
}

int cmdToBson(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/false);
    std::vector<uint8_t> data = readFileBytes(opts.input);
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    std::vector<std::vector<uint8_t>> encoded;
    std::string_view rest = text;
    while (true) {
        std::size_t ws = rest.find_first_not_of(" \t\r\n");
        if (ws == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(ws);
        std::size_t consumed = 0;
        Value v = parseJsonOne(rest, consumed);
        if (!v.is<Document>()) {
            throw std::runtime_error("top-level JSON value must be an object");
        }
        encoded.push_back(encodeDocument(v));
        rest.remove_prefix(consumed);
    }
    if (encoded.empty()) {
        throw std::runtime_error("input contains no JSON documents");
    }

    std::ofstream file;
    std::ostream& out = openOutput(opts, file, /*binary=*/true);
    for (const auto& bytes : encoded) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
        throw std::runtime_error("failed writing output");
    }
    return 0;
}

void countValues(const Value& v, std::map<std::string_view, std::size_t>& counts) {
    counts[typeName(v.type())]++;
    if (v.is<Document>()) {
        for (const auto& [key, field] : v.asDocument()) {
            countValues(field, counts);
        }
    } else if (v.is<Array>()) {
        for (const Value& e : v.asArray()) {
            countValues(e, counts);
        }
    }
}

int cmdInspect(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/false);
    std::vector<uint8_t> data = readFileBytes(opts.input);

    std::size_t docCount = 0;
    std::map<std::string_view, std::size_t> counts;
    std::span<const uint8_t> rest(data);
    while (!rest.empty()) {
        DecodeResult res = decodeOne(rest);
        ++docCount;
        countValues(res.document, counts);
        rest = rest.subspan(res.bytesConsumed);
    }

    std::cout << "documents:   " << docCount << '\n';
    std::cout << "total bytes: " << data.size() << '\n';
    std::cout << "value counts by type:\n";
    for (const auto& [name, count] : counts) {
        std::cout << "  " << name << ": " << count << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(std::cerr);
    }
    std::string_view command = argv[1];
    std::span<char*> rest(argv + 2, static_cast<std::size_t>(argc - 2));
    try {
        if (command == "to-json") {
            return cmdToJson(rest);
        }
        if (command == "to-bson") {
            return cmdToBson(rest);
        }
        if (command == "inspect") {
            return cmdInspect(rest);
        }
        if (command == "--help" || command == "-h" || command == "help") {
            return usage(std::cout);
        }
        std::cerr << "bisonc: unknown command '" << command << "'\n";
        return usage(std::cerr);
    } catch (const std::exception& e) {
        std::cerr << "bisonc: " << e.what() << '\n';
        return 2;
    }
}
