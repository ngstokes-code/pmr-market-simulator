#pragma once
#include <array>
#include <cstddef>
#include <stdexcept>

namespace msim {
// Coordinator-side ownership check. The barriers establish completion;
// this guard checks that the producer retires a lease before reusing bytes.
class BufferEpoch {
 public:
  void reading(std::size_t buffer) {
    if (active_.at(buffer)) throw std::logic_error("buffer still leased");
    active_[buffer] = true;
  }
  void retired(std::size_t buffer) { active_.at(buffer) = false; }
  void writing(std::size_t buffer) const {
    if (active_.at(buffer))
      throw std::logic_error("premature replay buffer reuse");
  }

 private:
  std::array<bool, 2> active_{};
};
}  // namespace msim
