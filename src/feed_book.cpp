#include "msim/feed_book.hpp"

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "msim/ladder.hpp"

namespace msim {

FeedBook::FeedBook() : FeedBook(Options{}) {}

const char* FeedBook::status_name(Status s) noexcept {
  switch (s) {
    case Status::Applied: return "applied";
    case Status::UnknownRef: return "unknown_ref";
    case Status::DuplicateRef: return "duplicate_ref";
    case Status::LocateMismatch: return "locate_mismatch";
    case Status::OverExecution: return "over_execution";
    case Status::ZeroShares: return "zero_shares";
    case Status::Capacity: return "capacity";
    case Status::InvalidSide: return "invalid_side";
  }
  return "invalid_status";
}

FeedBook::Status FeedBook::fault(Status s) noexcept {
  switch (s) {
    case Status::Applied: break;
    case Status::UnknownRef: ++faults_.unknown_ref; break;
    case Status::DuplicateRef: ++faults_.duplicate_ref; break;
    case Status::LocateMismatch: ++faults_.locate_mismatch; break;
    case Status::OverExecution: ++faults_.over_execution; break;
    case Status::ZeroShares: ++faults_.zero_shares; break;
    case Status::Capacity: ++faults_.capacity; break;
    case Status::InvalidSide: ++faults_.invalid_side; break;
  }
  return s;
}

namespace {

std::size_t validated(std::size_t max_orders) {
  if (max_orders >= UINT32_MAX)
    throw std::invalid_argument("FeedBook: max_orders exceeds 32-bit indices");
  return max_orders;
}

}  // namespace

FeedBook::FeedBook(Options o)
    : books_(std::size_t{1} << 16),
      refs_(validated(o.max_orders), o.reference_mode, o.reference_window_bits),
      max_orders_(o.max_orders),
      max_levels_(std::min(validated(o.max_levels), o.max_orders)) {
  nodes_.reserve(o.max_orders);
  levels_.reserve(max_levels_);
  free_levels_.reserve(max_levels_);
}

auto FeedBook::new_node() -> Index {
  if (free_node_ != kNil) {
    const Index n = free_node_;
    free_node_ = nodes_[n].next;
    return n;
  }
  nodes_.emplace_back();
  return static_cast<Index>(nodes_.size() - 1);
}

auto FeedBook::open_level(Locate loc, Side s, Price p) -> Index {
  Ladder& l = ladder(loc, s);
  const std::uint32_t k = key(s, p);
  const std::size_t i =
      detail::lower_bound_from_best(l.keys.data(), l.keys.size(), k);
  if (i < l.keys.size() && l.keys[i] == k) return l.levels[i];

  Index li;
  if (!free_levels_.empty()) {
    li = free_levels_.back();
    free_levels_.pop_back();
  } else {
    levels_.emplace_back();
    li = static_cast<Index>(levels_.size() - 1);
  }
  levels_[li] = Level{p, kNil, kNil, 0, loc, s, 0};
  const auto at = static_cast<std::ptrdiff_t>(i);
  l.keys.insert(l.keys.begin() + at, k);
  l.levels.insert(l.levels.begin() + at, li);
  ++live_levels_;
  return li;
}

void FeedBook::close_level(Index li) noexcept {
  const Level& lvl = levels_[li];
  Ladder& l = ladder(lvl.locate, lvl.side);
  const auto at = static_cast<std::ptrdiff_t>(detail::lower_bound_from_best(
      l.keys.data(), l.keys.size(), key(lvl.side, lvl.price)));
  l.keys.erase(l.keys.begin() + at);
  l.levels.erase(l.levels.begin() + at);
  --live_levels_;
  free_levels_.push_back(li);
}

void FeedBook::unlink(Index n) noexcept {
  Node& x = nodes_[n];
  Level& lvl = levels_[x.level];
  lvl.shares -= x.shares;
  (x.prev != kNil ? nodes_[x.prev].next : lvl.head) = x.next;
  (x.next != kNil ? nodes_[x.next].prev : lvl.tail) = x.prev;
  --lvl.count;
  const Index li = x.level;
  x.next = free_node_;
  free_node_ = n;
  if (lvl.head == kNil) close_level(li);
}

FeedBook::Status FeedBook::add(Locate loc, Ref ref, Side side, Shares shares,
                              Price price) {
  if (side != Side::BUY && side != Side::SELL) return fault(Status::InvalidSide);
  if (shares == 0) {
    return fault(Status::ZeroShares);
  }
  if (refs_.contains(ref)) {
    return fault(Status::DuplicateRef);
  }
  if (refs_.size() >= max_orders_) {
    return fault(Status::Capacity);
  }
  if (live_levels_ == max_levels_ && !level(loc, side, price).orders)
    return fault(Status::Capacity);
  const Index n = new_node();
  // Total live references are below the hash's admission limit, so a window
  // slide can migrate every displaced live entry before admitting this one.
  if (!refs_.insert(ref, n))
    throw std::logic_error("reference index rejected admitted order");
  const Index li = open_level(loc, side, price);
  Level& lvl = levels_[li];
  nodes_[n] = Node{ref, shares, lvl.tail, kNil, li};
  (lvl.tail != kNil ? nodes_[lvl.tail].next : lvl.head) = n;
  lvl.tail = n;
  ++lvl.count;
  lvl.shares += shares;
  if (refs_.size() > peak_orders_) peak_orders_ = refs_.size();
  return Status::Applied;
}

FeedBook::Status FeedBook::reduce(Locate loc, Ref ref, Shares shares) noexcept {
  const Index n = refs_.find(ref);
  if (n == RefMap::kEmpty) {
    return fault(Status::UnknownRef);
  }
  Node& x = nodes_[n];
  if (levels_[x.level].locate != loc) return fault(Status::LocateMismatch);
  if (shares == 0) return fault(Status::ZeroShares);
  if (shares > x.shares) {
    return fault(Status::OverExecution);
  }
  levels_[x.level].shares -= shares;
  x.shares -= shares;
  if (x.shares > 0) return Status::Applied;
  (void)refs_.take(ref);
  unlink(n);
  return Status::Applied;
}

FeedBook::Status FeedBook::execute(Locate loc, Ref ref, Shares shares) {
  return reduce(loc, ref, shares);
}

FeedBook::Status FeedBook::cancel(Locate loc, Ref ref, Shares shares) {
  return reduce(loc, ref, shares);
}

FeedBook::Status FeedBook::remove(Locate loc, Ref ref) {
  const Index n = refs_.find(ref);
  if (n == RefMap::kEmpty) return fault(Status::UnknownRef);
  if (levels_[nodes_[n].level].locate != loc) return fault(Status::LocateMismatch);
  (void)refs_.take(ref);
  unlink(n);
  return Status::Applied;
}

FeedBook::Status FeedBook::replace(Locate loc, Ref old_ref, Ref new_ref,
                                  Shares shares, Price price) {
  const Index n = refs_.find(old_ref);
  if (n == RefMap::kEmpty) return fault(Status::UnknownRef);
  const Level& old = levels_[nodes_[n].level];
  if (old.locate != loc) return fault(Status::LocateMismatch);
  if (shares == 0) return fault(Status::ZeroShares);
  if (refs_.contains(new_ref)) return fault(Status::DuplicateRef);
  const Side side = old.side;
  if (live_levels_ == max_levels_ && old.count != 1 &&
      !level(loc, side, price).orders) return fault(Status::Capacity);
  (void)refs_.take(old_ref);
  unlink(n);
  return add(loc, new_ref, side, shares, price);
}

std::optional<FeedBook::OrderState> FeedBook::order(Ref ref) const noexcept {
  const Index n = refs_.find(ref);
  if (n == RefMap::kEmpty) return std::nullopt;
  const Node& node = nodes_[n];
  const Level& l = levels_[node.level];
  return OrderState{l.locate, l.side, l.price, node.shares};
}

FeedBook::LevelState FeedBook::level(Locate loc, Side side, Price price) const noexcept {
  const auto& l = ladder(loc, side);
  const auto k = key(side, price);
  const auto at = detail::lower_bound_from_best(l.keys.data(), l.keys.size(), k);
  if (at == l.keys.size() || l.keys[at] != k) return {};
  const auto& value = levels_[l.levels[at]];
  return {value.shares, value.count};
}

std::optional<itch::Price> FeedBook::best(Locate loc,
                                          Side s) const noexcept {
  const Ladder& l = ladder(loc, s);
  if (l.levels.empty()) return std::nullopt;
  return levels_[l.levels.back()].price;
}

std::size_t FeedBook::levels(Locate loc, Side s) const noexcept {
  return ladder(loc, s).levels.size();
}

std::optional<FeedBook::Shares> FeedBook::shares(Ref ref) const noexcept {
  const Index n = refs_.find(ref);
  if (n == RefMap::kEmpty) return std::nullopt;
  return nodes_[n].shares;
}

auto FeedBook::position(Ref ref) const noexcept
    -> std::optional<QueuePosition> {
  const Index n = refs_.find(ref);
  if (n == RefMap::kEmpty) return std::nullopt;
  const Node& x = nodes_[n];
  const Level& lvl = levels_[x.level];
  const Ladder& l = ladder(lvl.locate, lvl.side);
  QueuePosition q{l.levels.back() == x.level, 0, lvl.count};
  for (Index p = x.prev; p != kNil; p = nodes_[p].prev) ++q.ahead;
  return q;
}

std::uint64_t FeedBook::digest() const noexcept {
  Digest d;
  for (std::size_t loc = 0; loc < books_.size(); ++loc)
    digest_into(d, static_cast<Locate>(loc));
  return d.value();
}

void FeedBook::digest_into(Digest& d, Locate loc) const noexcept {
  for (Side s : {Side::BUY, Side::SELL}) {
    if (ladder(loc, s).levels.empty()) continue;
    d.mix(std::uint64_t{loc} << 1 | (s == Side::SELL));
    for_each(loc, s, [&](Price p, Ref ref, Shares sh) {
      d.mix(p);
      d.mix(ref);
      d.mix(sh);
    });
  }
}

bool FeedBook::check_invariants(std::string* why) const {
  std::string problem;
  const auto fail = [&](std::string msg) {
    if (problem.empty()) problem = std::move(msg);
  };

  std::vector<bool> level_seen(levels_.size(), false);
  std::vector<bool> node_seen(nodes_.size(), false);
  std::unordered_set<Ref> refs;
  std::size_t live_levels = 0;
  for (std::size_t loc = 0; loc < books_.size(); ++loc) {
    for (Side s : {Side::BUY, Side::SELL}) {
      const Ladder& l = ladder(static_cast<Locate>(loc), s);
      const std::string where = "locate " + std::to_string(loc) +
                                (s == Side::BUY ? " bid" : " ask");
      if (l.keys.size() != l.levels.size())
        fail(where + " ladder sizes differ");
      for (std::size_t i = 0; i < l.levels.size() && i < l.keys.size(); ++i) {
        const Index li = l.levels[i];
        if (i > 0 && l.keys[i - 1] >= l.keys[i]) fail(where + " out of order");
        if (li >= levels_.size() || level_seen[li]) {
          fail(where + " has a bad or shared level");
          continue;
        }
        level_seen[li] = true;
        ++live_levels;
        const Level& lvl = levels_[li];
        if (lvl.locate != loc || lvl.side != s ||
            key(s, lvl.price) != l.keys[i])
          fail(where + " level disagrees with its key");
        if (lvl.head == kNil) fail(where + " level is empty");

        Index prev = kNil;
        std::uint32_t count = 0;
        std::uint64_t total_shares = 0;
        for (Index n = lvl.head; n != kNil && count <= nodes_.size();
             prev = n, n = nodes_[n].next, ++count) {
          if (n >= nodes_.size() || node_seen[n]) {
            fail("bad, shared, or cyclic order index");
            break;
          }
          node_seen[n] = true;
          const Node& x = nodes_[n];
          const std::string id = "ref " + std::to_string(x.ref);
          if (x.prev != prev) fail(id + " has a broken back link");
          if (x.level != li) fail(id + " names the wrong level");
          if (x.shares == 0) fail(id + " rests with no shares");
          total_shares += x.shares;
          if (!refs.insert(x.ref).second) fail(id + " rests twice");
          if (refs_.find(x.ref) != n) fail(id + " is not indexed at its node");
        }
        if (prev != lvl.tail) fail(where + " level has a stale tail");
        if (count != lvl.count) fail(where + " level count is wrong");
        if (total_shares != lvl.shares) fail(where + " level shares are wrong");
      }
    }
  }
  if (refs.size() != refs_.size())
    fail("order index holds a ref that rests nowhere");
  if (live_levels != live_levels_) fail("live level count is wrong");
  if (live_levels > max_levels_) fail("live level capacity exceeded");
  if (live_levels + free_levels_.size() != levels_.size())
    fail("levels leaked or double freed");
  for (Index li : free_levels_) {
    if (li >= levels_.size() || level_seen[li]) {
      fail("bad, shared, or duplicate free level");
      break;
    }
    level_seen[li] = true;
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
  if (free_nodes + refs.size() != nodes_.size())
    fail("nodes leaked or double freed");

  if (problem.empty()) return true;
  if (why) *why = problem;
  return false;
}

}  // namespace msim
