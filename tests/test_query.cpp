#include "core/bson_decoder.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"
#include "core/query/query.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace bisondb;
using namespace bisondb::query;

namespace {

namespace fs = std::filesystem;

struct TempDb {
    fs::path dir;
    explicit TempDb(const std::string& name) {
        dir = fs::temp_directory_path() / ("bisondb_db_" + name);
        fs::remove_all(dir);
    }
    ~TempDb() { fs::remove_all(dir); }
    std::string str() const { return dir.string(); }
};

// Results compared as sorted _id hex sets so scan order never matters.
std::set<std::string> idSet(const std::vector<Value>& docs) {
    std::set<std::string> out;
    for (const Value& d : docs) {
        out.insert(store::requireDocId(d).toHex());
    }
    return out;
}

Value randomDoc(std::mt19937& rng) {
    Document d;
    d.append("n", Value(static_cast<int32_t>(rng() % 100)));
    d.append("big", Value(static_cast<int64_t>(rng() % 1000000)));
    d.append("f", Value(static_cast<double>(rng() % 1000) / 4.0));
    d.append("s", Value(std::string(1, static_cast<char>('a' + rng() % 26))));
    if (rng() % 3 != 0) {
        d.append("nested", Value(Document{{"x", Value(static_cast<int32_t>(rng() % 50))}}));
    }
    if (rng() % 4 == 0) {
        d.append("flag", Value(rng() % 2 == 0));
    }
    return Value(std::move(d));
}

Value randomFilter(std::mt19937& rng) {
    auto num = [&rng] { return Value(static_cast<int32_t>(rng() % 100)); };
    switch (rng() % 8) {
    case 0: return parseJson(R"({"n": )" + std::to_string(rng() % 100) + "}");
    case 1: return Value(Document{{"n", Value(Document{{"$gt", num()}})}});
    case 2: return Value(Document{{"n", Value(Document{{"$gte", num()}, {"$lt", num()}})}});
    case 3:
        return Value(Document{
            {"f", Value(Document{{"$lte", Value(static_cast<double>(rng() % 1000) / 4.0)}})}});
    case 4:
        return Value(Document{{"s", Value(std::string(1, static_cast<char>('a' + rng() % 26)))}});
    case 5:
        return Value(Document{
            {"nested.x", Value(Document{{"$gt", Value(static_cast<int32_t>(rng() % 50))}})}});
    case 6:
        return Value(Document{
            {"$or", Value(Array{Value(Document{{"n", num()}}), Value(Document{{"n", num()}})})}});
    default:
        return Value(Document{
            {"$and", Value(Array{Value(Document{{"n", Value(Document{{"$gte", num()}})}}),
                                 Value(Document{{"s", Value(Document{{"$ne", Value("q")}})}})})}});
    }
}

} // namespace

TEST_CASE("insert, fetch, erase, update with indexes", "[query]") {
    TempDb db("crud");
    IndexedCollection coll(db.str(), "people");
    coll.createIndex("age");

    ObjectId a = coll.insert(parseJson(R"({"name": "ada", "age": 36})"));
    ObjectId b = coll.insert(parseJson(R"({"name": "bob", "age": 25})"));
    REQUIRE(coll.count() == 2);

    auto fetched = coll.fetch(a);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->asDocument().find("name")->get<std::string>() == "ada");

    QueryEngine q(coll);
    auto young = q.find(parseJson(R"({"age": {"$lt": 30}})"));
    REQUIRE(young.size() == 1);
    REQUIRE(store::requireDocId(young[0]) == b);

    REQUIRE(q.updateOne(parseJson(R"({"name": "bob"})"), parseJson(R"({"$set": {"age": 31}})")));
    REQUIRE(q.find(parseJson(R"({"age": {"$lt": 30}})")).empty());
    REQUIRE(q.find(parseJson(R"({"age": 31})")).size() == 1);

    REQUIRE(coll.eraseById(a));
    REQUIRE_FALSE(coll.eraseById(a));
    REQUIRE(coll.count() == 1);
}

