#pragma once
#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace msim {

class CountingResource : public std::pmr::memory_resource {
 public:
  explicit CountingResource(
      std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
      : upstream_(upstream) {}

  size_t bytes_allocated() const noexcept {
    return allocated_.load(std::memory_order_relaxed);
  }

 protected:
  void* do_allocate(size_t bytes, size_t align) override {
    allocated_.fetch_add(bytes, std::memory_order_relaxed);
    return upstream_->allocate(bytes, align);
  }
  void do_deallocate(void* p, size_t bytes, size_t align) override {
    upstream_->deallocate(p, bytes, align);
  }
  bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

 private:
  std::pmr::memory_resource* upstream_;
  std::atomic<size_t> allocated_{0};
};

// Single-threaded tally of what passes through to `upstream`: bytes live
// now, the peak of that, the cumulative total, and the allocation count.
// Stacked above and below an arena, two of these show the difference between
// what containers ask for and what the arena actually keeps.
class TallyResource : public std::pmr::memory_resource {
 public:
  explicit TallyResource(
      std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
      : upstream_(upstream) {}

  size_t live() const noexcept { return live_; }
  size_t peak() const noexcept { return peak_; }
  size_t total() const noexcept { return total_; }
  size_t allocations() const noexcept { return allocations_; }

 protected:
  void* do_allocate(size_t bytes, size_t align) override {
    void* p = upstream_->allocate(bytes, align);
    live_ += bytes;
    total_ += bytes;
    ++allocations_;
    if (live_ > peak_) peak_ = live_;
    return p;
  }
  void do_deallocate(void* p, size_t bytes, size_t align) override {
    live_ -= bytes;
    upstream_->deallocate(p, bytes, align);
  }
  bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

 private:
  std::pmr::memory_resource* upstream_;
  size_t live_ = 0, peak_ = 0, total_ = 0, allocations_ = 0;
};

}  // namespace msim
