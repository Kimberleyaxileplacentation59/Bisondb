#pragma once

#include "core/value.hpp"

#include <string>
#include <string_view>

namespace bisondb {

// Renders an IEEE 754-2008 decimal128 (binary integer decimal encoding) as
// the decimal string MongoDB tools produce: plain or scientific notation per
// the Extended JSON rules, with "NaN" / "Infinity" / "-Infinity" specials.
// Non-canonical bit patterns (significand above 10^34-1, including patterns
// that densely-packed-decimal would interpret differently) render with a
// zero significand, matching libbson.
std::string decimal128ToString(const Decimal128& value);

// Parses a decimal string ("123", "-4.5E+3", "NaN", "Infinity", ...) into a
// decimal128. Accepts at most 34 significant digits (excess trailing zeros
// are folded into the exponent); throws TypeError on syntax errors, inexact
// values, or exponents outside [-6176, 6111] after clamping.
Decimal128 decimal128FromString(std::string_view text);

} // namespace bisondb
