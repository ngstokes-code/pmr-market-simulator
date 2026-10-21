#include "msim/reference_book.hpp"

#include <algorithm>

namespace msim {

namespace {

template <typename Side>
std::size_t count_orders(const Side& side) {
  std::size_t n = 0;
  for (const auto& [tick, queue] : side) n += queue.size();
  return n;
}

template <typename Side>
void append_levels(const Side& side, std::vector<LevelSnapshot>& out) {
  for (const auto& [tick, queue] : side) {
    LevelSnapshot lvl{tick, {}};
    for (const auto& r : queue) lvl.orders.emplace_back(r.id, r.qty);
    out.push_back(std::move(lvl));
  }
}

}  // namespace

// Walks the opposite side in map order, which is priority order, and
// executes against the front of each queue while the price crosses.
template <typename Opposite>
Qty ReferenceBook::match(const Order& o, Opposite& opposite,
                         std::vector<Execution>& out) {
  Qty remaining = o.qty;
  while (remaining > 0 && !opposite.empty()) {
    auto level = opposite.begin();
    const Tick price = level->first;
    const bool crosses =
        o.side == Side::BUY ? price <= o.tick : price >= o.tick;
    if (!crosses) break;

    Queue& queue = level->second;
    while (remaining > 0 && !queue.empty()) {
      Resting& front = queue.front();
      const Qty n = std::min(remaining, front.qty);
      out.push_back(Execution{front.id, o.id, price, n});
      remaining -= n;
      front.qty -= n;
      if (front.qty == 0) queue.pop_front();
    }
    if (queue.empty()) opposite.erase(level);
  }
  return remaining;
}

bool ReferenceBook::rests(OrderId id) const {
  for (const auto& [tick, queue] : bids_)
    for (const auto& r : queue)
      if (r.id == id) return true;
  for (const auto& [tick, queue] : asks_)
    for (const auto& r : queue)
      if (r.id == id) return true;
  return false;
}

// Is there room in (bids, asks) for one more order at o.tick?
bool ReferenceBook::remainder_fits_in(const Order& o, const Bids& bids,
                                      const Asks& asks) const {
  if (count_orders(bids) + count_orders(asks) >= limits_.max_resting_orders)
    return false;
  const bool level_exists =
      o.side == Side::BUY ? bids.count(o.tick) > 0 : asks.count(o.tick) > 0;
  const std::size_t levels = o.side == Side::BUY ? bids.size() : asks.size();
  return level_exists || levels < limits_.max_levels_per_side;
}

bool ReferenceBook::remainder_fits(const Order& o) const {
  return remainder_fits_in(o, bids_, asks_);
}

AddResult ReferenceBook::add(const Order& o, std::vector<Execution>& out) {
  AddResult r;
  if (o.side != Side::BUY && o.side != Side::SELL) {
    r.status = AddStatus::InvalidSide;
    return r;
  }
  if (o.qty <= 0) {
    r.status = AddStatus::InvalidQty;
    return r;
  }
  if (o.tick < 1 || o.tick > limits_.max_tick) {
    r.status = AddStatus::InvalidTick;
    return r;
  }
  if (rests(o.id)) {
    r.status = AddStatus::DuplicateId;
    return r;
  }

  // Matching only removes orders. So if a remainder would fit now, it still
  // fits afterwards, and the book can match in place. Otherwise, match a copy
  // of the whole book and keep it only if the result is legal: slow, and
  // obviously atomic. That path settles every case where a limit is full.
  std::vector<Execution> execs;
  Qty remaining = 0;
  if (remainder_fits(o)) {
    remaining =
        o.side == Side::BUY ? match(o, asks_, execs) : match(o, bids_, execs);
  } else {
    Bids bids = bids_;
    Asks asks = asks_;
    remaining =
        o.side == Side::BUY ? match(o, asks, execs) : match(o, bids, execs);
    if (remaining > 0 && !remainder_fits_in(o, bids, asks)) {
      r.status = AddStatus::CapacityFull;
      return r;
    }
    bids_ = std::move(bids);
    asks_ = std::move(asks);
  }
  if (remaining > 0) {
    if (o.side == Side::BUY)
      bids_[o.tick].push_back(Resting{o.id, remaining});
    else
      asks_[o.tick].push_back(Resting{o.id, remaining});
  }

  out.insert(out.end(), execs.begin(), execs.end());
  r.filled = o.qty - remaining;
  r.rested = remaining;
  return r;
}

bool ReferenceBook::cancel(OrderId id) {
  for (auto level = bids_.begin(); level != bids_.end(); ++level) {
    auto& q = level->second;
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->id != id) continue;
      q.erase(it);
      if (q.empty()) bids_.erase(level);
      return true;
    }
  }
  for (auto level = asks_.begin(); level != asks_.end(); ++level) {
    auto& q = level->second;
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->id != id) continue;
      q.erase(it);
      if (q.empty()) asks_.erase(level);
      return true;
    }
  }
  return false;
}

std::optional<Tick> ReferenceBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Tick> ReferenceBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::size_t ReferenceBook::resting_orders() const {
  return count_orders(bids_) + count_orders(asks_);
}

BookSnapshot ReferenceBook::snapshot() const {
  BookSnapshot s;
  append_levels(bids_, s.bids);
  append_levels(asks_, s.asks);
  return s;
}

}  // namespace msim
