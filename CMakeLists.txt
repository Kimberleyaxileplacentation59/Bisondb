cmake_minimum_required(VERSION 3.21)
project(BisonDB LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(FetchContent)

# ── Warnings interface target ─────────────────────────────────────────────────
add_library(bisondb_warnings INTERFACE)
if(MSVC)
    # /Zc:preprocessor: the traditional preprocessor mis-lexes raw string
    # literals inside macro arguments. /utf-8: sources contain UTF-8 literals.
    target_compile_options(bisondb_warnings INTERFACE /W4 /WX /Zc:preprocessor /utf-8)
else()
    target_compile_options(bisondb_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# ── Catch2 ────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ── bisondb_core (static library) ─────────────────────────────────────────────
add_library(bisondb_core STATIC
    src/core/version.cpp
    src/core/value.cpp
    src/core/bson_decoder.cpp
    src/core/bson_encoder.cpp
    src/core/decimal128.cpp
    src/core/json_writer.cpp
    src/core/json_parser.cpp
)
target_include_directories(bisondb_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(bisondb_core PRIVATE bisondb_warnings)

# ── bisonc (CLI executable) ───────────────────────────────────────────────────
add_executable(bisonc
    src/cli/main.cpp
)
target_link_libraries(bisonc PRIVATE bisondb_core bisondb_warnings)

# ── bisondb_tests ─────────────────────────────────────────────────────────────
add_executable(bisondb_tests
    tests/test_sanity.cpp
    tests/test_value.cpp
    tests/test_bson_decoder.cpp
    tests/test_bson_encoder.cpp
    tests/test_json_writer.cpp
    tests/test_json_parser.cpp
    tests/test_roundtrip.cpp
    tests/test_corpus.cpp
    tests/test_fixtures.cpp
)
target_link_libraries(bisondb_tests PRIVATE bisondb_core bisondb_warnings Catch2::Catch2WithMain)
target_compile_definitions(bisondb_tests PRIVATE
    BISONDB_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
catch_discover_tests(bisondb_tests)
