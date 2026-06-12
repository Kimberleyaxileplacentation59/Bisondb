cmake_minimum_required(VERSION 3.21)
project(BisonDB LANGUAGES CXX VERSION 1.0.0)

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
    # Portable C stdio (fopen) is used deliberately; silence the fopen_s nags.
    target_compile_definitions(bisondb_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bisondb_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC's -Wmaybe-uninitialized produces well-known false positives in
        # std::variant/vector move paths at -O2/-O3.
        target_compile_options(bisondb_warnings INTERFACE -Wno-maybe-uninitialized)
    endif()
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
    src/core/btree/pager.cpp
    src/core/btree/key_codec.cpp
    src/core/btree/btree.cpp
    src/core/store/collection.cpp
    src/core/query/index_manager.cpp
    src/core/query/query.cpp
    src/core/net/socket.cpp
    src/core/query/database.cpp
    src/server/protocol.cpp
    src/client/client.cpp
)
if(WIN32)
    target_link_libraries(bisondb_core PUBLIC ws2_32)
endif()

# ── bisondb_server (server library; bisond is a thin wrapper) ─────────────────
add_library(bisondb_server STATIC
    src/server/server.cpp
    src/server/commands.cpp
)
target_link_libraries(bisondb_server PUBLIC bisondb_core)
target_link_libraries(bisondb_server PRIVATE bisondb_warnings)

add_executable(bisond
    src/server/main.cpp
)
target_link_libraries(bisond PRIVATE bisondb_server bisondb_shell bisondb_warnings)

# ── bisonsh (interactive shell) ───────────────────────────────────────────────
add_library(bisondb_shell STATIC
    src/shell/parser.cpp
    src/shell/printer.cpp
    src/shell/repl.cpp
    src/shell/banner.cpp
)
target_link_libraries(bisondb_shell PUBLIC bisondb_core)
target_link_libraries(bisondb_shell PRIVATE bisondb_warnings)

add_executable(bisonsh
    src/shell/main.cpp
)
target_link_libraries(bisonsh PRIVATE bisondb_shell bisondb_warnings)
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
    tests/test_pager.cpp
    tests/test_key_codec.cpp
    tests/test_btree.cpp
    tests/test_matcher.cpp
    tests/test_query.cpp
    tests/test_concurrency.cpp
    tests/test_net.cpp
    tests/test_banner.cpp
    tests/test_json_relaxed.cpp
    tests/test_shell_parser.cpp
    tests/test_shell_printer.cpp
    tests/integration/test_server.cpp
    tests/integration/test_shell.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(bisondb_tests PRIVATE Threads::Threads bisondb_server bisondb_shell)
target_link_libraries(bisondb_tests PRIVATE bisondb_core bisondb_warnings Catch2::Catch2WithMain)
target_compile_definitions(bisondb_tests PRIVATE
    BISONDB_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
catch_discover_tests(bisondb_tests)
