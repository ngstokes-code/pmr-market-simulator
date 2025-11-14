#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory_resource>
#include <msim/event.hpp>
#include <optional>
#include <string>

namespace msim {

struct Order {
  uint64_t id;
  double price;
  int qty;
  Side side;  // BUY or SELL
  uint64_t ts_ns;
};

class OrderBook {
 public:
  OrderBook(std::string symbol, std::pmr::memory_resource* mr);

  int add_order(const Order& o, double& trade_price);
  bool cancel_order(uint64_t order_id);

  std::optional<double> best_bid() const;
  std::optional<double> best_ask() const;
  const std::string& symbol() const { return symbol_; }

 private:
  using Queue = std::pmr::deque<Order>;
  using BidMap = std::pmr::map<double, Queue, std::greater<double>>;
  using AskMap = std::pmr::map<double, Queue, std::less<double>>;
  using IndexMap = std::pmr::map<uint64_t, std::pair<Side, double>>;

  BidMap bids_;
  AskMap asks_;
  IndexMap index_;
  std::string symbol_;
  std::pmr::memory_resource* mr_;
};

}  // namespace msim