TEST_CASE("planner picks the expected plans", "[query]") {
    TempDb db("plans");
    IndexedCollection coll(db.str(), "c");
    std::vector<ObjectId> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(
            coll.insert(Value(Document{{"v", Value(int32_t{i})}, {"w", Value(int32_t{i % 10})}})));
    }
    coll.createIndex("v");
    QueryEngine q(coll);

    SECTION("_id equality is a point lookup") {
        Value filter(Document{{"_id", Value(ids[42])}});
        ExplainResult e = q.explain(filter);
        REQUIRE(e.plan == "index_point");
        REQUIRE(e.index == "_id");
        REQUIRE(e.docsExamined == 1);
        REQUIRE(e.docsReturned == 1);
    }
    SECTION("equality on an indexed field is an index range") {
        ExplainResult e = q.explain(parseJson(R"({"v": 7})"));
        REQUIRE(e.plan == "index_range");
        REQUIRE(e.index == "v");
        REQUIRE(e.docsExamined == 1);
        REQUIRE(e.docsReturned == 1);
    }
    SECTION("range on an indexed field collapses docsExamined") {
        ExplainResult e = q.explain(parseJson(R"({"v": {"$gte": 90}})"));
        REQUIRE(e.plan == "index_range");
        REQUIRE(e.docsExamined == 10);
        REQUIRE(e.docsReturned == 10);
    }
    SECTION("two range bounds under $and combine on the same field") {
        ExplainResult e =
            q.explain(parseJson(R"({"$and": [{"v": {"$gte": 10}}, {"v": {"$lt": 20}}]})"));
        REQUIRE(e.plan == "index_range");
        REQUIRE(e.docsExamined == 10);
    }
    SECTION("unindexed fields fall back to scan") {
        ExplainResult e = q.explain(parseJson(R"({"w": 3})"));
        REQUIRE(e.plan == "scan");
        REQUIRE(e.docsExamined == 100);
        REQUIRE(e.docsReturned == 10);
    }
    SECTION("top-level $or disables index planning") {
        ExplainResult e = q.explain(parseJson(R"({"$or": [{"v": 1}, {"v": 2}]})"));
        REQUIRE(e.plan == "scan");
    }
    SECTION("residual predicates are re-checked on index scans") {
        ExplainResult e = q.explain(parseJson(R"({"v": {"$gte": 90}, "w": 0})"));
        REQUIRE(e.plan == "index_range");
        REQUIRE(e.docsExamined == 10);
        REQUIRE(e.docsReturned == 1); // only v=90 has w=0
    }
}

TEST_CASE("limit and skip", "[query]") {
    TempDb db("limit");
    IndexedCollection coll(db.str(), "c");
    for (int i = 0; i < 20; ++i) {
        coll.insert(Value(Document{{"v", Value(int32_t{i})}}));
    }
    QueryEngine q(coll);
    FindOptions opts;
    opts.limit = 5;
    REQUIRE(q.find(parseJson(R"({})"), opts).size() == 5);
    opts.skip = 18;
    opts.limit = 0;
    REQUIRE(q.find(parseJson(R"({})"), opts).size() == 2);
}

TEST_CASE("deleteMany via filter", "[query]") {
    TempDb db("delmany");
    IndexedCollection coll(db.str(), "c");
    for (int i = 0; i < 50; ++i) {
        coll.insert(Value(Document{{"v", Value(int32_t{i})}}));
    }
    coll.createIndex("v");
    QueryEngine q(coll);
    REQUIRE(q.deleteMany(parseJson(R"({"v": {"$lt": 20}})")) == 20);
    REQUIRE(coll.count() == 30);
    REQUIRE(q.find(parseJson(R"({"v": {"$lt": 20}})")).empty());
}

TEST_CASE("equivalence fuzz: index plans equal brute-force scans", "[query][fuzz]") {
    for (uint32_t seed : {11u, 222u, 3333u}) {
        TempDb db("equiv_" + std::to_string(seed));
        IndexedCollection coll(db.str(), "c");
        std::mt19937 rng(seed);
        for (int i = 0; i < 300; ++i) {
            coll.insert(randomDoc(rng));
        }
        coll.createIndex("n");
        coll.createIndex("f");
        coll.createIndex("nested.x");
        QueryEngine q(coll);

        for (int iter = 0; iter < 100; ++iter) {
            Value filter = randomFilter(rng);
            INFO("seed=" << seed << " iter=" << iter << " filter=" << toJson(filter));
            // Brute force: walk everything through the matcher.
            std::vector<Value> expected;
            for (const Value& doc : q.find(parseJson("{}"))) {
                if (matches(doc, filter)) {
                    expected.push_back(doc);
                }
            }
            REQUIRE(idSet(q.find(filter)) == idSet(expected));
        }
    }
}

TEST_CASE("index maintenance fuzz: mutations keep indexes consistent", "[query][fuzz]") {
    TempDb db("maint");
    IndexedCollection coll(db.str(), "c");
    coll.createIndex("n");
    QueryEngine q(coll);
    std::mt19937 rng(777);
    std::vector<ObjectId> live;

    for (int batch = 0; batch < 20; ++batch) {
        for (int i = 0; i < 30; ++i) {
            switch (rng() % 3) {
            case 0: live.push_back(coll.insert(randomDoc(rng))); break;
            case 1:
                if (!live.empty()) {
                    std::size_t at = rng() % live.size();
                    coll.eraseById(live[at]);
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(at));
                }
                break;
            default:
                if (!live.empty()) {
                    ObjectId oid = live[rng() % live.size()];
                    Value doc = coll.fetch(oid).value();
                    doc.asDocument().find("n")->get<int32_t>() = static_cast<int32_t>(rng() % 100);
                    coll.update(oid, std::move(doc));
                }
                break;
            }
        }
        // After every batch: each index query equals its brute-force result.
        for (int v = 0; v < 100; v += 7) {
            Value filter(Document{{"n", Value(Document{{"$gte", Value(int32_t{v})},
                                                       {"$lt", Value(int32_t{v + 7})}})}});
            ExplainResult e = q.explain(filter);
            REQUIRE(e.plan == "index_range");
            std::vector<Value> expected;
            for (const Value& doc : q.find(parseJson("{}"))) {
                if (matches(doc, filter)) {
                    expected.push_back(doc);
                }
            }
            REQUIRE(idSet(q.find(filter)) == idSet(expected));
        }
    }
    REQUIRE(coll.count() == live.size());
}

