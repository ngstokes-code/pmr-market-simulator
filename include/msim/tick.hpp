#pragma once

#include <cmath>
#include <optional>

#include "msim/types.hpp"

namespace msim {

// Rounds a decimal price to the nearest tick. Returns nullopt for a price that
// is not finite, a tick size that is not positive, or a result outside
// [1, max_tick]. Halfway cases round away from zero.
inline std::optional<Tick> price_to_tick(double price, double tick_size,
                                         Tick max_tick) noexcept {
  if (!std::isfinite(price) || !std::isfinite(tick_size) || !(tick_size > 0.0))
    return std::nullopt;
  const double x = std::round(price / tick_size);
  // Casting a double outside the int64 range is undefined, so bound it in
  // floating point first. 2^63 is exact as a double.
  constexpr double kTickLimit = 9223372036854775808.0;  // 2^63
  if (!(x >= 1.0) || !(x < kTickLimit)) return std::nullopt;
  // Compare with max_tick as integers: max_tick itself may not be exact as
  // a double, and rounding it up would admit one tick too many.
  const auto t = static_cast<Tick>(x);
  if (t > max_tick) return std::nullopt;
  return t;
}

inline double tick_to_price(Tick t, double tick_size) noexcept {
  return static_cast<double>(t) * tick_size;
}

}  // namespace msim
