#include "msim/tick.hpp"

#include <cmath>
#include <limits>

#include "check.hpp"

using msim::price_to_tick;

namespace {

void test_rounds_to_nearest_tick() {
  CHECK(price_to_tick(100.00, 0.01, 1'000'000) == 10000);
  CHECK(price_to_tick(100.004, 0.01, 1'000'000) == 10000);
  CHECK(price_to_tick(100.006, 0.01, 1'000'000) == 10001);
  CHECK(price_to_tick(0.01, 0.01, 1'000'000) == 1);
  CHECK(price_to_tick(99.5, 1.0, 1'000) == 100);  // half rounds away from 0
  CHECK(price_to_tick(1.0 / 32, 1.0 / 32, 1'000) == 1);
}

// 0.01 has no exact binary form, so prices that look like exact halves need
// not round up. The edge conversion makes this visible once, in one place,
// instead of inside every comparison the matcher makes.
void test_decimal_halves_follow_binary_value() {
  const auto t = price_to_tick(1.005, 0.01, 1'000'000);
  CHECK(t.has_value());
  CHECK(*t == 100 || *t == 101);
  CHECK(*t == static_cast<msim::Tick>(std::round(1.005 / 0.01)));
}

void test_refuses_bad_input() {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(!price_to_tick(nan, 0.01, 1'000'000));
  CHECK(!price_to_tick(inf, 0.01, 1'000'000));
  CHECK(!price_to_tick(-inf, 0.01, 1'000'000));
  CHECK(!price_to_tick(100.0, 0.0, 1'000'000));
  CHECK(!price_to_tick(100.0, -0.01, 1'000'000));
  CHECK(!price_to_tick(100.0, nan, 1'000'000));
  CHECK(!price_to_tick(0.0, 0.01, 1'000'000));    // tick 0
  CHECK(!price_to_tick(0.004, 0.01, 1'000'000));  // rounds to 0
  CHECK(!price_to_tick(-5.0, 0.01, 1'000'000));
  CHECK(!price_to_tick(10000.01, 0.01, 1'000'000));  // above max_tick
  CHECK(price_to_tick(10000.00, 0.01, 1'000'000) == 1'000'000);
  // Far beyond the int64 range: refused, with no undefined cast.
  constexpr auto kMax = std::numeric_limits<msim::Tick>::max();
  CHECK(!price_to_tick(1e300, 0.01, kMax));
  // max_tick as a double rounds up to exactly 2^63, one past the int64 range.
  CHECK(!price_to_tick(9223372036854775808.0, 1.0, kMax));
  CHECK(price_to_tick(4611686018427387904.0, 1.0, kMax) ==
        msim::Tick{4611686018427387904});  // 2^62 is fine
  // 2^54 - 1 is not exact as a double. It rounds up to 2^54, which must still
  // count as one past the limit.
  constexpr msim::Tick kOdd = (msim::Tick{1} << 54) - 1;
  CHECK(!price_to_tick(18014398509481984.0, 1.0, kOdd));
}

}  // namespace

int main() {
  test_rounds_to_nearest_tick();
  test_decimal_halves_follow_binary_value();
  test_refuses_bad_input();
  return check::finish("tick");
}
