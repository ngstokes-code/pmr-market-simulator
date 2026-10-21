#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "msim/book_types.hpp"
#include "msim/flat_hash.hpp"
#include "msim/types.hpp"

namespace msim {

// Price-time priority limit order book for one instrument.
//
// Internal storage is sized from BookLimits and allocated from `mr` in the
// constructor. Caller-owned execution output and diagnostics can allocate.
// A single thread owns each book; `mr` must outlive it.
//
// Layout:
//   orders  one slab of fixed-size nodes, linked into per-level FIFO lists
//           by 32-bit indices. Each node knows its level.
//   levels  one slab of levels for both sides.
//   ladder  per side, the active levels sorted so the best price is last,
//           plus a hash from tick to level. Adding to an existing level is
//           one hash lookup. Opening or closing a level searches from the
//           best end and moves the suffix above that position. Near-best
//           changes are cheap; deep arbitrary changes can move O(levels).
//   index   order id -> node, so a cancel uses one hash lookup.
class OrderBook {
 public:
  // Throws std::invalid_argument if a limit does not fit a 32-bit index.
  OrderBook(std::string symbol, std::pmr::memory_resource* mr,
            BookLimits limits = {});

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // Matches o against the opposite side, best price first and oldest first
  // within a price, and appends one Execution per resting order it touches.
  // Any remainder rests at o.tick behind existing orders at that price.
  //
  // A rejected order changes nothing. Every rejection, CapacityFull
  // included, is decided before the first execution.
  //
  // Insufficient caller output capacity is reserved before mutation. If
  // reservation throws, both book and output retain their prior state.
  // Mutation then performs no allocation and appending cannot throw.
  // A caller reserving max_resting_orders avoids output planning/allocation.
  //
  // Expected cost: O(1) per execution with well-distributed hash keys, plus
  // ladder search/movement up to O(levels). A hash operation is worst-case
  // linear in its table capacity. Full limits or insufficient caller output
  // reservation require a read-only crossing-order scan before mutation.
  [[nodiscard]] AddResult add(const Order& o, std::vector<Execution>& out);

  // Removes a resting order. Returns false if no order with this id rests.
  // One hash lookup and O(1) unlink, plus a ladder update if the price empties.
  // Expected constant index lookup; worst-case O(orders + levels).
  [[nodiscard]] bool cancel(OrderId id) noexcept;

  std::optional<Tick> best_bid() const noexcept { return best(Side::BUY); }
  std::optional<Tick> best_ask() const noexcept { return best(Side::SELL); }

  std::size_t resting_orders() const noexcept { return index_.size(); }
  std::size_t levels(Side s) const noexcept { return ladder(s).keys.size(); }
  const BookLimits& limits() const noexcept { return limits_; }
  const std::string& symbol() const noexcept { return symbol_; }

  // Validates then copies both sides. Diagnostic allocation is permitted;
  // corrupted indices throw rather than being traversed. Worst-case O(n^2).
  BookSnapshot snapshot() const;

  // Full consistency check (worst-case quadratic hash work). Returns false and, if
  // `why` is non-null, describes the first violated invariant.
  bool check_invariants(std::string* why = nullptr) const;

 private:
  friend struct OrderBookTestAccess;
  using Index = std::uint32_t;
  static constexpr Index kNil = UINT32_MAX;

  struct Node {
    OrderId id;
    Qty qty;
    Index prev;
    Index next;  // also links the free list
    Index level;
  };

  struct Level {
    Tick tick;
    Index head;
    Index tail;
    Side side;
  };

  // Keys grow toward the best price on both sides: the tick for bids, its
  // negation for asks. One search routine then serves both sides.
  struct Ladder {
    std::pmr::vector<Tick> keys;
    std::pmr::vector<Index> levels;
    FlatHashMap<Tick, Index> by_tick;
    Ladder(std::pmr::memory_resource* mr, std::size_t max_levels)
        : keys(mr), levels(mr), by_tick(mr, max_levels) {}
  };

  static Tick key(Side s, Tick t) noexcept { return s == Side::BUY ? t : -t; }
  static bool crosses(Side incoming, Tick limit, Tick resting) noexcept {
    return incoming == Side::BUY ? resting <= limit : resting >= limit;
  }

  Ladder& ladder(Side s) noexcept { return s == Side::BUY ? bids_ : asks_; }
  const Ladder& ladder(Side s) const noexcept {
    return s == Side::BUY ? bids_ : asks_;
  }
  std::optional<Tick> best(Side s) const noexcept;

  static std::size_t lower_bound(const Ladder& l, Tick k) noexcept;
  Index find_level(Side s, Tick t) const noexcept;
  Index open_level(Side s, Tick t) noexcept;
  void close_level(Index lvl) noexcept;
  bool fits_after_matching(const Order& o) const noexcept;

  std::string symbol_;
  BookLimits limits_;
  std::pmr::vector<Node> nodes_;
  std::pmr::vector<Level> levels_;
  std::pmr::vector<Index> free_levels_;
  Index free_node_ = kNil;
  Ladder bids_;
  Ladder asks_;
  FlatHashMap<OrderId, Index> index_;
};

}  // namespace msim
