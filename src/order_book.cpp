#include "order_book.hpp"

namespace msim {

OrderBook::OrderBook(std::string symbol, std::pmr::memory_resource* mr)
    : bids_(
          std::greater<double>(),
          std::pmr::polymorphic_allocator<std::pair<const double, Queue>>(mr)),
      asks_(
          std::less<double>(),
          std::pmr::polymorphic_allocator<std::pair<const double, Queue>>(mr)),
      index_(
          std::less<uint64_t>(),
          std::pmr::polymorphic_allocator<std::pair<const double, Queue>>(mr)),
      symbol_(std::move(symbol)),
      mr_(mr) {}

int OrderBook::add_order(const Order& o, double& trade_price) {
  int remaining = o.qty;
  if (o.side == 'B') {
    while (remaining > 0 && !asks_.empty()) {
      auto it = asks_.begin();
      if (o.price < it->first) break;
      auto& q = it->second;
      while (remaining > 0 && !q.empty()) {
        auto& top = q.front();
        int traded = std::min(remaining, top.qty);
        remaining -= traded;
        top.qty -= traded;
        trade_price = top.price;
        if (top.qty == 0) q.pop_front();
        if (remaining == 0) break;
      }
      if (q.empty()) asks_.erase(it);
    }
    if (remaining > 0) {
      Order rest = o;
      rest.qty = remaining;
      bids_[o.price].push_back(rest);
      index_.emplace(o.id, std::make_pair('B', o.price));
    }
  } else {
    while (remaining > 0 && !bids_.empty()) {
      auto it = bids_.begin();
      if (o.price > it->first) break;
      auto& q = it->second;
      while (remaining > 0 && !q.empty()) {
        auto& top = q.front();
        int traded = std::min(remaining, top.qty);
        remaining -= traded;
        top.qty -= traded;
        trade_price = top.price;
        if (top.qty == 0) q.pop_front();
        if (remaining == 0) break;
      }
      if (q.empty()) bids_.erase(it);
    }
    if (remaining > 0) {
      Order rest = o;
      rest.qty = remaining;
      asks_[o.price].push_back(rest);
      index_.emplace(o.id, std::make_pair('S', o.price));
    }
  }
  return o.qty - remaining;
}

bool OrderBook::cancel_order(uint64_t order_id) {
  auto it = index_.find(order_id);
  if (it == index_.end()) return false;
  char side = it->second.first;
  double price = it->second.second;
  bool removed = false;
  if (side == 'B') {
    auto lvl = bids_.find(price);
    if (lvl != bids_.end()) {
      auto& q = lvl->second;
      for (auto qi = q.begin(); qi != q.end(); ++qi) {
        if (qi->id == order_id) {
          q.erase(qi);
          removed = true;
          break;
        }
      }
      if (q.empty()) bids_.erase(lvl);
    }
  } else {
    auto lvl = asks_.find(price);
    if (lvl != asks_.end()) {
      auto& q = lvl->second;
      for (auto qi = q.begin(); qi != q.end(); ++qi) {
        if (qi->id == order_id) {
          q.erase(qi);
          removed = true;
          break;
        }
      }
      if (q.empty()) asks_.erase(lvl);
    }
  }
  if (removed) index_.erase(it);
  return removed;
}

std::optional<double> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}
std::optional<double> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

}  // namespace msim
