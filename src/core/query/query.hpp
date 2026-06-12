#pragma once

#include "core/query/index_manager.hpp"
#include "core/value.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace bisondb::query {

class QueryError : public Error {
  public:
    using Error::Error;
};

// Filter matcher. Supported shapes:
//   { field: literal }                                  equality (dotted ok)
//   { field: { $eq|$ne|$gt|$gte|$lt|$lte: v } }          comparisons
//   { field: { $in: [v, ...] } }
//   { $and: [f, ...] }, { $or: [f, ...] }
// Numbers compare numerically across Int32/Int64/Double; other comparisons
// require matching types (type bracketing). A missing field matches only
// $ne.
bool matches(const Value& doc, const Value& filter);

struct FindOptions {
    std::size_t limit = 0; // 0 = unlimited
    std::size_t skip = 0;
};

struct ExplainResult {
    std::string plan;  // "index_range" | "index_point" | "scan"
    std::string index; // field name when an index was used
    std::size_t docsExamined = 0;
    std::size_t docsReturned = 0;

    Value toValue() const;
};

// Plans and runs queries over an IndexedCollection. Planning is deliberately
// transparent: a single equality or range on an indexed field becomes an
// index scan with the full filter re-checked on every fetched document;
// everything else is a collection scan.
class QueryEngine {
  public:
    explicit QueryEngine(IndexedCollection& coll) : coll_(coll) {}

    std::vector<Value> find(const Value& filter, const FindOptions& options = {});
    ExplainResult explain(const Value& filter, const FindOptions& options = {});

    std::size_t deleteMany(const Value& filter);

    // update must be {"$set": {field: value, ...}} (dotted paths create
    // intermediate documents). Returns false when nothing matched.
    bool updateOne(const Value& filter, const Value& update);

  private:
    std::vector<Value> run(const Value& filter, const FindOptions& options, ExplainResult& explain);

    IndexedCollection& coll_;
};

} // namespace bisondb::query
