#include "msim/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
// #include <memory> // c++ 20
#include <new>

namespace msim {

static constexpr std::size_t kLevelCap = 2048;   // max distinct ticks per side
static constexpr std::size_t kIndexCap = 16384;  // max live resting orders

OrderBook::OrderBook(std::string symbol, std::pmr::memory_resource* mr, double tick_size)
    : bid_levels_(mr, kLevelCap, /*allow_grow=*/false),
      ask_levels_(mr, kLevelCap, /*allow_grow=*/false),
      index_(mr, kIndexCap, /*allow_grow=*/false),
      bid_ticks_(mr),
      ask_ticks_(mr),
      free_levels_(mr),
      symbol_(std::move(symbol)),
      mr_(mr),
      tick_size_(tick_size),
      inv_tick_(1.0 / tick_size) {
  if (!(tick_size_ > 0.0)) std::abort();
  bid_ticks_.reserve(512);
  ask_ticks_.reserve(512);
  free_levels_.reserve(256);
}

int32_t OrderBook::price_to_tick(double px) const noexcept {
  // Deterministic rounding; prices here are always positive in this sim.
  // Using llround on px*inv_tick is stable enough on MSVC for this workload.
  const double x = px * inv_tick_;
  return static_cast<int32_t>(std::llround(x));
}

OrderBook::Level* OrderBook::get_level(Side side, int32_t tick) noexcept {
  if (side == Side::BUY) {
    auto p = bid_levels_.find_ptr(tick);
    return p ? *p : nullptr;
  } else {
    auto p = ask_levels_.find_ptr(tick);
    return p ? *p : nullptr;
  }
}

const OrderBook::Level* OrderBook::get_level(Side side, int32_t tick) const noexcept {
  if (side == Side::BUY) {
    auto p = bid_levels_.find_ptr(tick);
    return p ? *p : nullptr;
  } else {
    auto p = ask_levels_.find_ptr(tick);
    return p ? *p : nullptr;
  }
}

void OrderBook::add_active_tick(Side side, int32_t tick) {
  auto& v = (side == Side::BUY) ? bid_ticks_ : ask_ticks_;
  v.push_back(tick);

  if (side == Side::BUY) {
    if (!best_bid_tick_ || tick > *best_bid_tick_) best_bid_tick_ = tick;
  } else {
    if (!best_ask_tick_ || tick < *best_ask_tick_) best_ask_tick_ = tick;
  }
}

void OrderBook::remove_active_tick(Side side, int32_t tick) {
  auto& v = (side == Side::BUY) ? bid_ticks_ : ask_ticks_;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == tick) {
      v[i] = v.back();
      v.pop_back();
      break;
    }
  }
}

void OrderBook::recompute_best(Side side) {
  auto& v = (side == Side::BUY) ? bid_ticks_ : ask_ticks_;
  if (v.empty()) {
    if (side == Side::BUY) best_bid_tick_.reset();
    else best_ask_tick_.reset();
    return;
  }

  int32_t best = v[0];
  if (side == Side::BUY) {
    for (std::size_t i = 1; i < v.size(); ++i) best = std::max(best, v[i]);
    best_bid_tick_ = best;
  } else {
    for (std::size_t i = 1; i < v.size(); ++i) best = std::min(best, v[i]);
    best_ask_tick_ = best;
  }
}

OrderBook::Level* OrderBook::get_or_create_level(Side side, int32_t tick) {
  Level* lvl = get_level(side, tick);
  if (lvl) return lvl;

  // allocate/reuse Level
  if (!free_levels_.empty()) {
    lvl = free_levels_.back();
    free_levels_.pop_back();
    lvl->reset(tick);
  } else {
    std::pmr::polymorphic_allocator<Level> a(mr_);
    lvl = a.allocate(1);
    // std::construct_at(lvl, tick, mr_); // c++20
    ::new (static_cast<void*>(lvl)) Level(tick, mr_);
  }

  bool ok = false;
  if (side == Side::BUY) ok = bid_levels_.insert(tick, lvl);
  else ok = ask_levels_.insert(tick, lvl);

  if (!ok) std::abort();  // should never happen; indicates capacity misuse

  add_active_tick(side, tick);
  return lvl;
}

