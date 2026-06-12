#include "core/query/query.hpp"

#include "core/btree/key_codec.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace bisondb::query {

namespace {

bool isNumeric(const Value& v) {
    return v.is<int32_t>() || v.is<int64_t>() || v.is<double>();
}

double asDouble(const Value& v) {
    if (v.is<int32_t>()) {
        return static_cast<double>(v.get<int32_t>());
    }
    if (v.is<int64_t>()) {
        return static_cast<double>(v.get<int64_t>());
    }
    return v.get<double>();
}

bool valuesEqual(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        return asDouble(a) == asDouble(b);
    }
    return a == b;
}

// Type-bracketed ordering: nullopt when the operands belong to different
// type classes (so range operators simply do not match).
std::optional<int> valuesCompare(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        double da = asDouble(a);
        double db = asDouble(b);
        if (da < db) {
            return -1;
        }
        if (da > db) {
            return 1;
        }
        if (da == db) {
            return 0;
        }
        return std::nullopt; // NaN involved
    }
    if (a.type() != b.type()) {
        return std::nullopt;
    }
    switch (a.type()) {
    case Type::String: {
        int c = a.get<std::string>().compare(b.get<std::string>());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case Type::Bool: return (a.get<bool>() ? 1 : 0) - (b.get<bool>() ? 1 : 0);
    case Type::DateTime: {
        int64_t ta = a.get<DateTime>().msSinceEpoch;
        int64_t tb = b.get<DateTime>().msSinceEpoch;
        return ta < tb ? -1 : (ta > tb ? 1 : 0);
    }
    case Type::ObjectId: {
        const auto& ba = a.get<ObjectId>().bytes;
        const auto& bb = b.get<ObjectId>().bytes;
        if (ba == bb) {
            return 0;
        }
        return ba < bb ? -1 : 1;
    }
    default: return std::nullopt;
    }
}

bool isOperatorDoc(const Value& v) {
    if (!v.is<Document>() || v.asDocument().empty()) {
        return false;
    }
    for (const auto& [k, sub] : v.asDocument()) {
        if (k.empty() || k[0] != '$') {
            return false;
        }
    }
    return true;
}

bool fieldMatches(const Value* fieldVal, const Value& cond) {
    if (!isOperatorDoc(cond)) {
        return fieldVal != nullptr && valuesEqual(*fieldVal, cond);
    }
    for (const auto& [op, operand] : cond.asDocument()) {
        if (op == "$eq") {
            if (fieldVal == nullptr || !valuesEqual(*fieldVal, operand)) {
                return false;
            }
        } else if (op == "$ne") {
            if (fieldVal != nullptr && valuesEqual(*fieldVal, operand)) {
                return false;
            }
        } else if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
            if (fieldVal == nullptr) {
                return false;
            }
            std::optional<int> c = valuesCompare(*fieldVal, operand);
            if (!c) {
                return false;
            }
            if ((op == "$gt" && *c <= 0) || (op == "$gte" && *c < 0) || (op == "$lt" && *c >= 0) ||
                (op == "$lte" && *c > 0)) {
                return false;
            }
        } else if (op == "$in") {
            if (fieldVal == nullptr || !operand.is<Array>()) {
                return false;
            }
            bool any = false;
            for (const Value& candidate : operand.asArray()) {
                if (valuesEqual(*fieldVal, candidate)) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                return false;
            }
        } else {
            throw QueryError("unsupported operator: " + op);
        }
    }
    return true;
}

// ---- planning ----------------------------------------------------------

struct RangeBounds {
    // Scan starts at `lower` and continues while key <= upperInclusive (when
    // set) / key < upperExclusive (when set). Empty lower = start of class.
    std::vector<uint8_t> lower;
    std::optional<std::vector<uint8_t>> upperInclusive;
    std::optional<std::vector<uint8_t>> upperExclusive;
};

struct FieldConstraint {
    std::string field;
    const Value* eq = nullptr;
    const Value* gt = nullptr;
    const Value* gte = nullptr;
    const Value* lt = nullptr;
    const Value* lte = nullptr;

    bool usable() const { return eq || gt || gte || lt || lte; }
};

// Collects per-field constraints from the top level of the filter, flattening
// nested $and. Returns false when the filter contains $or at the top (which
// disables index planning entirely).
bool collectConstraints(const Value& filter, std::vector<FieldConstraint>& out) {
    if (!filter.is<Document>()) {
        return false;
    }
    for (const auto& [key, cond] : filter.asDocument()) {
        if (key == "$and") {
            if (!cond.is<Array>()) {
                return false;
            }
            for (const Value& sub : cond.asArray()) {
                if (!collectConstraints(sub, out)) {
                    return false;
                }
            }
            continue;
        }
        if (key == "$or") {
            return false;
        }
        FieldConstraint* fc = nullptr;
        for (FieldConstraint& existing : out) {
            if (existing.field == key) {
                fc = &existing;
                break;
            }
        }
        if (fc == nullptr) {
            out.push_back(FieldConstraint{key, nullptr, nullptr, nullptr, nullptr, nullptr});
            fc = &out.back();
        }
        if (!isOperatorDoc(cond)) {
            fc->eq = &cond;
            continue;
        }
        for (const auto& [op, operand] : cond.asDocument()) {
            if (op == "$eq") {
                fc->eq = &operand;
            } else if (op == "$gt") {
                fc->gt = &operand;
            } else if (op == "$gte") {
                fc->gte = &operand;
            } else if (op == "$lt") {
                fc->lt = &operand;
            } else if (op == "$lte") {
                fc->lte = &operand;
            }
            // $ne / $in stay residual-only.
        }
    }
    return true;
}

