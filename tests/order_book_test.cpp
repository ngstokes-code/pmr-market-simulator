#include "msim/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

#include "check.hpp"

using msim::AddResult;
using msim::AddStatus;
using msim::BookLimits;
using msim::Execution;
using msim::Order;
using msim::OrderBook;
using msim::Side;

namespace msim {
struct OrderBookTestAccess {
  static void corrupt(OrderBook& b, unsigned kind) {
    const auto li = b.bids_.levels.back();
    const auto node = b.levels_[li].head;
    if (kind == 0) b.levels_[li].head = OrderBook::kNil - 1;
    if (kind == 1) b.nodes_[node].next = node;
    if (kind == 2) b.free_node_ = node;
    if (kind == 3) b.free_levels_.push_back(li);
    if (kind == 4) b.free_node_ = OrderBook::kNil - 1;
  }
};
}  // namespace msim

namespace {

// Passes allocations through to the heap and tracks what is still live, so a
// test can see whether destroying a book handed every block back.
class OutstandingResource : public std::pmr::memory_resource {
 public:
  std::size_t outstanding() const noexcept { return outstanding_; }
  std::size_t allocations() const noexcept { return allocations_; }

 private:
  void* do_allocate(std::size_t bytes, std::size_t align) override {
    outstanding_ += bytes;
    ++allocations_;
    return std::pmr::new_delete_resource()->allocate(bytes, align);
  }
  void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
    outstanding_ -= bytes;
    std::pmr::new_delete_resource()->deallocate(p, bytes, align);
  }
  bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
    return this == &o;
  }
  std::size_t outstanding_ = 0;
  std::size_t allocations_ = 0;
};

struct Fixture {
  std::pmr::unsynchronized_pool_resource mr;
  OrderBook book;
  std::vector<Execution> out;

  explicit Fixture(BookLimits limits = {}) : book("X", &mr, limits) {}

  AddResult add(msim::OrderId id, Side side, msim::Tick tick, msim::Qty qty) {
    out.clear();
    const AddResult r = book.add(Order{id, side, tick, qty}, out);
    std::string why;
    if (!CHECK(book.check_invariants(&why)))
      std::fprintf(stderr, "  %s\n", why.c_str());
    return r;
  }

  bool cancel(msim::OrderId id) {
    const bool ok = book.cancel(id);
    std::string why;
    if (!CHECK(book.check_invariants(&why)))
      std::fprintf(stderr, "  %s\n", why.c_str());
    return ok;
  }
};

bool accepted(const AddResult& r, msim::Qty filled, msim::Qty rested) {
  return r.status == AddStatus::Accepted && r.filled == filled &&
         r.rested == rested;
}

void test_partial_execution_and_cancel() {
  Fixture f;
  CHECK(accepted(f.add(1, Side::SELL, 101, 10), 0, 10));
  CHECK(f.book.best_ask() == 101);

  // Executes at the resting price, not the incoming limit.
  CHECK(accepted(f.add(2, Side::BUY, 102, 6), 6, 0));
  CHECK((f.out == std::vector<Execution>{{1, 2, 101, 6}}));
  CHECK(f.book.best_ask() == 101);

  CHECK(!f.cancel(2));  // fully executed on arrival; never rested
  CHECK(f.cancel(1));
  CHECK(!f.book.best_ask());
  CHECK(f.book.resting_orders() == 0);
}

void test_fifo_within_level() {
  Fixture f;
  CHECK(accepted(f.add(1, Side::SELL, 100, 5), 0, 5));
  CHECK(accepted(f.add(2, Side::SELL, 100, 5), 0, 5));

  CHECK(accepted(f.add(3, Side::BUY, 100, 6), 6, 0));
  CHECK((f.out == std::vector<Execution>{{1, 3, 100, 5}, {2, 3, 100, 1}}));

  CHECK(f.book.resting_orders() == 1);
  CHECK(!f.cancel(1));
  CHECK(f.cancel(2));
  CHECK(!f.book.best_ask());
}

void test_cancel_from_middle_keeps_order() {
  Fixture f;
  for (msim::OrderId id = 1; id <= 3; ++id) (void)f.add(id, Side::BUY, 50, 1);
  CHECK(f.cancel(2));
  CHECK(accepted(f.add(9, Side::SELL, 50, 2), 2, 0));
  CHECK((f.out == std::vector<Execution>{{1, 9, 50, 1}, {3, 9, 50, 1}}));
}

void test_sweep_then_rest_remainder() {
  Fixture f;
  (void)f.add(1, Side::SELL, 101, 5);
  (void)f.add(2, Side::SELL, 102, 5);
  (void)f.add(3, Side::SELL, 104, 5);

  // Crosses 101 and 102, stops before 104, rests the rest at its limit.
  CHECK(accepted(f.add(4, Side::BUY, 103, 12), 10, 2));
  CHECK((f.out == std::vector<Execution>{{1, 4, 101, 5}, {2, 4, 102, 5}}));
  CHECK(f.book.best_bid() == 103);
  CHECK(f.book.best_ask() == 104);
  CHECK(f.book.levels(Side::SELL) == 1);
}

void test_best_price_after_cancel() {
  Fixture f;
  (void)f.add(1, Side::BUY, 99, 1);
  (void)f.add(2, Side::BUY, 97, 1);
  (void)f.add(3, Side::BUY, 98, 1);
  CHECK(f.book.best_bid() == 99);
  CHECK(f.cancel(1));
  CHECK(f.book.best_bid() == 98);
  CHECK(f.cancel(3));
  CHECK(f.book.best_bid() == 97);
  CHECK(f.cancel(2));
  CHECK(!f.book.best_bid());
}

