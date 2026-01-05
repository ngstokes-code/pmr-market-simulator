#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

namespace msim {

// Small, deterministic, fixed-capacity flat hash map for integral keys.
// - Open addressing, linear probing
// - Tombstones supported
// - Compacts tombstones by rehashing at same capacity when needed
// - No growth by default (good with monotonic arenas)
template <typename K, typename V>
class FlatHashMap {
  static_assert(std::is_integral_v<K>, "FlatHashMap requires integral keys");

 public:
  FlatHashMap(std::pmr::memory_resource* mr,
              std::size_t capacity_pow2,
              bool allow_grow = false)
      : mr_(mr),
        allow_grow_(allow_grow),
        mask_(0),
        size_(0),
        tombs_(0),
        table_(mr),
        scratch_(mr) {
    reserve_pow2(capacity_pow2);
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t tombs() const noexcept { return tombs_; }
  std::size_t capacity() const noexcept { return mask_ + 1; }

  V* find_ptr(K key) noexcept {
    const std::size_t idx = find_index(key);
    if (idx == npos) return nullptr;
    return &table_[idx].value;
  }
  const V* find_ptr(K key) const noexcept {
    const std::size_t idx = find_index(key);
    if (idx == npos) return nullptr;
    return &table_[idx].value;
  }

  bool contains(K key) const noexcept { return find_index(key) != npos; }

  bool insert(K key, const V& value) { return emplace_impl(key, value); }
  bool insert(K key, V&& value) { return emplace_impl(key, std::move(value)); }

  V* find_or_insert(K key, const V& value) {
    return find_or_insert_impl(key, value);
  }
  V* find_or_insert(K key, V&& value) {
    return find_or_insert_impl(key, std::move(value));
  }

  bool erase(K key) noexcept {
    const std::size_t idx = find_index(key);
    if (idx == npos) return false;
    table_[idx].state = State::Tomb;
    --size_;
    ++tombs_;
    return true;
  }

 private:
  enum class State : uint8_t { Empty = 0, Filled = 1, Tomb = 2 };

  struct Entry {
    K key{};
    V value{};
    State state{State::Empty};
  };

  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

  std::pmr::memory_resource* mr_;
  bool allow_grow_;
  std::size_t mask_;
  std::size_t size_;
  std::size_t tombs_;
  std::pmr::vector<Entry> table_;
  std::pmr::vector<Entry> scratch_;

  static std::size_t next_pow2(std::size_t x) {
    if (x < 8) return 8;
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    if constexpr (sizeof(std::size_t) == 8) x |= x >> 32;
    return x + 1;
  }

  void reserve_pow2(std::size_t cap_pow2) {
    const std::size_t cap = next_pow2(cap_pow2);
    table_.clear();
    table_.resize(cap);
    scratch_.clear();
    scratch_.resize(cap);
    mask_ = cap - 1;
    size_ = 0;
    tombs_ = 0;

    // Ensure both tables start Empty
    for (auto& e : table_) e.state = State::Empty;
    for (auto& e : scratch_) e.state = State::Empty;
  }

  static std::size_t hash_key(K key) noexcept {
    if constexpr (sizeof(K) == 8) {
      uint64_t x = static_cast<uint64_t>(key);
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return static_cast<std::size_t>(x);
    } else {
      uint32_t x = static_cast<uint32_t>(key);
      x ^= x >> 16;
      x *= 0x7feb352dU;
      x ^= x >> 15;
      x *= 0x846ca68bU;
      x ^= x >> 16;
      return static_cast<std::size_t>(x);
    }
  }

  std::size_t find_index(K key) const noexcept {
    std::size_t idx = hash_key(key) & mask_;
    for (;;) {
      const Entry& e = table_[idx];
      if (e.state == State::Empty) return npos;
      if (e.state == State::Filled && e.key == key) return idx;
      idx = (idx + 1) & mask_;
    }
  }

  static void die_capacity(std::size_t size, std::size_t tombs, std::size_t cap) {
    std::fprintf(stderr,
                 "FlatHashMap capacity exceeded (fixed-size).\n"
                 "  size=%zu tombs=%zu cap=%zu (threshold=80%%)\n"
                 "  Suggestion: increase capacity_pow2 or enable growth.\n",
                 size, tombs, cap);
    std::fflush(stderr);
    std::abort();
  }

  void maybe_compact_for_tombs() {
    // If tombstones are high, rehash at the same capacity to restore performance.
    // Deterministic: triggers purely based on counts.
    const std::size_t cap = mask_ + 1;
    if (tombs_ == 0) return;

    // Trigger compaction when tombstones are "meaningfully" large.
    // This prevents the exact failure you're seeing (size small, tombs huge).
    if (tombs_ > cap / 4 || (size_ + tombs_) * 10 >= cap * 7) {
      rehash_same_capacity();
    }
  }

  void rehash_same_capacity() {
    const std::size_t cap = mask_ + 1;

    // reset scratch to all Empty
    for (auto& e : scratch_) e.state = State::Empty;

    std::size_t new_size = 0;

    for (auto& e : table_) {
      if (e.state != State::Filled) continue;

      // Insert into scratch with no capacity checks (same cap, fewer tombs)
      std::size_t idx = hash_key(e.key) & mask_;
      for (;;) {
        Entry& dst = scratch_[idx];
        if (dst.state == State::Empty) {
          dst.key = e.key;
          dst.value = std::move(e.value);
          dst.state = State::Filled;
          ++new_size;
          break;
        }
        idx = (idx + 1) & mask_;
      }
    }

    table_.swap(scratch_);
    size_ = new_size;
    tombs_ = 0;

    // If we somehow still violate bounds, better to fail loudly.
    if (!allow_grow_) {
      if ((size_ + tombs_) * 10 >= cap * 8) die_capacity(size_, tombs_, cap);
    }
  }

  template <typename VV>
  bool emplace_impl(K key, VV&& value) {
    if (!allow_grow_) {
      maybe_compact_for_tombs();
      const std::size_t cap = mask_ + 1;
      if ((size_ + tombs_) * 10 >= cap * 8) die_capacity(size_, tombs_, cap);
    }

    std::size_t idx = hash_key(key) & mask_;
    std::size_t first_tomb = npos;

    for (;;) {
      Entry& e = table_[idx];
      if (e.state == State::Empty) {
        const std::size_t ins = (first_tomb != npos) ? first_tomb : idx;
        Entry& dst = table_[ins];
        dst.key = key;
        dst.value = std::forward<VV>(value);
        dst.state = State::Filled;
        ++size_;
        if (first_tomb != npos) --tombs_;
        return true;
      }
      if (e.state == State::Tomb) {
        if (first_tomb == npos) first_tomb = idx;
      } else if (e.key == key) {
        return false;
      }
      idx = (idx + 1) & mask_;
    }
  }

  template <typename VV>
  V* find_or_insert_impl(K key, VV&& value) {
    if (!allow_grow_) {
      maybe_compact_for_tombs();
      const std::size_t cap = mask_ + 1;
      if ((size_ + tombs_) * 10 >= cap * 8) die_capacity(size_, tombs_, cap);
    }

    std::size_t idx = hash_key(key) & mask_;
    std::size_t first_tomb = npos;

    for (;;) {
      Entry& e = table_[idx];
      if (e.state == State::Empty) {
        const std::size_t ins = (first_tomb != npos) ? first_tomb : idx;
        Entry& dst = table_[ins];
        dst.key = key;
        dst.value = std::forward<VV>(value);
        dst.state = State::Filled;
        ++size_;
        if (first_tomb != npos) --tombs_;
        return &dst.value;
      }
      if (e.state == State::Tomb) {
        if (first_tomb == npos) first_tomb = idx;
      } else if (e.key == key) {
        return &e.value;
      }
      idx = (idx + 1) & mask_;
    }
  }
};

}  // namespace msim