// Builds index-scan bounds for a constraint; nullopt when any referenced
// value cannot be encoded (NaN, non-indexable type) — the planner then falls
// back to a full scan.
std::optional<RangeBounds> boundsFor(const FieldConstraint& fc) {
    try {
        RangeBounds b;
        if (fc.eq != nullptr) {
            auto enc = btree::encodeKey(*fc.eq);
            b.lower = enc;
            enc.push_back(0xFF); // composite keys are enc||0x00||oid, all < enc||0xFF
            b.upperInclusive = std::move(enc);
            return b;
        }
        uint8_t tag = 0;
        if (fc.gt != nullptr) {
            auto enc = btree::encodeKey(*fc.gt);
            tag = enc[0];
            enc.push_back(0xFF);
            b.lower = std::move(enc);
        } else if (fc.gte != nullptr) {
            b.lower = btree::encodeKey(*fc.gte);
            tag = b.lower[0];
        }
        if (fc.lt != nullptr) {
            b.upperExclusive = btree::encodeKey(*fc.lt);
            tag = (*b.upperExclusive)[0];
        } else if (fc.lte != nullptr) {
            auto enc = btree::encodeKey(*fc.lte);
            tag = enc[0];
            enc.push_back(0xFF);
            b.upperInclusive = std::move(enc);
        }
        // Type bracketing: an open end stops at the edge of the type class.
        if (b.lower.empty()) {
            b.lower = {tag};
        }
        if (!b.upperInclusive && !b.upperExclusive) {
            b.upperExclusive = std::vector<uint8_t>{static_cast<uint8_t>(tag + 1)};
        }
        return b;
    } catch (const btree::KeyNotIndexable&) {
        return std::nullopt;
    } catch (const btree::KeyTooLong&) {
        return std::nullopt;
    }
}

bool withinUpper(std::span<const uint8_t> key, const RangeBounds& b) {
    auto cmp = [](std::span<const uint8_t> a, const std::vector<uint8_t>& v) {
        std::span<const uint8_t> bs{v.data(), v.size()};
        std::size_t n = std::min(a.size(), bs.size());
        int c = n == 0 ? 0 : std::memcmp(a.data(), bs.data(), n);
        if (c != 0) {
            return c;
        }
        return a.size() == bs.size() ? 0 : (a.size() < bs.size() ? -1 : 1);
    };
    if (b.upperInclusive) {
        return cmp(key, *b.upperInclusive) <= 0;
    }
    if (b.upperExclusive) {
        return cmp(key, *b.upperExclusive) < 0;
    }
    return true;
}

} // namespace

bool matches(const Value& doc, const Value& filter) {
    if (!filter.is<Document>()) {
        throw QueryError("filter must be a document");
    }
    for (const auto& [key, cond] : filter.asDocument()) {
        if (key == "$and") {
            if (!cond.is<Array>()) {
                throw QueryError("$and requires an array");
            }
            for (const Value& sub : cond.asArray()) {
                if (!matches(doc, sub)) {
                    return false;
                }
            }
        } else if (key == "$or") {
            if (!cond.is<Array>()) {
                throw QueryError("$or requires an array");
            }
            bool any = false;
            for (const Value& sub : cond.asArray()) {
                if (matches(doc, sub)) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                return false;
            }
        } else if (!key.empty() && key[0] == '$') {
            throw QueryError("unsupported top-level operator: " + key);
        } else {
            if (!fieldMatches(store::lookupPath(doc, key), cond)) {
                return false;
            }
        }
    }
    return true;
}

Value ExplainResult::toValue() const {
    Document d{{"plan", Value(plan)}};
    if (!index.empty()) {
        d.append("index", Value(index));
    }
    d.append("docsExamined", Value(static_cast<int64_t>(docsExamined)));
    d.append("docsReturned", Value(static_cast<int64_t>(docsReturned)));
    return Value(std::move(d));
}

