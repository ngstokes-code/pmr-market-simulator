#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

#include "msim/flat_hash.hpp"

namespace msim {

// Recent references use a direct ring; older and far references use a hash.
// A far hash entry may later lie inside the window. Ring misses therefore
// still consult the hash. Neither duplicates nor capacity failures may move
// the window or discard a live identity.
class RefMap {
 public:
  using Ref = std::uint64_t;
  using Value = std::uint32_t;
  enum class Mode { Window, HashOnly };
  struct Stats {
    std::uint64_t ring_hits=0,hash_hits=0,hash_misses=0,scanned_slots=0,migrations=0;
    std::size_t peak_hash_entries=0;
  };
  static constexpr Value kEmpty = UINT32_MAX;
  static constexpr Ref kWindow = Ref{1} << 24;
  static constexpr Ref kMaxJump = Ref{1} << 32;

  explicit RefMap(std::size_t max_old = 4'000'000,
                  Mode mode = Mode::Window, unsigned window_bits = 24, Stats* stats = nullptr)
      : window_(window_size(mode, window_bits)),
        ring_(static_cast<std::size_t>(window_), kEmpty),
        old_(std::pmr::get_default_resource(), max_old), stats_(stats) {}

  std::size_t size() const noexcept { return size_; }
  std::size_t old_size() const noexcept { return old_.size(); }
  std::uint64_t jumps() const noexcept { return jumps_; }
  // Kept for existing diagnostics: rejected movement never loses an entry.
  std::uint64_t lost() const noexcept { return 0; }

  Value find(Ref ref) const noexcept {
    if (inside(ref)) {
      const Value v = ring_[ref & (window_ - 1)];
      if (v != kEmpty) { if (stats_) ++stats_->ring_hits; return v; }
    }
    const Value* v = old_.find_ptr(ref);
    if (stats_) { if (v) ++stats_->hash_hits; else ++stats_->hash_misses; }
    return v ? *v : kEmpty;
  }
  bool contains(Ref ref) const noexcept { return find(ref) != kEmpty; }

  bool insert(Ref ref, Value v) {
    if (v == kEmpty) throw std::invalid_argument("reserved reference value");
    if (contains(ref)) return false;
    if (!window_) {
      if (!old_.insert(ref, v)) return false;
    } else {
      const bool ahead = ref >= base_ && ref - base_ >= window_;
      const bool far = ahead && ref - base_ - window_ >= kMaxJump;
      if (ahead && !far && !slide(ref)) return false;
      if (inside(ref)) {
        ring_[ref & (window_ - 1)] = v;
      } else {
        if (!old_.insert(ref, v)) return false;
        jumps_ += far;
      }
    }
    ++size_;
    if (stats_) stats_->peak_hash_entries=std::max(stats_->peak_hash_entries,old_.size());
    return true;
  }

  std::optional<Value> take(Ref ref) noexcept {
    if (inside(ref)) {
      Value& v = ring_[ref & (window_ - 1)];
      if (v != kEmpty) {
        if (stats_) ++stats_->ring_hits;
        const auto result = v;
        v = kEmpty;
        --size_;
        return result;
      }
    }
    const auto result = old_.take(ref);
    if (stats_) { if (result) ++stats_->hash_hits; else ++stats_->hash_misses; }
    if (result) --size_;
    return result;
  }

 private:
  static Ref window_size(Mode mode, unsigned bits) {
    if (bits < 3 || bits > 24)
      throw std::invalid_argument("reference window bits must be 3 to 24");
    return mode == Mode::HashOnly ? 0 : Ref{1} << bits;
  }
  bool inside(Ref ref) const noexcept {
    return window_ && ref >= base_ && ref - base_ < window_;
  }
  bool slide(Ref ref) {
    const Ref step = window_ / 8;
    const Ref want = (ref - window_) / step * step + step;
    const Ref count = std::min(want - base_, window_);
    if (stats_) stats_->scanned_slots+=count;
    std::size_t moving = 0;
    for (Ref i = 0; i < count; ++i)
      moving += ring_[(base_ + i) & (window_ - 1)] != kEmpty;
    if (moving > old_.max_size() - old_.size()) return false;
    if (stats_) { stats_->scanned_slots+=count;stats_->migrations+=moving; }
    for (Ref i = 0; i < count; ++i) {
      const Ref r = base_ + i;
      Value& v = ring_[r & (window_ - 1)];
      if (v == kEmpty) continue;
      if (!old_.insert(r, v))
        throw std::logic_error("reference index ownership invariant");
      v = kEmpty;
    }
    if (stats_) stats_->peak_hash_entries=std::max(stats_->peak_hash_entries,old_.size());
    base_ = want;
    return true;
  }

  Ref window_;
  std::vector<Value> ring_;
  FlatHashMap<Ref, Value> old_;
  Ref base_ = 0;
  std::size_t size_ = 0;
  std::uint64_t jumps_ = 0;
  Stats* stats_ = nullptr;
};

}  // namespace msim
