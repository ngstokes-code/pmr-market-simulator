#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "msim/spsc_ring.hpp"

static void test_basic() {
  msim::SpscRing<int, 8> q;
  int out = -1;

  assert(!q.try_pop(out));
  assert(q.try_push(1));
  assert(q.try_push(2));
  assert(q.try_pop(out) && out == 1);
  assert(q.try_pop(out) && out == 2);
  assert(!q.try_pop(out));
}

static void test_full_empty() {
  msim::SpscRing<int, 4> q;
  assert(q.try_push(1));
  assert(q.try_push(2));
  assert(q.try_push(3));
  assert(q.try_push(4));
  assert(q.full());
  assert(!q.try_push(5));

  int out = 0;
  assert(q.try_pop(out) && out == 1);
  assert(q.try_push(5));

  assert(q.try_pop(out) && out == 2);
  assert(q.try_pop(out) && out == 3);
  assert(q.try_pop(out) && out == 4);
  assert(q.try_pop(out) && out == 5);
  assert(q.empty());
}

static void test_threaded_ordering() {
  constexpr std::size_t N = 200000;
  msim::SpscRing<std::uint64_t, 1024> q;

  std::vector<std::uint64_t> got;
  got.reserve(N);

  std::thread prod([&] {
    for (std::uint64_t i = 0; i < N; ++i) {
      while (!q.try_push(i)) {}
    }
  });

  std::thread cons([&] {
    std::uint64_t v = 0;
    while (got.size() < N) {
      if (q.try_pop(v)) got.push_back(v);
    }
  });

  prod.join();
  cons.join();

  assert(got.size() == N);
  for (std::size_t i = 0; i < N; ++i) assert(got[i] == i);
}

int main() {
  test_basic();
  test_full_empty();
  test_threaded_ordering();
  std::cout << "OK: spsc_ring\n";
  return 0;
}
