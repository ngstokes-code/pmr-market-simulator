#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace msim {

// Small, deterministic, fixed-capacity flat hash map for integral keys.
//
// Open addressing with linear probing. The table never grows: it is sized for
// max_size live entries at <= 50% load, so it allocates once and suits a
// monotonic arena.
//
// Linear probing relies on one invariant: every entry sits at its home slot
// or after it, with no empty slot in between. Lookup walks forward from home
// and stops at the first empty slot. Erase keeps the invariant by shifting
// later entries of the same cluster back into the hole (Knuth, TAOCP vol. 3,
// Algorithm R). Deletions leave no persistent tombstones; new key clusters
// can still be long. No periodic tombstone cleanup or rehash is needed.
template <typename K, typename V>
class FlatHashMap {
  friend struct FlatHashMapTestAccess;
  static_assert(std::is_integral_v<K>, "FlatHashMap requires integral keys");
  static_assert(std::is_nothrow_move_assignable_v<V> &&
                std::is_nothrow_move_constructible_v<V>,
                "FlatHashMap values must move without throwing");

 public:
  FlatHashMap(std::pmr::memory_resource* mr, std::size_t max_size)
      : max_size_(max_size), table_(mr) {
    std::size_t cap = 8;
    if (max_size > table_.max_size() / 2)
      throw std::length_error("FlatHashMap capacity is not representable");
    while (cap < 2 * max_size) {
      if (cap > table_.max_size() / 2)
        throw std::length_error("FlatHashMap capacity is not representable");
      cap <<= 1;
    }
    table_.resize(cap);
    mask_ = cap - 1;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t max_size() const noexcept { return max_size_; }
  bool full() const noexcept { return size_ >= max_size_; }
  std::size_t capacity() const noexcept { return mask_ + 1; }

  V* find_ptr(K key) noexcept {
    const std::size_t idx = find_index(key);
    return idx == npos ? nullptr : &table_[idx].value;
  }
  const V* find_ptr(K key) const noexcept {
    const std::size_t idx = find_index(key);
    return idx == npos ? nullptr : &table_[idx].value;
  }

  bool contains(K key) const noexcept { return find_index(key) != npos; }

  // False for a duplicate or exhausted capacity; neither changes the map.
  bool insert(K key, V value) {
    if (full()) return false;
    std::size_t idx = home(key);
    while (table_[idx].used) {
      if (table_[idx].key == key) return false;
      idx = next(idx);
    }
    table_[idx] = Entry{key, std::move(value), true};
    ++size_;
    return true;
  }

  // Cost: O(length of the key's cluster). At <= 50% load the expected
  // cluster is short.
  bool erase(K key) noexcept {
    const std::size_t idx = find_index(key);
    if (idx == npos) return false;
    erase_at(idx);
    return true;
  }

  // Removes the key and returns its value, in one probe.
  std::optional<V> take(K key) noexcept {
    const std::size_t idx = find_index(key);
    if (idx == npos) return std::nullopt;
    std::optional<V> v(std::move(table_[idx].value));
    erase_at(idx);
    return v;
  }

  // O(capacity × cluster length) check of the probing invariant, for tests:
  // every entry is reachable from its home slot without crossing an empty
  // slot, and the live count matches size().
  bool check_invariants() const noexcept {
    std::size_t live = 0;
    for (std::size_t i = 0; i <= mask_; ++i) {
      if (!table_[i].used) continue;
      ++live;
      for (std::size_t j = home(table_[i].key); j != i; j = next(j))
        if (!table_[j].used) return false;
    }
    return live == size_ && live <= max_size_ && live <= capacity() / 2;
  }

 private:
  struct Entry {
    K key{};
    V value{};
    bool used{false};
  };

  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

  std::size_t max_size_;
  std::size_t mask_{0};
  std::size_t size_{0};
  std::pmr::vector<Entry> table_;

  void erase_at(std::size_t hole) noexcept {
    for (std::size_t j = next(hole); table_[j].used; j = next(j)) {
      // The entry at j may fill the hole only if its home is not in the
      // cyclic range (hole, j]. Otherwise moving it back would place it
      // before its home, where lookup would never reach it.
      if (in_cyclic_range(home(table_[j].key), hole, j)) continue;
      table_[hole] = std::move(table_[j]);
      hole = j;
    }
    table_[hole].used = false;
    --size_;
  }

  std::size_t next(std::size_t i) const noexcept { return (i + 1) & mask_; }
  std::size_t home(K key) const noexcept { return hash_key(key) & mask_; }

  // True if x lies in (lo, hi] going forward around the table.
  static bool in_cyclic_range(std::size_t x, std::size_t lo,
                              std::size_t hi) noexcept {
    return lo <= hi ? (lo < x && x <= hi) : (lo < x || x <= hi);
  }

  static std::size_t hash_key(K key) noexcept {
    if constexpr (sizeof(K) == 8) {
      std::uint64_t x = static_cast<std::uint64_t>(key);
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return static_cast<std::size_t>(x);
    } else {
      std::uint32_t x = static_cast<std::uint32_t>(key);
      x ^= x >> 16;
      x *= 0x7feb352dU;
      x ^= x >> 15;
      x *= 0x846ca68bU;
      x ^= x >> 16;
      return static_cast<std::size_t>(x);
    }
  }

  // Terminates because live entries are <= 50% of capacity, so an empty
  // slot always exists.
  std::size_t find_index(K key) const noexcept {
    std::size_t idx = home(key);
    for (std::size_t steps = 0; steps < capacity() && table_[idx].used;
         ++steps, idx = next(idx))
      if (table_[idx].key == key) return idx;
    return npos;
  }
};

}  // namespace msim
