#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "msim/digest.hpp"
#include "msim/flat_hash.hpp"
#include "msim/itch.hpp"
#include "msim/ref_map.hpp"
#include "msim/types.hpp"

namespace msim {

// Every symbol's book, rebuilt from an exchange feed.
//
// Unlike OrderBook this never matches. Semantic rejections preserve displayed
// state; allocation exceptions invalidate the run rather than permit recovery.
//
// Orders and levels live in two market-wide slabs linked by 32-bit indices,
// so the slabs may grow without invalidating anything. RefMap finds an
// order by reference. Each symbol keeps, per side, its levels sorted with
// the best price last, and that ladder is also how a level is found by
// price. Sharing that lookup avoids a second price index, at the cost of
// linear movement when a level opens or closes far from the best end.
class FeedBook {
 public:
  using Locate = std::uint16_t;
  using Ref = itch::Ref;
  using Price = itch::Price;
  using Shares = std::uint32_t;

  struct Options {
    std::size_t max_orders = 4'000'000;  // live across this owner's locates
    std::size_t max_levels = 1'000'000;  // live, across all owned locates
    RefMap::Mode reference_mode = RefMap::Mode::Window;
    unsigned reference_window_bits = 24;
  };

  enum class Status {
    Applied, UnknownRef, DuplicateRef, LocateMismatch, OverExecution, ZeroShares, Capacity,
    InvalidSide
  };
  static const char* status_name(Status s) noexcept;

  struct Faults {
    std::uint64_t unknown_ref = 0;      // modify or delete of no live order
    std::uint64_t duplicate_ref = 0;    // add with a live reference
    std::uint64_t over_execution = 0;   // more shares than the order holds
    std::uint64_t locate_mismatch = 0;  // message locate differs from order's
    std::uint64_t zero_shares = 0;      // add, replace, or reduction of no shares
    std::uint64_t capacity = 0;         // live-order or level limit reached
    std::uint64_t invalid_side = 0;
    std::uint64_t total() const noexcept {
      return unknown_ref + duplicate_ref + over_execution + locate_mismatch +
             zero_shares + capacity + invalid_side;
    }
  };

  // Reconstructed displayed rank, not a claim about venue execution priority.
  struct QueuePosition {
    bool at_best;        // its level is the best on its side
    std::uint32_t ahead; // orders before it at its level
    std::uint32_t queue; // orders at its level
  };
  struct OrderState { Locate locate; Side side; Price price; Shares shares; };
  struct LevelState { std::uint64_t shares = 0; std::uint32_t orders = 0; };
  std::optional<OrderState> order(Ref ref) const noexcept;
  LevelState level(Locate loc, Side side, Price price) const noexcept;

  // Throws std::invalid_argument if max_orders does not fit a 32-bit index.
  FeedBook();
  explicit FeedBook(Options o);

  // One call per book-changing message. Each verifies the message against
  // the book first and counts a fault instead of applying a bad one.
  Status add(Locate loc, Ref ref, Side side, Shares shares, Price price);
  Status execute(Locate loc, Ref ref, Shares shares);  // E and C
  Status cancel(Locate loc, Ref ref, Shares shares);   // X: partial cancel
  Status remove(Locate loc, Ref ref);                  // D
  // U: the old order leaves, and the new one joins the back of its level.
  Status replace(Locate loc, Ref old_ref, Ref new_ref, Shares shares,
                 Price price);

  std::optional<Price> best(Locate loc, Side s) const noexcept;
  std::optional<QueuePosition> position(Ref ref) const noexcept;
  std::optional<Shares> shares(Ref ref) const noexcept;

  std::size_t orders() const noexcept { return refs_.size(); }
  // Live orders older than RefMap's window, held in its hash.
  std::size_t old_orders() const noexcept { return refs_.old_size(); }
  // References that arrived far past RefMap's window.
  std::uint64_t ref_jumps() const noexcept { return refs_.jumps(); }
  std::size_t peak_orders() const noexcept { return peak_orders_; }
  std::size_t levels() const noexcept { return live_levels_; }
  std::size_t levels(Locate loc, Side s) const noexcept;
  Faults faults() const noexcept {
    Faults f = faults_;
    f.capacity += refs_.lost();
    return f;
  }

  // Calls f(price, ref, shares) for each order on one side, best price
  // first and oldest first within a price.
  template <typename F>
  void for_each(Locate loc, Side s, F&& f) const;

  // Hash of every order in every book, in locate, side, priority order.
  std::uint64_t digest() const noexcept;
  // Adds one symbol's orders to d, as digest() does for each symbol. Lets
  // books split across several FeedBooks produce the same digest.
  void digest_into(Digest& d, Locate loc) const noexcept;

  bool check_invariants(std::string* why = nullptr) const;

 private:
  friend struct FeedBookTestAccess;
  using Index = std::uint32_t;
  static constexpr Index kNil = UINT32_MAX;

  struct Node {
    Ref ref;
    Shares shares;
    Index prev;
    Index next;  // also links the free list
    Index level;
  };

  struct Level {
    Price price;
    Index head;
    Index tail;
    std::uint32_t count;
    Locate locate;
    Side side;
    std::uint64_t shares;
  };

  // Keys grow toward the best price: the price for bids, its complement for
  // asks.
  struct Ladder {
    std::vector<std::uint32_t> keys;
    std::vector<Index> levels;
  };
  struct Book {
    Ladder bids;
    Ladder asks;
  };

  static std::uint32_t key(Side s, Price p) noexcept {
    return s == Side::BUY ? p : ~p;
  }
  Ladder& ladder(Locate loc, Side s) noexcept {
    return s == Side::BUY ? books_[loc].bids : books_[loc].asks;
  }
  const Ladder& ladder(Locate loc, Side s) const noexcept {
    return s == Side::BUY ? books_[loc].bids : books_[loc].asks;
  }

  Index open_level(Locate loc, Side s, Price p);
  void close_level(Index lvl) noexcept;
  Index new_node();
  void unlink(Index n) noexcept;
  // Takes shares off an order and removes it at zero.
  Status reduce(Locate loc, Ref ref, Shares shares) noexcept;
  Status fault(Status s) noexcept;

  std::vector<Book> books_;  // by locate
  std::vector<Node> nodes_;
  std::vector<Level> levels_;
  Index free_node_ = kNil;
  std::vector<Index> free_levels_;
  RefMap refs_;
  std::size_t max_orders_;
  std::size_t max_levels_;
  std::size_t live_levels_ = 0;
  std::size_t peak_orders_ = 0;
  Faults faults_;
};

template <typename F>
void FeedBook::for_each(Locate loc, Side s, F&& f) const {
  const Ladder& l = ladder(loc, s);
  for (std::size_t i = l.levels.size(); i-- > 0;) {
    const Level& lvl = levels_[l.levels[i]];
    for (Index n = lvl.head; n != kNil; n = nodes_[n].next)
      f(lvl.price, nodes_[n].ref, nodes_[n].shares);
  }
}

}  // namespace msim
