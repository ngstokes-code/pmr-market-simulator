#pragma once

// The matching contract shared by every book implementation: what goes in,
// what comes out, and what a book's state looks like from outside. The
// engine (order_book.hpp) and the reference (reference_book.hpp) share these
// types and nothing else.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <type_traits>
#include <vector>

#include "msim/types.hpp"

namespace msim {

struct Order {
  OrderId id{};
  Side side{Side::BUY};
  Tick tick{};
  Qty qty{};
};

// One execution between an incoming order and one resting order. It always
// prints at the resting order's price.
struct Execution {
  OrderId resting_id{};
  OrderId incoming_id{};
  Tick tick{};
  Qty qty{};

  friend bool operator==(const Execution& a, const Execution& b) noexcept {
    return a.resting_id == b.resting_id && a.incoming_id == b.incoming_id &&
           a.tick == b.tick && a.qty == b.qty;
  }
  friend bool operator!=(const Execution& a, const Execution& b) noexcept {
    return !(a == b);
  }
};
static_assert(std::is_nothrow_copy_constructible_v<Execution>,
              "pre-reserved execution output must not throw during mutation");

struct BookLimits {
  std::size_t max_resting_orders = 12'000;  // across both sides
  std::size_t max_levels_per_side = 1'600;
  Tick max_tick = 1'000'000'000;
};

enum class AddStatus : std::uint8_t {
  Accepted,
  InvalidQty,   // qty <= 0
  InvalidTick,  // tick outside [1, max_tick]
  DuplicateId,  // an order with this id is resting now
  // Matching would leave a remainder that exceeds a BookLimits capacity.
  // The book refuses the whole order rather than execute part of it.
  CapacityFull,
  InvalidSide,
};

const char* to_string(AddStatus s) noexcept;

// filled + rested == qty for an accepted order. Both are 0 for a rejected one.
struct AddResult {
  AddStatus status{AddStatus::Accepted};
  Qty filled{0};
  Qty rested{0};

  friend bool operator==(const AddResult& a, const AddResult& b) noexcept {
    return a.status == b.status && a.filled == b.filled && a.rested == b.rested;
  }
};

// One price level as seen from outside: its tick and its resting orders as
// (id, remaining qty), oldest first.
struct LevelSnapshot {
  Tick tick{};
  std::vector<std::pair<OrderId, Qty>> orders;

  friend bool operator==(const LevelSnapshot& a, const LevelSnapshot& b) {
    return a.tick == b.tick && a.orders == b.orders;
  }
};

// Both sides in priority order: bids from highest tick, asks from lowest.
struct BookSnapshot {
  std::vector<LevelSnapshot> bids;
  std::vector<LevelSnapshot> asks;

  friend bool operator==(const BookSnapshot& a, const BookSnapshot& b) {
    return a.bids == b.bids && a.asks == b.asks;
  }
};

}  // namespace msim