void test_rejections_leave_book_unchanged() {
  Fixture f(BookLimits{100, 100, 1000});
  (void)f.add(1, Side::SELL, 100, 5);

  CHECK(f.add(2, Side::BUY, 100, 0).status == AddStatus::InvalidQty);
  CHECK(f.add(2, Side::BUY, 100, -3).status == AddStatus::InvalidQty);
  CHECK(f.add(2, static_cast<Side>(3), 100, 1).status == AddStatus::InvalidSide);
  CHECK(f.add(2, Side::BUY, 0, 1).status == AddStatus::InvalidTick);
  CHECK(f.add(2, Side::BUY, 1001, 1).status == AddStatus::InvalidTick);
  CHECK(f.out.empty());

  // A live id is refused before matching, even though it would cross.
  const AddResult dup = f.add(1, Side::BUY, 100, 5);
  CHECK(dup.status == AddStatus::DuplicateId);
  CHECK(dup.filled == 0 && f.out.empty());
  CHECK(f.book.best_ask() == 100);

  // Once order 1 is gone, its id may be used again.
  (void)f.add(2, Side::BUY, 100, 5);
  CHECK(accepted(f.add(1, Side::BUY, 90, 1), 0, 1));
}

// The bid side holds its one allowed level. An order that would leave a
// remainder at a new bid price is refused whole: nothing executes.
void test_level_limit_rejects_whole_order() {
  Fixture f(BookLimits{10, 1, 1000});
  (void)f.add(1, Side::SELL, 100, 5);
  (void)f.add(2, Side::BUY, 99, 5);

  const AddResult r = f.add(3, Side::BUY, 100, 8);
  CHECK(r.status == AddStatus::CapacityFull);
  CHECK(r.filled == 0 && f.out.empty());
  CHECK(f.book.best_ask() == 100);  // the ask is untouched

  // The same order sized to execute in full needs no level, so it passes.
  CHECK(accepted(f.add(3, Side::BUY, 100, 5), 5, 0));
  // Joining the existing level needs no new level either.
  CHECK(accepted(f.add(4, Side::BUY, 99, 2), 0, 2));
}

// With the order index full, a remainder can still rest when matching frees
// a slot first. That happens exactly when the order crosses at least one
// resting order and still has quantity left.
void test_order_limit_counts_slots_freed_by_matching() {
  Fixture f(BookLimits{2, 10, 1000});
  CHECK(accepted(f.add(1, Side::SELL, 100, 5), 0, 5));
  CHECK(accepted(f.add(2, Side::BUY, 90, 1), 0, 1));
  CHECK(f.book.resting_orders() == 2);

  // No crossing: no slot frees, so the order is refused.
  CHECK(f.add(3, Side::BUY, 95, 1).status == AddStatus::CapacityFull);

  // Executes order 1 in full, which frees its slot for the remainder.
  CHECK(accepted(f.add(4, Side::BUY, 100, 8), 5, 3));
  CHECK((f.out == std::vector<Execution>{{1, 4, 100, 5}}));
  CHECK(f.book.resting_orders() == 2);
  CHECK(f.book.best_bid() == 100);
}

template <typename Book>
void test_no_allocation_after_construction() {
  OutstandingResource mr;
  {
    Book book("X", &mr, BookLimits{1000, 100, 100000});
    std::vector<Execution> out;
    out.reserve(16);
    const std::size_t after_construction = mr.allocations();
    // Most adds are executed away at once, which empties their level and
    // sends it to the reuse pool. Every seventh stays, so active and pooled
    // levels both exist when the book is destroyed.
    for (msim::OrderId id = 1; id <= 500; ++id) {
      const auto tick = static_cast<msim::Tick>(1000 + id % 60);
      (void)book.add(Order{id, Side::SELL, tick, 3}, out);
      if (id % 7 != 0)
        (void)book.add(Order{100000 + id, Side::BUY, tick, 3}, out);
    }
    for (msim::OrderId id = 7; id <= 500; id += 14) (void)book.cancel(id);
    CHECK(book.check_invariants());
    CHECK(mr.allocations() == after_construction);
    CHECK(mr.outstanding() > 0);
  }
  CHECK(mr.outstanding() == 0);
}

}  // namespace

int main() {
  for (unsigned kind = 0; kind < 5; ++kind) {
    std::pmr::unsynchronized_pool_resource resource;
    OrderBook corrupt("X", &resource, BookLimits{4, 4, 10000});
    std::vector<Execution> out;
    (void)corrupt.add(Order{1, Side::BUY, 5000, 100}, out);
    msim::OrderBookTestAccess::corrupt(corrupt, kind);
    std::string why;
    CHECK(!corrupt.check_invariants(&why) && !why.empty());
    bool refused_snapshot = false;
    try { (void)corrupt.snapshot(); }
    catch (const std::logic_error&) { refused_snapshot = true; }
    CHECK(refused_snapshot);
  }
  test_partial_execution_and_cancel();
  test_fifo_within_level();
  test_cancel_from_middle_keeps_order();
  test_sweep_then_rest_remainder();
  test_best_price_after_cancel();
  test_rejections_leave_book_unchanged();
  test_level_limit_rejects_whole_order();
  test_order_limit_counts_slots_freed_by_matching();
  test_no_allocation_after_construction<OrderBook>();
  return check::finish("order_book");
}
