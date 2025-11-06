
#pragma once
#include <memory_resource>
#include <cstddef>
#include <atomic>

namespace msim {

class CountingResource : public std::pmr::memory_resource {
public:
    explicit CountingResource(std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
    : upstream_(upstream) {}

    size_t bytes_allocated() const noexcept { return allocated_.load(std::memory_order_relaxed); }

protected:
    void* do_allocate(size_t bytes, size_t align) override {
        allocated_.fetch_add(bytes, std::memory_order_relaxed);
        return upstream_->allocate(bytes, align);
    }
    void do_deallocate(void* p, size_t bytes, size_t align) override {
        upstream_->deallocate(p, bytes, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
private:
    std::pmr::memory_resource* upstream_;
    std::atomic<size_t> allocated_{0};
};

} // namespace msim
