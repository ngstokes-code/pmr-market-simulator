#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>

namespace msim {
// ITCH A/F/U births use day-unique references, including retired orders.
// Sparse 64K-reference blocks bound memory without assuming dense 64-bit IDs.
class DayReferences {
 public:
  explicit DayReferences(std::size_t maximum_blocks=65536) : maximum_(maximum_blocks) {}
  bool contains(std::uint64_t ref) const noexcept {
    const auto found=blocks_.find(ref>>16);
    return found!=blocks_.end() &&
        ((*found->second)[(ref&65535)>>6] & (std::uint64_t{1}<<(ref&63)));
  }
  void validate_new(std::uint64_t ref) const {
    const auto found=blocks_.find(ref>>16);
    if (found==blocks_.end() && blocks_.size()==maximum_)
      throw std::runtime_error("day-reference capacity exhausted");
    if (found!=blocks_.end() &&
        ((*found->second)[(ref&65535)>>6] & (std::uint64_t{1}<<(ref&63))))
      throw std::runtime_error("duplicate day reference");
  }
  void insert(std::uint64_t ref) {
    validate_new(ref);
    const auto high=ref>>16;
    auto found=blocks_.find(high);
    if (found==blocks_.end())
      found=blocks_.emplace(high,std::make_unique<Block>()).first;
    (*found->second)[(ref&65535)>>6] |= std::uint64_t{1}<<(ref&63);
    ++births_;
  }
  std::size_t blocks() const noexcept { return blocks_.size(); }
  std::uint64_t births() const noexcept { return births_; }
  template<class F> void for_each(F&& visit) const {
    for (const auto& [high,block] : blocks_)
      for (std::size_t word=0;word<block->size();++word) {
        const auto value=(*block)[word];
        if (!value) continue;
        for (unsigned bit=0;bit<64;++bit)
          if (value&(std::uint64_t{1}<<bit))
            visit((high<<16)+(word<<6)+bit);
      }
  }
 private:
  using Block=std::array<std::uint64_t,1024>;
  std::map<std::uint64_t,std::unique_ptr<Block>> blocks_;
  std::size_t maximum_;
  std::uint64_t births_=0;
};
}  // namespace msim
