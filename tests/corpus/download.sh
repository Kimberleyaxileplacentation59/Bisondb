#!/usr/bin/env bash
# Fetches the official BSON corpus test files (for the 11 types BisonDB
# supports) from github.com/mongodb/specifications into this directory.
set -euo pipefail
cd "$(dirname "$0")"
base="https://raw.githubusercontent.com/mongodb/specifications/master/source/bson-corpus/tests"
for f in double.json string.json document.json array.json oid.json \
         boolean.json datetime.json null.json int32.json int64.json \
         decimal128-1.json; do
    curl -fsSL -o "$f" "$base/$f"
    echo "fetched $f"
done
