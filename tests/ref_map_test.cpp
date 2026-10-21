// RefMap against std::unordered_map, on reference streams shaped like a
// feed's: mostly increasing with local disorder, plus jumps past the window,
// references below it, and orders that outlive it.

#include "msim/ref_map.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "msim/rng.hpp"
#include "msim/day_references.hpp"

using msim::RefMap;

namespace {

void test_window_and_hash_boundary() {
  RefMap m;
  const RefMap::Ref w = RefMap::kWindow;
  CHECK(m.insert(5, 50));
  CHECK(!m.insert(5, 51));            // already present
  CHECK(m.insert(w + 10, 60));        // slides the window past 5
  CHECK(m.old_size() == 1 && m.find(5) == 50u);
  CHECK(m.insert(3, 30));             // below the window: goes to the hash
  CHECK(m.find(3) == 30u && m.old_size() == 2);
  CHECK(m.take(5) == 50u && !m.take(5));
  CHECK(m.find(w + 10) == 60u && m.size() == 2);
  CHECK(m.insert(10 * w, 70));        // jumps more than a window ahead
  CHECK(m.find(w + 10) == 60u && m.find(10 * w) == 70u);
  CHECK(m.take(3) == 30u && m.take(w + 10) == 60u && m.take(10 * w) == 70u);
  CHECK(m.size() == 0 && m.old_size() == 0);
}

void test_far_jump_does_not_move_the_window() {
  RefMap m;
  CHECK(m.insert(100, 1));
  const RefMap::Ref far = RefMap::kWindow + RefMap::kMaxJump + 5;
  CHECK(m.insert(far, 2));  // a corrupt or outlying reference
  CHECK(m.jumps() == 1 && m.old_size() == 1 && m.find(far) == 2u);
  CHECK(m.insert(101, 3));  // ordinary references still use the window
  CHECK(m.old_size() == 1 && m.find(100) == 1u && m.find(101) == 3u);
  CHECK(m.take(far) == 2u && m.old_size() == 0);
}

void test_full_hash_rejects_movement_atomically() {
  RefMap m(1);
  const RefMap::Ref w = RefMap::kWindow;
  CHECK(m.insert(1, 1) && m.insert(2, 2));
  CHECK(!m.insert(w + 5, 3));  // both would leave; preserve both instead
  CHECK(m.lost() == 0 && m.size() == 2);
  CHECK(m.find(1) == 1 && m.find(2) == 2);
  CHECK(!m.contains(w + 5));
  CHECK(m.take(2) == 2);
  CHECK(m.insert(w + 5, 3));
  CHECK(m.find(1) == 1 && m.find(w + 5) == 3);
  CHECK(!m.insert(0, 9));
}

void test_window_catches_up_with_far_hash_entry() {
  RefMap m(8);
  const auto far = RefMap::kWindow + RefMap::kMaxJump + 5;
  CHECK(m.insert(far, 11));
  CHECK(m.insert(far - 6, 22));
  CHECK(m.insert(far + 1, 33));
  CHECK(m.find(far) == 11);
  CHECK(!m.insert(far, 44));
  CHECK(m.size() == 3 && m.lost() == 0);
  CHECK(m.take(far) == 11 && !m.take(far));
  CHECK(m.find(far - 6) == 22 && m.find(far + 1) == 33);
  RefMap edge(8, RefMap::Mode::Window, 3);
  CHECK(edge.insert(UINT64_MAX, 1));
  CHECK(edge.insert(UINT64_MAX - RefMap::kMaxJump, 2));
  CHECK(edge.insert(UINT64_MAX - 1, 3));
  CHECK(edge.find(UINT64_MAX) == 1);
  CHECK(edge.insert(0, 4) && edge.find(0) == 4);
}

void test_random_streams() {
  for (std::uint64_t seed = 0; seed < 8; ++seed) {
    Xoroshiro128Plus rng(seed + 11);
    RefMap m(4'000'000, RefMap::Mode::Window, 16);
    std::unordered_map<RefMap::Ref, RefMap::Value> want;
    std::vector<RefMap::Ref> live;
    RefMap::Ref next = 1;
    for (std::uint32_t i = 0; i < 400'000; ++i) {
      const double u = rng.next_uniform01();
      if (u < 0.45 || live.empty()) {
        RefMap::Ref r = next + rand_index(rng, 64);  // local disorder
        next += 1 + rand_index(rng, 3);
        if (rand_bool(rng, 0.0005)) next += RefMap::kWindow / 2;  // jump
        if (rand_bool(rng, 0.01) && r > 100) r -= rand_index(rng, 100);
        const bool fresh = !want.count(r);
        if (!CHECK(m.insert(r, i) == fresh)) return;
        if (fresh) {
          want[r] = i;
          live.push_back(r);
        }
      } else {
        // Mostly recent orders, sometimes old ones, sometimes absent ones.
        const std::size_t recent = std::min<std::size_t>(live.size(), 50);
        const std::size_t k = rand_bool(rng, 0.9)
                                  ? live.size() - 1 - rand_index(rng, recent)
                                  : rand_index(rng, live.size());
        const RefMap::Ref r = rand_bool(rng, 0.05) ? next + 1000 : live[k];
        const auto it = want.find(r);
        const auto got = m.take(r);
        if (!CHECK(got.has_value() == (it != want.end()) &&
                   (!got || *got == it->second))) {
          std::fprintf(stderr, "  seed %llu step %u ref %llu\n",
                       static_cast<unsigned long long>(seed), i,
                       static_cast<unsigned long long>(r));
          return;
        }
        if (it != want.end()) {
          want.erase(it);
          live[k] = live.back();
          live.pop_back();
        }
      }
      if (i % 4096 == 0)
        for (std::size_t j = 0; j < live.size(); j += 97)
          if (!CHECK(m.find(live[j]) == want[live[j]])) return;
    }
    CHECK(m.size() == want.size() && m.lost() == 0);
  }
}

}  // namespace

