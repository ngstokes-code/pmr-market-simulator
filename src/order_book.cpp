#include "msim/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "msim/ladder.hpp"

namespace msim {

namespace {

BookLimits validated(BookLimits l) {
  constexpr std::size_t kMax = UINT32_MAX - 1;  // UINT32_MAX is kNil
  if (l.max_resting_orders > kMax || l.max_levels_per_side > kMax / 2)
    throw std::invalid_argument("BookLimits exceed 32-bit indices");
  return l;
}

}  // namespace

OrderBook::OrderBook(std::string symbol, std::pmr::memory_resource* mr,
                     BookLimits limits)
    : symbol_(std::move(symbol)),
      limits_(validated(limits)),
      nodes_(limits_.max_resting_orders, mr),
      levels_(2 * limits_.max_levels_per_side, mr),
      free_levels_(mr),
      bids_(mr, limits_.max_levels_per_side),
      asks_(mr, limits_.max_levels_per_side),
      index_(mr, limits_.max_resting_orders) {
  for (std::size_t i = 0; i + 1 < nodes_.size(); ++i)
    nodes_[i].next = static_cast<Index>(i + 1);
  if (!nodes_.empty()) {
    nodes_.back().next = kNil;
    free_node_ = 0;
  }
  free_levels_.reserve(levels_.size());
  for (std::size_t i = levels_.size(); i-- > 0;)
    free_levels_.push_back(static_cast<Index>(i));
  for (Ladder* l : {&bids_, &asks_}) {
    l->keys.reserve(limits_.max_levels_per_side);
    l->levels.reserve(limits_.max_levels_per_side);
  }
}

std::optional<Tick> OrderBook::best(Side s) const noexcept {
  const Ladder& l = ladder(s);
  if (l.keys.empty()) return std::nullopt;
  return levels_[l.levels.back()].tick;
}

std::size_t OrderBook::lower_bound(const Ladder& l, Tick k) noexcept {
  return detail::lower_bound_from_best(l.keys.data(), l.keys.size(), k);
}

auto OrderBook::find_level(Side s, Tick t) const noexcept -> Index {
  const Index* p = ladder(s).by_tick.find_ptr(t);
  return p ? *p : kNil;
}

// Precondition: a level at t exists, or the side has room for one.
auto OrderBook::open_level(Side s, Tick t) noexcept -> Index {
  Ladder& l = ladder(s);
  if (const Index* p = l.by_tick.find_ptr(t)) return *p;
  const Tick k = key(s, t);
  const std::size_t i = lower_bound(l, k);

  const Index lvl = free_levels_.back();
  free_levels_.pop_back();
  levels_[lvl] = Level{t, kNil, kNil, s};
  // Capacity was reserved in the constructor, so neither insert allocates.
  const auto at = static_cast<std::ptrdiff_t>(i);
  l.keys.insert(l.keys.begin() + at, k);
  l.levels.insert(l.levels.begin() + at, lvl);
  (void)l.by_tick.insert(t, lvl);
  return lvl;
}

void OrderBook::close_level(Index lvl) noexcept {
  const Level& v = levels_[lvl];
  Ladder& l = ladder(v.side);
  const auto at =
      static_cast<std::ptrdiff_t>(lower_bound(l, key(v.side, v.tick)));
  l.keys.erase(l.keys.begin() + at);
  l.levels.erase(l.levels.begin() + at);
  (void)l.by_tick.erase(v.tick);
  free_levels_.push_back(lvl);
}

// Decides, before anything changes, whether o can complete when a limit is
// full now. Two facts make this exact without simulating the match:
//  - Matching removes orders only from the opposite side, so a missing level
//    slot on o's own side stays missing.
//  - If a remainder exists, every crossing resting order executed in full,
//    so the index frees one slot per crossed order.
bool OrderBook::fits_after_matching(const Order& o) const noexcept {
  const Ladder& opp = ladder(opposite(o.side));
  Qty crossing = 0;
  for (std::size_t i = opp.levels.size(); i-- > 0;) {
    const Level& lvl = levels_[opp.levels[i]];
    if (!crosses(o.side, o.tick, lvl.tick)) break;
    for (Index n = lvl.head; n != kNil; n = nodes_[n].next) {
      // Compare before adding, so the sum stays below o.qty.
      if (nodes_[n].qty >= o.qty - crossing) return true;
      crossing += nodes_[n].qty;
    }
  }
  const bool level_ok =
      ladder(o.side).keys.size() < limits_.max_levels_per_side ||
      find_level(o.side, o.tick) != kNil;
  const bool slot_ok = !index_.full() || crossing > 0;
  return level_ok && slot_ok;
}

AddResult OrderBook::add(const Order& o, std::vector<Execution>& out) {
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
  if (index_.contains(o.id)) {
    r.status = AddStatus::DuplicateId;
    return r;
  }
  const bool room =
      !index_.full() &&
      (ladder(o.side).keys.size() < limits_.max_levels_per_side ||
       find_level(o.side, o.tick) != kNil);
  if (!room && !fits_after_matching(o)) {
    r.status = AddStatus::CapacityFull;
    return r;
  }

  // Reserve before the first mutation. The measured path supplies enough
  // caller capacity and avoids this scan. Execution is trivially copyable;
  // after reservation, appending cannot allocate or throw.
  if (out.capacity() - out.size() < index_.size()) {
    std::size_t required = 0;
    Qty remaining = o.qty;
    const Ladder& opposite_side = ladder(opposite(o.side));
    for (std::size_t i = opposite_side.levels.size(); i-- > 0 && remaining;) {
      const Level& level = levels_[opposite_side.levels[i]];
      if (!crosses(o.side, o.tick, level.tick)) break;
      for (Index n = level.head; n != kNil && remaining; n = nodes_[n].next) {
        ++required;
        remaining -= std::min(remaining, nodes_[n].qty);
      }
    }
    if (required > out.max_size() - out.size())
      throw std::length_error("execution output size is not representable");
    out.reserve(out.size() + required);
  }

  Ladder& opp = ladder(opposite(o.side));
  Qty remaining = o.qty;
  while (remaining > 0 && !opp.levels.empty()) {
    const Index li = opp.levels.back();
    Level& lvl = levels_[li];
    if (!crosses(o.side, o.tick, lvl.tick)) break;
    // Output capacity was established before any book mutation.
    for (;;) {
      Node& top = nodes_[lvl.head];
      const Qty q = std::min(remaining, top.qty);
      out.push_back(Execution{top.id, o.id, lvl.tick, q});
      remaining -= q;
      top.qty -= q;
      if (top.qty > 0) break;

      (void)index_.erase(top.id);
      const Index n = lvl.head;
      lvl.head = top.next;
      top.next = free_node_;
      free_node_ = n;
      if (lvl.head == kNil) {
        lvl.tail = kNil;
        opp.keys.pop_back();
        opp.levels.pop_back();
        (void)opp.by_tick.erase(lvl.tick);
        free_levels_.push_back(li);
        break;
      }
      nodes_[lvl.head].prev = kNil;
      if (remaining == 0) break;
    }
  }

  r.filled = o.qty - remaining;
  if (remaining == 0) return r;

  // The capacity check above guarantees a free node, a free index slot, and
  // a level at o.tick or room to open one.
  const Index li = open_level(o.side, o.tick);
  Level& lvl = levels_[li];
  const Index n = free_node_;
  free_node_ = nodes_[n].next;
  nodes_[n] = Node{o.id, remaining, lvl.tail, kNil, li};
  (lvl.tail != kNil ? nodes_[lvl.tail].next : lvl.head) = n;
  lvl.tail = n;
  (void)index_.insert(o.id, n);
  r.rested = remaining;
  return r;
}

bool OrderBook::cancel(OrderId id) noexcept {
  const std::optional<Index> found = index_.take(id);
  if (!found) return false;
  const Index n = *found;
  Node& x = nodes_[n];
  const Index li = x.level;
  Level& lvl = levels_[li];
  (x.prev != kNil ? nodes_[x.prev].next : lvl.head) = x.next;
  (x.next != kNil ? nodes_[x.next].prev : lvl.tail) = x.prev;
  x.next = free_node_;
  free_node_ = n;
  if (lvl.head == kNil) close_level(li);
  return true;
}

BookSnapshot OrderBook::snapshot() const {
  std::string why;
  if (!check_invariants(&why))
    throw std::logic_error("cannot snapshot corrupt book: " + why);
  BookSnapshot snap;
  for (Side s : {Side::BUY, Side::SELL}) {
    const Ladder& l = ladder(s);
    auto& dst = s == Side::BUY ? snap.bids : snap.asks;
    dst.reserve(l.levels.size());
    for (std::size_t i = l.levels.size(); i-- > 0;) {
      const Level& lvl = levels_[l.levels[i]];
      LevelSnapshot out{lvl.tick, {}};
      for (Index n = lvl.head; n != kNil; n = nodes_[n].next)
        out.orders.emplace_back(nodes_[n].id, nodes_[n].qty);
      dst.push_back(std::move(out));
    }
  }
  return snap;
}

bool OrderBook::check_invariants(std::string* why) const {
  std::string problem;
  const auto fail = [&](std::string msg) {
    if (problem.empty()) problem = std::move(msg);
  };

  std::unordered_set<OrderId> seen;
  std::vector<bool> level_used(levels_.size(), false);
  std::vector<bool> node_seen(nodes_.size(), false);
  std::size_t live_levels = 0;
  for (Side s : {Side::BUY, Side::SELL}) {
    const Ladder& l = ladder(s);
    const std::string name = s == Side::BUY ? "bid" : "ask";
    if (l.keys.size() != l.levels.size() || l.by_tick.size() != l.keys.size())
      fail(name + " ladder arrays differ in size");
    if (!l.by_tick.check_invariants())
      fail(name + " tick hash breaks its probing rule");
    if (l.keys.size() > limits_.max_levels_per_side)
      fail(name + " side exceeds max_levels_per_side");
    for (std::size_t i = 0; i < l.keys.size() && i < l.levels.size(); ++i) {
      const Index li = l.levels[i];
      const std::string at = name + " level " + std::to_string(i);
      if (i > 0 && l.keys[i - 1] >= l.keys[i]) fail(at + " is out of order");
      if (li >= levels_.size() || level_used[li]) {
        fail(at + " has a bad or shared level index");
        continue;
      }
      level_used[li] = true;
      ++live_levels;
      const Level& lvl = levels_[li];
      if (lvl.tick < 1 || lvl.tick > limits_.max_tick) {
        fail(at + " has an invalid tick");
        continue;
      }
      if (lvl.side != s || key(s, lvl.tick) != l.keys[i])
        fail(at + " disagrees with its key");
      const Index* hashed = l.by_tick.find_ptr(lvl.tick);
      if (!hashed || *hashed != li) fail(at + " is missing from the tick hash");
      if (lvl.head == kNil) fail(at + " is empty");

      Index prev = kNil;
      std::size_t steps = 0;
      for (Index n = lvl.head; n != kNil && steps <= nodes_.size();
           prev = n, n = nodes_[n].next, ++steps) {
        if (n >= nodes_.size() || node_seen[n]) {
          fail("bad, shared, or cyclic order index");
          break;
        }
        node_seen[n] = true;
        const Node& x = nodes_[n];
        const std::string id = "order " + std::to_string(x.id);
        if (x.prev != prev) fail(id + " has a broken back link");
        if (x.level != li) fail(id + " names the wrong level");
        if (x.qty <= 0) fail(id + " rests with qty <= 0");
        if (!seen.insert(x.id).second) fail(id + " rests twice");
        const Index* in = index_.find_ptr(x.id);
        if (!in || *in != n) fail(id + " is not indexed at its node");
      }
      if (steps > nodes_.size()) fail(at + " has a cycle");
      if (prev != lvl.tail) fail(at + " has a stale tail");
    }
  }

  if (seen.size() != index_.size())
    fail("index holds an order that rests nowhere");
  if (!index_.check_invariants()) fail("order index breaks its probing rule");
  if (live_levels + free_levels_.size() != levels_.size())
    fail("levels leaked or double freed");

  for (Index li : free_levels_) {
    if (li >= levels_.size() || level_used[li]) {
      fail("bad, shared, or duplicate free level");
      break;
    }
    level_used[li] = true;
  }
  std::size_t free_nodes = 0;
  for (Index n = free_node_; n != kNil; n = nodes_[n].next) {
    if (n >= nodes_.size() || node_seen[n]) {
      fail("bad, shared, or cyclic free node");
      break;
    }
    node_seen[n] = true;
    ++free_nodes;
  }
  if (free_nodes + seen.size() != nodes_.size())
    fail("order nodes leaked or double freed");

  if (problem.empty()) {
    const auto bb = best_bid(), ba = best_ask();
    if (bb && ba && *bb >= *ba) fail("book is crossed");
  }

  if (problem.empty()) return true;
  if (why) *why = problem;
  return false;
}

}  // namespace msim
