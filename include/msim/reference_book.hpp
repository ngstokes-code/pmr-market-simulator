#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "msim/book_types.hpp"

namespace msim {

// A deliberately plain book, written from the matching contract alone, to
// check the engine against. It favours obviousness over speed:
//  - each side is an ordered map from tick to a FIFO queue,
//  - there is no order index, so cancel and duplicate checks scan everything,
//  - while a limit is full, an add is matched on a copy of the book and the
//    copy is kept only if the result is legal.
// It shares no code with OrderBook beyond the types in book_types.hpp.
class ReferenceBook {
 public:
  explicit ReferenceBook(BookLimits limits = {}) : limits_(limits) {}

  [[nodiscard]] AddResult add(const Order& o, std::vector<Execution>& out);
  [[nodiscard]] bool cancel(OrderId id);

  std::optional<Tick> best_bid() const;
  std::optional<Tick> best_ask() const;
  std::size_t resting_orders() const;
  BookSnapshot snapshot() const;

 private:
  struct Resting {
    OrderId id;
    Qty qty;
  };
  using Queue = std::deque<Resting>;
  // Map order is priority order: best price first on both sides.
  using Bids = std::map<Tick, Queue, std::greater<Tick>>;
  using Asks = std::map<Tick, Queue, std::less<Tick>>;

  template <typename Opposite>
  static Qty match(const Order& o, Opposite& opposite,
                   std::vector<Execution>& out);

  bool rests(OrderId id) const;
  bool remainder_fits(const Order& o) const;
  bool remainder_fits_in(const Order& o, const Bids& bids,
                         const Asks& asks) const;

  BookLimits limits_;
  Bids bids_;
  Asks asks_;
};

}  // namespace msim
