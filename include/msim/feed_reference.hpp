#pragma once

#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

#include "msim/digest.hpp"
#include "msim/feed_book.hpp"

namespace msim {

// A plain model of FeedBook for differential checks: ordered maps of FIFO
// lists, with nothing shared with FeedBook except its interface and its
// fault rules.
class FeedReference {
 public:
  using Locate = FeedBook::Locate;
  using Ref = FeedBook::Ref;
  using Price = FeedBook::Price;
  using Shares = FeedBook::Shares;
  using Status = FeedBook::Status;

  Status add(Locate loc, Ref ref, Side side, Shares shares, Price price) {
    if (side != Side::BUY && side != Side::SELL) {
      ++faults_.invalid_side;
      return Status::InvalidSide;
    }
    if (shares == 0) {
      ++faults_.zero_shares;
      return Status::ZeroShares;
    }
    if (orders_.count(ref)) {
      ++faults_.duplicate_ref;
      return Status::DuplicateRef;
    }
    orders_[ref] = Order{loc, side, price, shares};
    book(loc, side)[price].push_back(ref);
    return Status::Applied;
  }

  Status execute(Locate loc, Ref ref, Shares shares) { return reduce(loc, ref, shares); }
  Status cancel(Locate loc, Ref ref, Shares shares) { return reduce(loc, ref, shares); }

  Status remove(Locate loc, Ref ref) {
    const auto it = orders_.find(ref);
    if (it == orders_.end()) {
      ++faults_.unknown_ref;
      return Status::UnknownRef;
    }
    if (it->second.loc != loc) {
      ++faults_.locate_mismatch;
      return Status::LocateMismatch;
    }
    erase(it);
    return Status::Applied;
  }

  Status replace(Locate loc, Ref old_ref, Ref new_ref, Shares shares,
               Price price) {
    const auto it = orders_.find(old_ref);
    if (it == orders_.end()) {
      ++faults_.unknown_ref;
      return Status::UnknownRef;
    }
    if (it->second.loc != loc) {
      ++faults_.locate_mismatch;
      return Status::LocateMismatch;
    }
    if (shares == 0) {
      ++faults_.zero_shares;
      return Status::ZeroShares;
    }
    if (orders_.count(new_ref)) {
      ++faults_.duplicate_ref;
      return Status::DuplicateRef;
    }
    const Side side = it->second.side;
    erase(it);
    return add(loc, new_ref, side, shares, price);
  }

  std::optional<Price> best(Locate loc, Side s) const {
    const auto b = books_.find({loc, s == Side::SELL});
    if (b == books_.end() || b->second.empty()) return std::nullopt;
    return s == Side::BUY ? b->second.rbegin()->first
                          : b->second.begin()->first;
  }

  std::optional<FeedBook::QueuePosition> position(Ref ref) const {
    const auto it = orders_.find(ref);
    if (it == orders_.end()) return std::nullopt;
    const Order& o = it->second;
    const auto& queue = books_.at({o.loc, o.side == Side::SELL}).at(o.price);
    std::uint32_t ahead = 0;
    for (auto r = queue.begin(); *r != ref; ++r) ++ahead;
    return FeedBook::QueuePosition{best(o.loc, o.side) == o.price, ahead,
                                   static_cast<std::uint32_t>(queue.size())};
  }

  const FeedBook::Faults& faults() const noexcept { return faults_; }
  std::size_t orders() const noexcept { return orders_.size(); }

  template <typename F>
  void for_each(Locate loc, Side s, F&& f) const {
    const auto b = books_.find({loc, s == Side::SELL});
    if (b == books_.end()) return;
    const auto visit = [&](const auto& level) {
      for (Ref r : level.second) f(level.first, r, orders_.at(r).shares);
    };
    if (s == Side::BUY)
      for (auto l = b->second.rbegin(); l != b->second.rend(); ++l) visit(*l);
    else
      for (const auto& l : b->second) visit(l);
  }

  // Same words in the same order as FeedBook::digest.
  std::uint64_t digest() const {
    Digest d;
    for (const auto& [key, levels] : books_) {
      if (levels.empty()) continue;
      d.mix(std::uint64_t{key.first} << 1 | key.second);
      for_each(key.first, key.second ? Side::SELL : Side::BUY,
               [&](Price p, Ref r, Shares sh) {
                 d.mix(p);
                 d.mix(r);
                 d.mix(sh);
               });
    }
    return d.value();
  }

 private:
  struct Order {
    Locate loc;
    Side side;
    Price price;
    Shares shares;
  };
  using Levels = std::map<Price, std::list<Ref>>;

  Levels& book(Locate loc, Side s) { return books_[{loc, s == Side::SELL}]; }

  Status reduce(Locate loc, Ref ref, Shares shares) {
    const auto it = orders_.find(ref);
    if (it == orders_.end()) {
      ++faults_.unknown_ref;
      return Status::UnknownRef;
    }
    Order& o = it->second;
    if (o.loc != loc) {
      ++faults_.locate_mismatch;
      return Status::LocateMismatch;
    }
    if (shares == 0) {
      ++faults_.zero_shares;
      return Status::ZeroShares;
    }
    if (shares > o.shares) {
      ++faults_.over_execution;
      return Status::OverExecution;
    }
    o.shares -= shares;
    if (o.shares == 0) erase(it);
    return Status::Applied;
  }

  void erase(std::unordered_map<Ref, Order>::iterator it) {
    const Order o = it->second;
    Levels& levels = book(o.loc, o.side);
    auto level = levels.find(o.price);
    level->second.remove(it->first);
    if (level->second.empty()) levels.erase(level);
    orders_.erase(it);
  }

  // Keyed by (locate, is_sell), so iteration runs locate, then bid, then ask.
  std::map<std::pair<Locate, bool>, Levels> books_;
  std::unordered_map<Ref, Order> orders_;
  FeedBook::Faults faults_;
};

}  // namespace msim
