#include "core/json_parser.hpp"
#include "core/query/query.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bisondb;
using bisondb::query::matches;

namespace {

bool match(const char* docJson, const char* filterJson) {
    return matches(parseJson(docJson), parseJson(filterJson));
}

} // namespace

TEST_CASE("equality matching", "[matcher]") {
    REQUIRE(match(R"({"a": 1})", R"({"a": 1})"));
    REQUIRE_FALSE(match(R"({"a": 1})", R"({"a": 2})"));
    REQUIRE(match(R"({"a": "x"})", R"({"a": "x"})"));
    REQUIRE_FALSE(match(R"({"a": "x"})", R"({"b": "x"})")); // missing field
    REQUIRE(match(R"({"a": 1, "b": 2})", R"({"a": 1, "b": 2})"));
    REQUIRE_FALSE(match(R"({"a": 1, "b": 2})", R"({"a": 1, "b": 3})"));
    REQUIRE(match(R"({"a": null})", R"({"a": null})"));
    REQUIRE(match(R"({"a": {"b": 1}})", R"({"a": {"b": 1}})")); // subdoc literal
}

TEST_CASE("cross-numeric equality and comparison", "[matcher]") {
    REQUIRE(match(R"({"a": 1})", R"({"a": 1.0})"));
    REQUIRE(match(R"({"a": {"$numberLong": "5"}})", R"({"a": 5})"));
    REQUIRE(match(R"({"a": 2147483648})", R"({"a": {"$gt": 2147483647}})")); // int64 vs int32
    REQUIRE(match(R"({"a": 1.5})", R"({"a": {"$gt": 1, "$lt": 2}})"));
    REQUIRE_FALSE(match(R"({"a": "5"})", R"({"a": 5})")); // no string/number coercion
}

TEST_CASE("range operators with type bracketing", "[matcher]") {
    REQUIRE(match(R"({"a": 5})", R"({"a": {"$gte": 5}})"));
    REQUIRE_FALSE(match(R"({"a": 5})", R"({"a": {"$gt": 5}})"));
    REQUIRE(match(R"({"a": 5})", R"({"a": {"$lte": 5}})"));
    REQUIRE_FALSE(match(R"({"a": 5})", R"({"a": {"$lt": 5}})"));
    REQUIRE(match(R"({"a": "b"})", R"({"a": {"$gt": "a"}})"));
    // Different type classes never satisfy range operators.
    REQUIRE_FALSE(match(R"({"a": "zzz"})", R"({"a": {"$gt": 5}})"));
    REQUIRE_FALSE(match(R"({"a": 5})", R"({"a": {"$lt": "x"}})"));
}

TEST_CASE("$ne and missing fields", "[matcher]") {
    REQUIRE(match(R"({"a": 1})", R"({"a": {"$ne": 2}})"));
    REQUIRE_FALSE(match(R"({"a": 1})", R"({"a": {"$ne": 1}})"));
    // Missing fields match only $ne.
    REQUIRE(match(R"({"b": 1})", R"({"a": {"$ne": 1}})"));
    REQUIRE_FALSE(match(R"({"b": 1})", R"({"a": {"$gt": 0}})"));
    REQUIRE_FALSE(match(R"({"b": 1})", R"({"a": {"$in": [1]}})"));
}

TEST_CASE("$in", "[matcher]") {
    REQUIRE(match(R"({"a": 2})", R"({"a": {"$in": [1, 2, 3]}})"));
    REQUIRE_FALSE(match(R"({"a": 4})", R"({"a": {"$in": [1, 2, 3]}})"));
    REQUIRE(match(R"({"a": 2})", R"({"a": {"$in": [2.0]}})")); // cross-numeric
}

TEST_CASE("$and and $or", "[matcher]") {
    REQUIRE(match(R"({"a": 1, "b": 2})", R"({"$and": [{"a": 1}, {"b": 2}]})"));
    REQUIRE_FALSE(match(R"({"a": 1, "b": 2})", R"({"$and": [{"a": 1}, {"b": 3}]})"));
    REQUIRE(match(R"({"a": 1})", R"({"$or": [{"a": 2}, {"a": 1}]})"));
    REQUIRE_FALSE(match(R"({"a": 1})", R"({"$or": [{"a": 2}, {"a": 3}]})"));
    REQUIRE(match(R"({"a": 5, "b": 1})",
                  R"({"$or": [{"a": {"$lt": 3}}, {"b": 1}], "a": {"$gte": 5}})"));
}

TEST_CASE("dotted paths", "[matcher]") {
    REQUIRE(match(R"({"address": {"city": "Lahore"}})", R"({"address.city": "Lahore"})"));
    REQUIRE_FALSE(match(R"({"address": {"city": "Lahore"}})", R"({"address.city": "Karachi"})"));
    REQUIRE(match(R"({"a": {"b": {"c": 3}}})", R"({"a.b.c": {"$gt": 2}})"));
    REQUIRE_FALSE(match(R"({"a": 1})", R"({"a.b": 1})")); // path through non-document
}

TEST_CASE("unsupported operators throw", "[matcher]") {
    REQUIRE_THROWS_AS(match(R"({"a": 1})", R"({"a": {"$regex": "x"}})"),
                      bisondb::query::QueryError);
    REQUIRE_THROWS_AS(match(R"({"a": 1})", R"({"$not": [{"a": 1}]})"), bisondb::query::QueryError);
}
