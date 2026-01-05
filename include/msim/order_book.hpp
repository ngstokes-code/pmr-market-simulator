#pragma once

#include <cstdint>
#include <deque>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "msim/event.hpp"
#include "msim/flat_hash.hpp"

namespace msim {

struct Order {
  uint64_t id;
  double price;
  int qty;
  Side side;     // BUY or SELL
  uint64_t ts_ns;
};

class OrderBook {
 public:
  // tick_size defaults to 0.01. Keep default so existing call sites don't change.
  OrderBook(std::string symbol, std::pmr::memory_resource* mr, double tick_size = 0.01);

  int add_order(const Order& o, double& trade_price);
  bool cancel_order(uint64_t order_id);

  std::optional<double> best_bid() const;
  std::optional<double> best_ask() const;

  const std::string& symbol() const { return symbol_; }

  // Debug / test hook (helps validate index cleanup & invariants).
  std::size_t index_size() const noexcept { return index_.size(); }

 private:
  struct OrderRef {
    Side side;
    int32_t tick;
  };

  struct Level {
    int32_t tick{0};
    std::pmr::deque<Order> q;

    Level(int32_t t, std::pmr::memory_resource* mr) : tick(t), q(mr) {}

    void reset(int32_t t) {
      tick = t;
      q.clear();
    }
  };

  // Price -> tick conversions (positive price assumption is fine for this sim)
  int32_t price_to_tick(double px) const noexcept;
  double tick_to_price(int32_t t) const noexcept { return double(t) * tick_size_; }

  Level* get_or_create_level(Side side, int32_t tick);
  Level* get_level(Side side, int32_t tick) noexcept;
  const Level* get_level(Side side, int32_t tick) const noexcept;

  void remove_level_if_empty(Side side, int32_t tick, Level* lvl);

  void add_active_tick(Side side, int32_t tick);
  void remove_active_tick(Side side, int32_t tick);
  void recompute_best(Side side);

  // Fixed-capacity maps to avoid pmr monotonic rehash leaks.
  // Tune caps as needed.
  FlatHashMap<int32_t, Level*> bid_levels_;
  FlatHashMap<int32_t, Level*> ask_levels_;
  FlatHashMap<uint64_t, OrderRef> index_;

  std::pmr::vector<int32_t> bid_ticks_;
  std::pmr::vector<int32_t> ask_ticks_;
  std::pmr::vector<Level*> free_levels_;

  std::optional<int32_t> best_bid_tick_;
  std::optional<int32_t> best_ask_tick_;

  std::string symbol_;
  std::pmr::memory_resource* mr_{nullptr};

  double tick_size_{0.01};
  double inv_tick_{100.0};
};

}  // namespace msim
