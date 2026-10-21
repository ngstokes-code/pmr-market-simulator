#pragma once

#include <algorithm>
#include <cstddef>

namespace msim::detail {

// First position in the ascending range [keys, keys + n) whose key is >= k.
// Both book sides keep their best price last. A short backward scan favors
// changes near that end; deeper changes fall back to binary search. This
// does not remove the linear movement cost of inserting a deep level.
template <typename Key>
std::size_t lower_bound_from_best(const Key* keys, std::size_t n,
                                  Key k) noexcept {
  constexpr std::size_t kScan = 8;
  std::size_t i = n;
  const std::size_t stop = n > kScan ? n - kScan : 0;
  while (i > stop && keys[i - 1] >= k) --i;
  if (i == stop && i > 0 && keys[i - 1] >= k)
    i = static_cast<std::size_t>(std::lower_bound(keys, keys + i, k) - keys);
  return i;
}

}  // namespace msim::detail
