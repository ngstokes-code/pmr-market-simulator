#include "msim/flat_hash.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "msim/rng.hpp"

namespace msim {
struct FlatHashMapTestAccess {
  template<class K,class V> static std::size_t bucket(const FlatHashMap<K,V>& m,K k) {
    return m.home(k);
  }
  template<class K,class V> static void omit_shift(FlatHashMap<K,V>& m,K k) {
    const auto at=m.find_index(k); m.table_[at].used=false; --m.size_;
  }
};
}

namespace {

// Random insert/erase churn against std::unordered_map, with the table held
// near its 50% load limit so clusters are long and wrap around the end of
// the table. The probing invariant is checked after every operation, which
// is what catches a wrong backward shift.
void test_churn_matches_unordered_map() {
  std::pmr::monotonic_buffer_resource arena;
  constexpr std::size_t kMax = 64;
  msim::FlatHashMap<std::int64_t, std::uint64_t> map(&arena, kMax);
  std::unordered_map<std::int64_t, std::uint64_t> ref;

  Xoroshiro128Plus rng(7);
  std::size_t mismatches = 0;
  std::size_t broken = 0;

  for (std::uint64_t step = 0; step < 200000; ++step) {
    const auto key = static_cast<std::int64_t>(rand_index(rng, 100)) - 50;

    if (rand_bool(rng, 0.55) && !map.full()) {
      const bool inserted = map.insert(key, step);
      mismatches += inserted != ref.emplace(key, step).second;
    } else {
      mismatches += map.erase(key) != (ref.erase(key) == 1);
    }

    const auto* v = map.find_ptr(key);
    const auto it = ref.find(key);
    mismatches += (v != nullptr) != (it != ref.end());
    if (v && it != ref.end()) mismatches += *v != it->second;
    mismatches += map.size() != ref.size();
    broken += !map.check_invariants();
  }

  CHECK(mismatches == 0);
  CHECK(broken == 0);
  CHECK(map.capacity() == 128);  // never grew
}

// Fill to the limit, then erase in a shuffled order. Every erase must leave
// all remaining keys reachable.
void test_drain_in_random_order() {
  std::pmr::monotonic_buffer_resource arena;
  msim::FlatHashMap<std::uint64_t, std::uint64_t> map(&arena, 500);
  std::vector<std::uint64_t> keys;
  for (std::uint64_t k = 0; k < 500; ++k) {
    keys.push_back(k * 7919);
    CHECK(map.insert(keys.back(), k));
  }
  CHECK(map.full());

  Xoroshiro128Plus rng(11);
  for (std::size_t i = keys.size(); i > 1; --i)
    std::swap(keys[i - 1], keys[rand_index(rng, i)]);

  std::size_t lost = 0;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    CHECK(map.erase(keys[i]));
    for (std::size_t j = i + 1; j < keys.size(); ++j)
      lost += !map.contains(keys[j]);
  }
  CHECK(lost == 0);
  CHECK(map.size() == 0);
  CHECK(map.check_invariants());
}

void test_full_and_duplicate() {
  std::pmr::monotonic_buffer_resource arena;
  msim::FlatHashMap<std::uint64_t, int> map(&arena, 3);
  CHECK(map.insert(1, 10));
  CHECK(!map.insert(1, 11));  // duplicate keeps the first value
  CHECK(*map.find_ptr(1) == 10);
  CHECK(map.insert(2, 20));
  CHECK(map.insert(3, 30));
  CHECK(map.full());
  CHECK(!map.insert(4, 40));
  CHECK(!map.insert(1, 99));
  CHECK(map.size() == 3 && *map.find_ptr(1) == 10);
  CHECK(map.erase(2));
  CHECK(!map.full());
  CHECK(!map.contains(2));
  CHECK(!map.erase(2));
}

}  // namespace

int main() {
  {
    msim::FlatHashMap<std::uint64_t,int> mutant(std::pmr::get_default_resource(),4);
    std::array<std::vector<std::uint64_t>,8> buckets;
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k=0;keys.empty();++k) {
      auto& b=buckets[msim::FlatHashMapTestAccess::bucket(mutant,k)];
      b.push_back(k); if (b.size()==3) keys=b;
    }
    for (auto k:keys) CHECK(mutant.insert(k,1));
    msim::FlatHashMapTestAccess::omit_shift(mutant,keys[0]);
    CHECK(!mutant.check_invariants());
    CHECK(!mutant.contains(keys[1]) && !mutant.contains(keys[2]));
  }
  bool overflow_rejected = false;
  try {
    msim::FlatHashMap<std::uint64_t, int> impossible(
        std::pmr::get_default_resource(), SIZE_MAX / 2 + 1);
  } catch (const std::length_error&) { overflow_rejected = true; }
  CHECK(overflow_rejected);
  test_churn_matches_unordered_map();
  test_drain_in_random_order();
  test_full_and_duplicate();
  return check::finish("flat_hash");
}