std::vector<Value> QueryEngine::run(const Value& filter, const FindOptions& options,
                                    ExplainResult& explain) {
    std::vector<Value> results;
    std::size_t seen = 0; // matched docs, before skip/limit

    auto admit = [&](Value doc) {
        ++explain.docsExamined;
        if (!matches(doc, filter)) {
            return true;
        }
        if (seen++ < options.skip) {
            return true;
        }
        results.push_back(std::move(doc));
        return options.limit == 0 || results.size() < options.limit;
    };

    std::vector<FieldConstraint> constraints;
    bool plannable = collectConstraints(filter, constraints);

    if (plannable) {
        // _id equality: point lookup.
        for (const FieldConstraint& fc : constraints) {
            if (fc.field == "_id" && fc.eq != nullptr && fc.eq->is<ObjectId>()) {
                explain.plan = "index_point";
                explain.index = "_id";
                if (auto doc = coll_.fetch(fc.eq->get<ObjectId>())) {
                    admit(std::move(*doc));
                }
                explain.docsReturned = results.size();
                return results;
            }
        }
        // First indexed field with a usable constraint: range scan.
        for (const FieldConstraint& fc : constraints) {
            btree::BTree* index = coll_.fieldIndex(fc.field);
            if (index == nullptr || !fc.usable()) {
                continue;
            }
            std::optional<RangeBounds> bounds = boundsFor(fc);
            if (!bounds) {
                continue;
            }
            explain.plan = "index_range";
            explain.index = fc.field;
            // Materialize matching oids first so no tree lock is held while
            // fetching documents.
            std::vector<ObjectId> oids;
            for (auto c = index->lowerBound({bounds->lower.data(), bounds->lower.size()});
                 c.valid(); c.next()) {
                if (!withinUpper(c.key(), *bounds)) {
                    break;
                }
                oids.push_back(btree::oidFromCompositeKey(
                    std::vector<uint8_t>(c.key().begin(), c.key().end())));
            }
            for (const ObjectId& oid : oids) {
                auto doc = coll_.fetch(oid);
                if (!doc) {
                    continue; // deleted between scan and fetch
                }
                if (!admit(std::move(*doc))) {
                    break;
                }
            }
            explain.docsReturned = results.size();
            return results;
        }
    }

    // Fallback: full collection scan via the _id index.
    explain.plan = "scan";
    std::vector<ObjectId> oids;
    {
        std::shared_lock lock(coll_.mutex());
        for (auto c = coll_.idIndex().lowerBound({}); c.valid(); c.next()) {
            oids.push_back(
                btree::oidFromCompositeKey(std::vector<uint8_t>(c.key().begin(), c.key().end())));
        }
    }
    for (const ObjectId& oid : oids) {
        auto doc = coll_.fetch(oid);
        if (!doc) {
            continue;
        }
        if (!admit(std::move(*doc))) {
            break;
        }
    }
    explain.docsReturned = results.size();
    return results;
}

std::vector<Value> QueryEngine::find(const Value& filter, const FindOptions& options) {
    ExplainResult ignored;
    return run(filter, options, ignored);
}

ExplainResult QueryEngine::explain(const Value& filter, const FindOptions& options) {
    ExplainResult result;
    run(filter, options, result);
    return result;
}

std::size_t QueryEngine::deleteMany(const Value& filter) {
    std::vector<Value> docs = find(filter);
    std::size_t deleted = 0;
    for (const Value& doc : docs) {
        if (coll_.eraseById(store::requireDocId(doc))) {
            ++deleted;
        }
    }
    return deleted;
}

namespace {

// Applies one $set assignment, creating intermediate documents along dotted
// paths.
void setPath(Value& doc, const std::string& path, const Value& newValue) {
    Value* cur = &doc;
    std::size_t start = 0;
    while (true) {
        std::size_t dot = path.find('.', start);
        std::string segment =
            path.substr(start, (dot == std::string::npos ? path.size() : dot) - start);
        Document& d = cur->asDocument();
        Value* next = d.find(segment);
        if (dot == std::string::npos) {
            if (next != nullptr) {
                *next = newValue;
            } else {
                d.append(segment, newValue);
            }
            return;
        }
        if (next == nullptr || !next->is<Document>()) {
            if (next == nullptr) {
                d.append(segment, Value(Document{}));
                next = d.find(segment);
            } else {
                *next = Value(Document{});
            }
        }
        cur = next;
        start = dot + 1;
    }
}

} // namespace

bool QueryEngine::updateOne(const Value& filter, const Value& update) {
    const Value* set = update.is<Document>() ? update.asDocument().find("$set") : nullptr;
    if (set == nullptr || !set->is<Document>() || update.asDocument().size() != 1) {
        throw QueryError("update must be {\"$set\": {...}}");
    }
    FindOptions one;
    one.limit = 1;
    std::vector<Value> found = find(filter, one);
    if (found.empty()) {
        return false;
    }
    Value doc = std::move(found.front());
    ObjectId oid = store::requireDocId(doc);
    for (const auto& [path, v] : set->asDocument()) {
        if (path == "_id") {
            throw QueryError("$set may not change _id");
        }
        setPath(doc, path, v);
    }
    coll_.update(oid, std::move(doc));
    return true;
}

} // namespace bisondb::query
