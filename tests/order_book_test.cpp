#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <vector>

#include "msim/order_book.hpp"

static void test_basic_match_and_cancel() {
  std::vector<std::byte> buf(1 << 16);
  std::pmr::monotonic_buffer_resource mr(buf.data(), buf.size());

  msim::OrderBook book("X", &mr, /*tick_size=*/1.0);

  double tp = 0.0;

  // Resting ask id=1 price=101 qty=10
  msim::Order a{1, 101.0, 10, msim::Side::SELL, 0};
  int m0 = book.add_order(a, tp);
  assert(m0 == 0);
  assert(book.best_ask().has_value() && *book.best_ask() == 101.0);

  // Incoming buy id=2 price=102 qty=6 -> should trade at 101
  msim::Order b{2, 102.0, 6, msim::Side::BUY, 0};
  int m1 = book.add_order(b, tp);
  assert(m1 == 6);
  assert(tp == 101.0);

  // Ask should still exist (remaining 4)
  assert(book.best_ask().has_value() && *book.best_ask() == 101.0);

  // Cancel filled buy should fail (was never resting)
  assert(book.cancel_order(2) == false);

  // Cancel remaining ask should succeed
  assert(book.cancel_order(1) == true);
  assert(!book.best_ask().has_value());
}

static void test_price_time_priority_same_level() {
  std::vector<std::byte> buf(1 << 16);
  std::pmr::monotonic_buffer_resource mr(buf.data(), buf.size());

  msim::OrderBook book("X", &mr, /*tick_size=*/1.0);
  double tp = 0.0;

  // Two asks at same price level, id1 then id2
  msim::Order a1{1, 100.0, 5, msim::Side::SELL, 0};
  msim::Order a2{2, 100.0, 5, msim::Side::SELL, 1};
  assert(book.add_order(a1, tp) == 0);
  assert(book.add_order(a2, tp) == 0);
  assert(book.index_size() == 2);

  // Buy qty=6 at 100: should fully fill id1 (5) then partially fill id2 (1)
  msim::Order b{3, 100.0, 6, msim::Side::BUY, 2};
  assert(book.add_order(b, tp) == 6);
  assert(tp == 100.0);

  // id1 must be gone from index; id2 must remain
  assert(book.index_size() == 1);
  assert(book.cancel_order(1) == false);
  assert(book.cancel_order(2) == true);
  assert(book.index_size() == 0);
  assert(!book.best_ask().has_value());
}

int main() {
  test_basic_match_and_cancel();
  test_price_time_priority_same_level();
  std::cout << "OK: order_book\n";
  return 0;
}