int main() {
  {
    msim::RefMap::Stats stats;
    RefMap measured(16,RefMap::Mode::Window,3,&stats);
    CHECK(measured.insert(1,10) && measured.find(1)==10);
    CHECK(measured.insert(20,20) && measured.find(1)==10);
    CHECK(measured.take(1)==10 && measured.take(20)==20);
    CHECK(stats.ring_hits>=2 && stats.hash_hits>=2 && stats.hash_misses==2);
    CHECK(stats.migrations==1 && stats.scanned_slots==16 && stats.peak_hash_entries==1);
  }
  {
    msim::DayReferences day(3);
    for (auto ref : std::vector<std::uint64_t>{0,1,63,64,65535,65536,UINT64_MAX})
      day.insert(ref);
    CHECK(day.births()==7 && day.blocks()==3 && day.contains(UINT64_MAX));
    bool duplicate=false, exhausted=false;
    try { day.insert(0); } catch (const std::runtime_error&) { duplicate=true; }
    try { day.insert(131072); } catch (const std::runtime_error&) { exhausted=true; }
    CHECK(duplicate && exhausted && day.births()==7 && !day.contains(131072));
    msim::DayReferences stream(16);
    std::set<std::uint64_t> oracle;
    Xoroshiro128Plus rng(99);
    for (unsigned i=0;i<10000;++i) {
      const auto ref=rng.next_u64()&((std::uint64_t{1}<<20)-1);
      const bool fresh=oracle.insert(ref).second;
      bool inserted=true;
      try { stream.insert(ref); } catch (const std::runtime_error&) { inserted=false; }
      CHECK(inserted==fresh && stream.contains(ref));
    }
    CHECK(stream.births()==oracle.size());
  }
  test_window_and_hash_boundary();
  test_far_jump_does_not_move_the_window();
  test_full_hash_rejects_movement_atomically();
  test_window_catches_up_with_far_hash_entry();
  test_random_streams();
  return check::finish("ref_map");
}
