
#pragma once
#include <map>
#include <deque>
#include <optional>
#include <string>
#include <cstdint>
#include <memory_resource>

namespace msim {

struct Order {
    uint64_t id;
    double price;
    int qty;
    char side; // 'B' or 'S'
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
    using IndexMap = std::pmr::map<uint64_t, std::pair<char,double>>;

    BidMap bids_;
    AskMap asks_;
    IndexMap index_;
    std::string symbol_;
    std::pmr::memory_resource* mr_;
};

} // namespace msim
