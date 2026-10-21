#pragma once

#include <cstdint>

namespace msim {

// Order-sensitive 64-bit digest of a sequence of words. Two runs that feed
// the same words in the same order produce the same value. Built for cheap
// equality checks between runs, not for security.
class Digest {
 public:
  void mix(std::uint64_t v) noexcept {
    // The splitmix64 finalizer, applied to the running state.
    std::uint64_t x = h_ ^ v;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    h_ = x;
  }

  std::uint64_t value() const noexcept { return h_; }

 private:
  std::uint64_t h_ = 0x9e3779b97f4a7c15ULL;
};

}  // namespace msim
