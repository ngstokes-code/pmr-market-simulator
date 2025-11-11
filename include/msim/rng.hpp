// rng.hpp
#pragma once
#include <array>
#include <cstdint>

struct SplitMix64 {
  uint64_t x;
  explicit SplitMix64(uint64_t seed) : x(seed) {}
  uint64_t next() {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

struct Xoroshiro128Plus {
  uint64_t s0, s1;
  explicit Xoroshiro128Plus(uint64_t seed = 1) {
    SplitMix64 sm(seed);
    s0 = sm.next();
    s1 = sm.next();
  }
  static inline uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
  }
  uint64_t next_u64() {
    uint64_t r = s0 + s1;
    s1 ^= s0;
    s0 = rotl(s0, 55) ^ s1 ^ (s1 << 14);
    s1 = rotl(s1, 36);
    return r;
  }
  double next_uniform01() {  // [0,1)
    // 53-bit mantissa -> double in [0,1)
    return (next_u64() >> 11) * (1.0 / 9007199254740992.0);
  }
};

inline bool rand_bool(Xoroshiro128Plus& rng, double p = 0.5) {
  return rng.next_uniform01() < p;
}

inline int rand_int(Xoroshiro128Plus& rng, int min, int max) {
  return static_cast<int>(min + (max - min + 1) * rng.next_uniform01());
}

inline size_t rand_index(Xoroshiro128Plus& rng, size_t max_exclusive) {
  return static_cast<size_t>(rng.next_uniform01() * max_exclusive);
}
