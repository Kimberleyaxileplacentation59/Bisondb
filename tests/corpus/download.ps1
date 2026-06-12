# Fetches the official BSON corpus test files (for the 11 types BisonDB
# supports) from github.com/mongodb/specifications into this directory.
$ErrorActionPreference = 'Stop'
$files = @(
    'double.json', 'string.json', 'document.json', 'array.json', 'oid.json',
    'boolean.json', 'datetime.json', 'null.json', 'int32.json', 'int64.json',
    'decimal128-1.json'
)
$base = 'https://raw.githubusercontent.com/mongodb/specifications/master/source/bson-corpus/tests'
foreach ($f in $files) {
    Invoke-WebRequest -Uri "$base/$f" -OutFile (Join-Path $PSScriptRoot $f) -UseBasicParsing
    Write-Host "fetched $f"
}
