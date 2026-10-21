#include "msim/command.hpp"
#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"

#include <array>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <vector>

#include "check.hpp"

namespace { bool fail_next_allocation = false; }
void* operator new(std::size_t n) {
  if (fail_next_allocation) { fail_next_allocation = false; throw std::bad_alloc(); }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace msim {
struct OrderBookTestAccess {
  static void reverse_two_order_fifo(OrderBook& book) {
    for (auto index:book.asks_.levels) {
      auto& level=book.levels_[index];const auto first=level.head;
      if (first==OrderBook::kNil) continue;
      const auto second=book.nodes_[first].next;
      if (second==OrderBook::kNil || book.nodes_[second].next!=OrderBook::kNil) continue;
      level.head=second;level.tail=first;
      book.nodes_[second].prev=OrderBook::kNil;book.nodes_[second].next=first;
      book.nodes_[first].prev=second;book.nodes_[first].next=OrderBook::kNil;
      return;
    }
  }
};
}

namespace {
using namespace msim;
std::uint64_t transitions = 0, sequences = 0;

Qty quantity(const BookSnapshot& s) {
  Qty n = 0;
  for (const auto* side : {&s.bids, &s.asks})
    for (const auto& l : *side)
      for (const auto& o : l.orders) n += o.second;
  return n;
}

bool compare(const std::vector<Command>& trace, BookLimits limits,
             unsigned mutant = 0) {
  std::pmr::unsynchronized_pool_resource resource;
  OrderBook book("tiny", &resource, limits);
  ReferenceBook reference(limits);
  std::vector<Execution> a, b;
  a.reserve(limits.max_resting_orders);
  for (const auto& command : trace) {
    const auto before = reference.snapshot();
    if (mutant==1) OrderBookTestAccess::reverse_two_order_fifo(book);
    Outcome got;
    if (mutant == 2 && command.type == CommandType::Cancel) {
      a.clear();got.type = command.type; got.canceled = true;
    } else {
      got = apply(book, command, a);
    }
    const auto expected = apply(reference, command, b);
    if (mutant == 3 && command.type == CommandType::Add &&
        expected.add.status == AddStatus::CapacityFull)
      {
        // Planted preflight omission: execute the first share, then report the
        // original rejection with no execution. Only independent state catches it.
        auto partial=command;partial.qty=1;(void)apply(book,partial,a);a.clear();
      }
    if (!(got == expected) || a != b ||
        !(book.snapshot() == reference.snapshot()) || !book.check_invariants())
      return false;
    if (command.type == CommandType::Add) {
      Qty executed = 0;
      for (const auto& x : a) {
        if (x.qty <= 0 || x.incoming_id != command.id) return false;
        executed += x.qty;
      }
      if (got.add.status == AddStatus::Accepted) {
        if (got.add.filled + got.add.rested != command.qty ||
            executed != got.add.filled ||
            quantity(book.snapshot()) != quantity(before) - executed + got.add.rested)
          return false;
      } else if (!a.empty() || !(book.snapshot() == before)) return false;
    }
    ++transitions;
  }
  return true;
}

void enumerate(std::vector<Command>& prefix, const std::vector<Command>& domain,
               BookLimits limits, unsigned depth) {
  if (!CHECK(compare(prefix, limits))) return;
  ++sequences;
  if (depth == 0) return;
  for (auto c : domain) {
    c.seq = prefix.size() + 1;
    prefix.push_back(c);
    enumerate(prefix, domain, limits, depth - 1);
    prefix.pop_back();
  }
}

void exhaustive() {
  // No state merging: histories with identical public state can have
  // different free lists and hash clusters. Enumerate every sequence <= 4.
  std::vector<Command> domain;
  for (OrderId id = 1; id <= 3; ++id) {
    domain.push_back(Command::cancel(0, id));
    for (Side side : {Side::BUY, Side::SELL})
      for (Tick price : {1, 2})
        for (Qty size : {1, 2})
          domain.push_back(Command::add(0, id, side, price, size));
  }
  CHECK(domain.size() == 27);
  std::vector<Command> trace;
  for (std::size_t levels : {std::size_t{1}, std::size_t{2}})
    enumerate(trace, domain, BookLimits{2, levels, 2}, 4);
  CHECK(sequences == 2 * (1 + 27 + 27*27 + 27*27*27 + 27*27*27*27));
  CHECK(!compare({Command::add(1, 1, Side::SELL, 1, 1),
                  Command::add(2, 2, Side::SELL, 1, 1),
                  Command::add(3, 3, Side::BUY, 1, 1)}, {2, 2, 2}, 1));
  CHECK(!compare({Command::add(1, 1, Side::BUY, 1, 1),
                  Command::cancel(2, 1)}, {2, 2, 2}, 2));
  CHECK(!compare({Command::add(1, 1, Side::BUY, 1, 1),
                  Command::add(2, 2, Side::SELL, 2, 1),
                  Command::add(3, 3, Side::BUY, 2, 2)}, {2, 1, 2}, 3));
  std::printf("all-sequence bound: ids=3 prices=2 quantities=2 depth=4 profiles=2 sequences=%llu transitions=%llu\n",
              static_cast<unsigned long long>(sequences),
              static_cast<unsigned long long>(transitions));
}

void exception_boundary() {
  std::pmr::unsynchronized_pool_resource resource;
  OrderBook book("exceptions", &resource, {4, 4, 100});
  std::vector<Execution> output;
  (void)book.add({1, Side::SELL, 10, 1}, output);
  (void)book.add({2, Side::SELL, 11, 1}, output);
  const auto before = book.snapshot();
  fail_next_allocation = true;
  bool threw = false;
  try { (void)book.add({3, Side::BUY, 12, 3}, output); }
  catch (const std::bad_alloc&) { threw = true; }
  fail_next_allocation = false;
  CHECK(threw && output.empty() && book.snapshot() == before);
  CHECK(book.check_invariants());
  output.reserve(4);
  fail_next_allocation = true;
  const auto result = book.add({3, Side::BUY, 12, 3}, output);
  const bool no_allocation = fail_next_allocation;
  fail_next_allocation = false;
  CHECK(no_allocation && result.filled == 2 && result.rested == 1);
  CHECK(output.size() == 2 && book.check_invariants());
  const auto state = book.snapshot();
  const auto prior_output = output;
  auto malformed = Command::cancel(4, 3);
  malformed.type = static_cast<CommandType>(255);
  bool rejected = false;
  try { (void)apply(book, malformed, output); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected && book.snapshot() == state && output == prior_output);
}
}  // namespace

int main() {
  exception_boundary();
  exhaustive();
  return check::finish("exhaustive matching and mutation boundary");
}