TEST_CASE("crash recovery: dirty indexes are rebuilt and match a fresh build", "[query]") {
    TempDb db("rebuild");
    std::set<std::string> expectedIds;
    {
        IndexedCollection coll(db.str(), "c");
        coll.createIndex("n");
        std::mt19937 rng(99);
        for (int i = 0; i < 100; ++i) {
            coll.insert(randomDoc(rng));
        }
        coll.sync();
        // More writes that never get synced to the index files; the log
        // appends land on disk immediately (source of truth).
        for (int i = 0; i < 50; ++i) {
            coll.insert(randomDoc(rng));
        }
        QueryEngine q(coll);
        expectedIds = idSet(q.find(parseJson("{}")));
        // Simulate the crash: drop the collection object without sync(); the
        // BTree destructors flush, so manually dirty the files instead.
    }
    // Force the rebuild path by corrupting the clean flags.
    for (const char* idx : {"c._id.idx", "c.n.idx"}) {
        auto p = db.dir / idx;
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(24);
        f.put('\0'); // cleanFlag = 0
    }
    IndexedCollection coll(db.str(), "c");
    QueryEngine q(coll);
    REQUIRE(idSet(q.find(parseJson("{}"))) == expectedIds);
    ExplainResult e = q.explain(parseJson(R"({"n": {"$gte": 0}})"));
    REQUIRE(e.plan == "index_range");
    std::vector<Value> viaIndex = q.find(parseJson(R"({"n": {"$gte": 0}})"));
    std::vector<Value> expected;
    for (const Value& doc : q.find(parseJson("{}"))) {
        if (matches(doc, parseJson(R"({"n": {"$gte": 0}})"))) {
            expected.push_back(doc);
        }
    }
    REQUIRE(idSet(viaIndex) == idSet(expected));
}

TEST_CASE("compaction shrinks the log and preserves results", "[query]") {
    TempDb db("compact");
    IndexedCollection coll(db.str(), "c");
    coll.createIndex("n");
    QueryEngine q(coll);
    std::mt19937 rng(5);
    std::vector<ObjectId> ids;
    for (int i = 0; i < 200; ++i) {
        ids.push_back(coll.insert(randomDoc(rng)));
    }
    for (int i = 0; i < 150; ++i) {
        coll.eraseById(ids[static_cast<std::size_t>(i)]);
    }
    auto before = idSet(q.find(parseJson("{}")));
    REQUIRE(before.size() == 50);

    coll.compact();
    REQUIRE(idSet(q.find(parseJson("{}"))) == before);
    ExplainResult e = q.explain(parseJson(R"({"n": {"$gte": 50}})"));
    REQUIRE(e.plan == "index_range");
}

TEST_CASE("zips fixture: index range query equals scan, explain shows the collapse",
          "[query][fixtures]") {
    fs::path zips = fs::path(BISONDB_TESTS_DIR) / "fixtures" / "zips.bson";
    if (!fs::exists(zips)) {
        SKIP("tests/fixtures/zips.bson not present");
    }
    TempDb db("zips");
    IndexedCollection coll(db.str(), "zips");
    std::vector<uint8_t> data = bisondb::test::readFileBytes(zips.string());
    std::span<const uint8_t> rest(data);
    while (!rest.empty()) {
        DecodeResult res = decodeOne(rest);
        coll.insert(std::move(res.document));
        rest = rest.subspan(res.bytesConsumed);
    }
    REQUIRE(coll.count() == 29470);

    QueryEngine q(coll);
    Value filter = parseJson(R"({"pop": {"$gte": 40000}})");

    ExplainResult scanPlan = q.explain(filter);
    REQUIRE(scanPlan.plan == "scan");
    REQUIRE(scanPlan.docsExamined == 29470);
    std::set<std::string> viaScan = idSet(q.find(filter));

    auto stats = coll.createIndex("pop");
    REQUIRE(stats.indexed == 29470);
    ExplainResult indexPlan = q.explain(filter);
    REQUIRE(indexPlan.plan == "index_range");
    REQUIRE(indexPlan.index == "pop");
    REQUIRE(indexPlan.docsExamined == indexPlan.docsReturned); // exact range
    // The money shot: 1015 docs examined via the index vs 29470 for the scan.
    REQUIRE(indexPlan.docsExamined < 2000);
    REQUIRE(idSet(q.find(filter)) == viaScan);
}
