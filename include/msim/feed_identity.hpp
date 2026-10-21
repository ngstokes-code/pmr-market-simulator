#pragma once

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "msim/feed_book.hpp"

namespace msim {

// File-order validation of the live reference namespace, before partitioning
// by symbol. Only the coordinator owns this state. Price and FIFO stay in the
// worker books. Resource failure is terminal; semantic rejection is atomic.
class FeedIdentity {
 public:
  using Status = FeedBook::Status;
  using Ref = FeedBook::Ref;
  using Locate = FeedBook::Locate;
  using Shares = FeedBook::Shares;
  using Price = FeedBook::Price;
  using Row = std::tuple<Ref, Locate, Shares>;

  explicit FeedIdentity(std::size_t maximum = FeedBook::Options{}.max_orders,
                        RefMap::Mode mode = RefMap::Mode::Window,
                        unsigned window_bits = 24)
      : refs_(validated(maximum), mode, window_bits), maximum_(maximum) {
    slots_.reserve(maximum);
  }

  Status add(Locate locate, Ref ref, Side side, Shares shares, Price) {
    if (side != Side::BUY && side != Side::SELL) return Status::InvalidSide;
    if (!shares) return Status::ZeroShares;
    if (refs_.contains(ref)) return Status::DuplicateRef;
    if (refs_.size() == maximum_) return Status::Capacity;
    const auto index = free_ == nil ? static_cast<Index>(slots_.size()) : free_;
    if (!refs_.insert(ref, index))
      throw std::logic_error("identity index rejected admitted reference");
    if (free_ == nil)
      slots_.push_back({ref, shares, locate, true});
    else {
      free_ = slots_[index].shares;
      slots_[index] = {ref, shares, locate, true};
    }
    return Status::Applied;
  }

  Status execute(Locate locate, Ref ref, Shares shares) {
    const auto index = refs_.find(ref);
    if (index == nil) return Status::UnknownRef;
    auto& entry = slots_[index];
    if (entry.locate != locate) return Status::LocateMismatch;
    if (!shares) return Status::ZeroShares;
    if (shares > entry.shares) return Status::OverExecution;
    if (shares == entry.shares)
      release(ref, index);
    else
      entry.shares -= shares;
    return Status::Applied;
  }
  Status cancel(Locate locate, Ref ref, Shares shares) {
    return execute(locate, ref, shares);
  }
  Status remove(Locate locate, Ref ref) {
    const auto index = refs_.find(ref);
    if (index == nil) return Status::UnknownRef;
    if (slots_[index].locate != locate) return Status::LocateMismatch;
    release(ref, index);
    return Status::Applied;
  }
  Status replace(Locate locate, Ref old_ref, Ref new_ref, Shares shares,
                 Price) {
    const auto index = refs_.find(old_ref);
    if (index == nil) return Status::UnknownRef;
    if (slots_[index].locate != locate) return Status::LocateMismatch;
    if (!shares) return Status::ZeroShares;
    if (refs_.contains(new_ref)) return Status::DuplicateRef;
    (void)refs_.take(old_ref);
    if (!refs_.insert(new_ref, index))
      throw std::logic_error("identity index rejected admitted replacement");
    slots_[index] = {new_ref, shares, locate, true};
    return Status::Applied;
  }

  std::size_t size() const noexcept { return refs_.size(); }
  bool agrees(Ref ref, Locate locate, Shares shares) const noexcept {
    const auto index = refs_.find(ref);
    return index != nil && slots_[index].live && slots_[index].locate == locate &&
           slots_[index].shares == shares;
  }

  // Diagnostic allocation is outside the replay path.
  std::vector<Row> snapshot() const {
    std::vector<Row> rows;
    for (const auto& entry : slots_)
      if (entry.live) rows.emplace_back(entry.ref, entry.locate, entry.shares);
    std::sort(rows.begin(), rows.end());
    return rows;
  }
  bool check_invariants() const {
    if (refs_.lost() || slots_.size() > maximum_) return false;
    std::vector<bool> seen(slots_.size(), false);
    std::size_t live = 0, free = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      const auto& entry = slots_[i];
      if (!entry.live) continue;
      if (!entry.shares || refs_.find(entry.ref) != i) return false;
      seen[i] = true;
      ++live;
    }
    for (auto index = free_; index != nil; index = slots_[index].shares) {
      if (index >= slots_.size() || seen[index] || slots_[index].live)
        return false;
      seen[index] = true;
      ++free;
    }
    return live == refs_.size() && live + free == slots_.size();
  }

 private:
  using Index = RefMap::Value;
  static constexpr Index nil = RefMap::kEmpty;
  struct Entry {
    Ref ref;
    // In a free slot, this field holds the next free index.
    Shares shares;
    Locate locate;
    bool live;
  };
  static std::size_t validated(std::size_t maximum) {
    if (maximum >= nil)
      throw std::invalid_argument("identity capacity exceeds 32-bit indices");
    return maximum;
  }
  void release(Ref ref, Index index) noexcept {
    (void)refs_.take(ref);
    slots_[index].live = false;
    slots_[index].shares = free_;
    free_ = index;
  }
  RefMap refs_;
  std::size_t maximum_;
  std::vector<Entry> slots_;
  Index free_ = nil;
};

}  // namespace msim
