#pragma once
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "msim/itch.hpp"
#include "msim/input_file.hpp"

namespace msim {
// Bounded streaming reader for analysis. It does not retain mapped input pages.
class ItchFile {
 public:
  explicit ItchFile(const std::string& path) : input_(path), buffer_(1 << 20) {}
  const InputFile& input() const noexcept { return input_; }
  void finish() const { input_.finish(); }
  struct Frame {
    const unsigned char* data;
    std::size_t size, offset;
    std::uint64_t ordinal;
  };
  bool next(Frame& frame) {
    if (!ensure(2)) {
      if (have_ != at_)
        throw std::runtime_error("truncated length prefix at " +
                                 std::to_string(offset_));
      input_.finish();
      return false;
    }
    const auto size = itch::detail::be16(buffer_.data() + at_);
    if (size < 11 || !ensure(2 + static_cast<std::size_t>(size)))
      throw std::runtime_error("invalid or truncated frame at " +
                               std::to_string(offset_));
    frame = {buffer_.data() + at_ + 2, size, offset_, ++ordinal_};
    at_ += 2 + size;
    offset_ += 2 + size;
    return true;
  }

 private:
  InputFile input_;
  std::vector<unsigned char> buffer_;
  std::size_t at_ = 0, have_ = 0, offset_ = 0;
  std::uint64_t ordinal_ = 0;
  bool ensure(std::size_t need) {
    if (have_ - at_ >= need) return true;
    const auto remaining = have_ - at_;
    std::memmove(buffer_.data(), buffer_.data() + at_, remaining);
    at_ = 0;
    have_ = remaining;
    while (have_ < need && have_ < buffer_.size()) {
      const auto n = input_.read(buffer_.data() + have_, buffer_.size() - have_);
      if (!n) break;
      have_ += n;
    }
    return have_ >= need;
  }
};
}  // namespace msim
