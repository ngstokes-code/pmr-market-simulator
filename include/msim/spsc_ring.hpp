#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace msim {

/**
 * Bounded single-producer / single-consumer ring buffer.
 * - Fixed capacity (power-of-two), no allocation
 * - Head/tail padded to avoid false sharing
 * - Acquire/release for safe publication
 * 
 * Memory ordering invariants  
 * - Producer constructs T then does head.store(..., release) -> publishes the object.
 * - Consumer does head.load(acquire) before reading slot -> guarantees it sees the constructed object.
 * - Consumer destroys object then tail.store(..., release) -> publishes slot-freeing.
 * - Producer does tail.load(acquire) to avoid overwriting an unconsumed slot.
 */
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "Capacity must be >= 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power-of-two");
  static_assert(std::is_move_constructible_v<T>, "T must be move-constructible");

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  ~SpscRing() { clear(); }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool try_push(const T& v) { return emplace_impl(v); }
  bool try_push(T&& v) { return emplace_impl(std::move(v)); }

  bool try_pop(T& out) noexcept(
      std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
    const std::size_t tail = tail_.v.load(std::memory_order_relaxed);
    const std::size_t head = head_.v.load(std::memory_order_acquire);
    if (tail == head) return false;  // empty

    T* ptr = ptr_at(tail);
    out = std::move(*ptr);
    ptr->~T();

    tail_.v.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    const std::size_t tail = tail_.v.load(std::memory_order_acquire);
    const std::size_t head = head_.v.load(std::memory_order_acquire);
    return tail == head;
  }

  bool full() const noexcept {
    const std::size_t head = head_.v.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.v.load(std::memory_order_acquire);
    return (head - tail) == Capacity;
  }

  // Only safe when no other thread is concurrently accessing the ring.
  void clear() noexcept {
    std::size_t tail = tail_.v.load(std::memory_order_relaxed);
    const std::size_t head = head_.v.load(std::memory_order_relaxed);
    while (tail != head) {
      ptr_at(tail)->~T();
      ++tail;
    }
    tail_.v.store(head, std::memory_order_relaxed);
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;
  using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

  struct alignas(64) PaddedAtomic {
    std::atomic<std::size_t> v{0};
  };

  T* ptr_at(std::size_t idx) noexcept {
    return std::launder(reinterpret_cast<T*>(&buf_[idx & kMask]));
  }

  template <typename U>
  bool emplace_impl(U&& v) {
    const std::size_t head = head_.v.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.v.load(std::memory_order_acquire);
    if ((head - tail) == Capacity) return false;  // full

    ::new (static_cast<void*>(ptr_at(head))) T(std::forward<U>(v));
    head_.v.store(head + 1, std::memory_order_release);
    return true;
  }

  PaddedAtomic head_{};
  PaddedAtomic tail_{};
  Storage buf_[Capacity];
};

}  // namespace msim
