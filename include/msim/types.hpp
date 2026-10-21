#pragma once

#include <cstdint>

namespace msim {

using OrderId = std::uint64_t;

// Prices inside the book are integer multiples of the tick size. Conversion
// from a decimal price happens once, at the edge (see tick.hpp).
using Tick = std::int64_t;

using Qty = std::int64_t;

enum class Side : std::uint8_t { BUY = 1, SELL = 2 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::BUY ? Side::SELL : Side::BUY;
}

}  // namespace msim
