# BisonDB

BisonDB is a document database with a BSON storage engine, inspired by MongoDB. It is written
in C++20 and targets Windows as its primary development platform, with Linux support planned.
Phase 1 is complete: `bisondb_core` provides a BSON value model, a hardened BSON
decoder/encoder, a MongoDB Extended JSON v2 writer (relaxed and canonical modes, including
decimal128 string rendering), and a strict RFC 8259 JSON parser with Extended JSON folding.
The `bisonc` CLI converts between BSON and JSON. No storage or networking code exists yet.

## Building

### Prerequisites

- CMake 3.21+
- Ninja (recommended) or Visual Studio 2022
- MSVC, GCC, or Clang with C++20 support

### Quick start (MSVC / Visual Studio)

```bat
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

### Quick start (Ninja)

```bat
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

### MinGW (MSYS2 GCC)

```bat
cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug
```

## bisonc CLI

`bisonc` converts between BSON files (single documents or concatenated dumps, as produced by
mongodump) and JSON.

```bat
:: BSON -> JSON Lines (relaxed Extended JSON) on stdout
bisonc to-json dump.bson

:: Canonical (lossless) Extended JSON, written to a file
bisonc to-json dump.bson --canonical -o dump.jsonl

:: Pretty-printed instead of one document per line
bisonc to-json dump.bson --pretty

:: JSON (single document or JSON Lines) -> concatenated BSON
bisonc to-bson dump.jsonl -o dump.bson

:: Document count, total bytes, and per-type value counts
bisonc inspect dump.bson
```

Errors go to stderr with a non-zero exit code. Canonical mode round-trips losslessly:
`to-json --canonical` followed by `to-bson` reproduces the original file byte for byte.

## Tests

The Catch2 suite covers unit tests per module, byte-exact round-trips, and the official
[BSON corpus](https://github.com/mongodb/specifications/tree/master/source/bson-corpus)
(fetched copies live in `tests/corpus/`; re-fetch with `tests/corpus/download.ps1` or
`download.sh`). Any `.bson` files dropped into `tests/fixtures/` are automatically
round-trip-tested for byte-identical re-encoding.

## Code formatting

This project uses `clang-format` with the configuration in `.clang-format` (LLVM style, 4-space
indent, 100-column limit, left pointer alignment). To check formatting locally:

```bash
clang-format --dry-run --Werror $(find src tests -name "*.cpp" -o -name "*.hpp")
```

CI will fail the lint job if any file is not formatted correctly.
