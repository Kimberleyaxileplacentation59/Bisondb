#pragma once

#include "core/value.hpp"

#include <string>

namespace bisondb {

enum class JsonMode {
    // MongoDB Extended JSON v2 relaxed: plain JSON numbers where possible.
    Relaxed,
    // Extended JSON v2 canonical: $numberInt/$numberLong/$numberDouble
    // wrappers preserve exact types; round-trips losslessly via parseJson.
    Canonical,
};

std::string toJson(const Value& v, JsonMode mode = JsonMode::Relaxed, bool pretty = false);

} // namespace bisondb