void OrderBook::remove_level_if_empty(Side side, int32_t tick, Level* lvl) {
  if (!lvl->q.empty()) return;

  // remove from map
  if (side == Side::BUY) {
    (void)bid_levels_.erase(tick);
  } else {
    (void)ask_levels_.erase(tick);
  }

  remove_active_tick(side, tick);

  // update best if needed
  if (side == Side::BUY) {
    if (best_bid_tick_ && *best_bid_tick_ == tick) recompute_best(Side::BUY);
  } else {
    if (best_ask_tick_ && *best_ask_tick_ == tick) recompute_best(Side::SELL);
  }

  // keep level for reuse
  free_levels_.push_back(lvl);
}

int OrderBook::add_order(const Order& o, double& trade_price) {
  int remaining = o.qty;

  const int32_t tick = price_to_tick(o.price);
  const double snapped_px = tick_to_price(tick);

  if (o.side == Side::BUY) {
    // match against best asks while crossing
    while (remaining > 0 && best_ask_tick_ && *best_ask_tick_ <= tick) {
      const int32_t best_tick = *best_ask_tick_;
      Level* lvl = get_level(Side::SELL, best_tick);
      if (!lvl) {
        // should not happen if invariants hold; recompute and continue
        recompute_best(Side::SELL);
        continue;
      }

      auto& q = lvl->q;
      while (remaining > 0 && !q.empty()) {
        Order& top = q.front();
        const int traded = std::min(remaining, top.qty);
        remaining -= traded;
        top.qty -= traded;
        trade_price = top.price;

        if (top.qty == 0) {
          // Correctness: remove filled resting order from index
          index_.erase(top.id);
          q.pop_front();
        }
      }

      remove_level_if_empty(Side::SELL, best_tick, lvl);
      // loop continues if still crossing
    }

    if (remaining > 0) {
      Level* lvl = get_or_create_level(Side::BUY, tick);
      Order rest = o;
      rest.qty = remaining;
      rest.price = snapped_px;
      lvl->q.push_back(rest);

      // Index the resting order for cancels
      if (!index_.insert(rest.id, OrderRef{Side::BUY, tick})) std::abort();
    }

  } else {
    // SELL: match against best bids while crossing
    while (remaining > 0 && best_bid_tick_ && *best_bid_tick_ >= tick) {
      const int32_t best_tick = *best_bid_tick_;
      Level* lvl = get_level(Side::BUY, best_tick);
      if (!lvl) {
        recompute_best(Side::BUY);
        continue;
      }

      auto& q = lvl->q;
      while (remaining > 0 && !q.empty()) {
        Order& top = q.front();
        const int traded = std::min(remaining, top.qty);
        remaining -= traded;
        top.qty -= traded;
        trade_price = top.price;

        if (top.qty == 0) {
          index_.erase(top.id);
          q.pop_front();
        }
      }

      remove_level_if_empty(Side::BUY, best_tick, lvl);
    }

    if (remaining > 0) {
      Level* lvl = get_or_create_level(Side::SELL, tick);
      Order rest = o;
      rest.qty = remaining;
      rest.price = snapped_px;
      lvl->q.push_back(rest);
      if (!index_.insert(rest.id, OrderRef{Side::SELL, tick})) std::abort();
    }
  }

  return o.qty - remaining;
}

bool OrderBook::cancel_order(uint64_t order_id) {
  auto ref = index_.find_ptr(order_id);
  if (!ref) return false;

  const Side side = ref->side;
  const int32_t tick = ref->tick;

  Level* lvl = get_level(side, tick);
  if (!lvl) {
    // Stale index entry would imply bug; erase and return false.
    index_.erase(order_id);
    return false;
  }

  auto& q = lvl->q;
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (it->id == order_id) {
      q.erase(it);
      index_.erase(order_id);
      remove_level_if_empty(side, tick, lvl);
      return true;
    }
  }

  // If we got here, the index was stale. Clean it up.
  index_.erase(order_id);
  return false;
}

std::optional<double> OrderBook::best_bid() const {
  if (!best_bid_tick_) return std::nullopt;
  return tick_to_price(*best_bid_tick_);
}

std::optional<double> OrderBook::best_ask() const {
  if (!best_ask_tick_) return std::nullopt;
  return tick_to_price(*best_ask_tick_);
}

}  // namespace msim
