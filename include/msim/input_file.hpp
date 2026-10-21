#pragma once
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "msim/sha256.hpp"

namespace msim {
// A regular input opened once. Reads never extend beyond its initial size.
// Final descriptor AND pathname metadata must agree with the initial file.
// Concurrent mutation is unsupported and detected changes invalidate replay.
// This is not protection against a malicious kernel or undetectable storage faults.
class InputFile {
 public:
  explicit InputFile(std::string path) : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open input");
    if (::fstat(fd_, &initial_) || !S_ISREG(initial_.st_mode) || initial_.st_size < 0 ||
        static_cast<std::uint64_t>(initial_.st_size) > UINT64_MAX / 8) {
      ::close(fd_); fd_ = -1;
      throw std::runtime_error("input is not a readable regular file");
    }
  }
  ~InputFile() { if (fd_ >= 0) ::close(fd_); }
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;
  std::uint64_t size() const noexcept { return static_cast<std::uint64_t>(initial_.st_size); }
  std::uint64_t consumed() const noexcept { return consumed_; }
  std::string sha256() const { return digest_.hex(); }
  std::size_t read(unsigned char* data, std::size_t capacity) {
    const auto remaining = size() - consumed_;
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, capacity));
    if (!want) return 0;
    ::ssize_t got;
    do { got = ::pread(fd_, data, want, static_cast<off_t>(consumed_)); }
    while (got < 0 && errno == EINTR);
    if (got <= 0) throw std::runtime_error("input shortened or read failed");
    const auto n = static_cast<std::size_t>(got);
    digest_.update(data, n); consumed_ += n;
    return n;
  }
  void validate_unchanged() const {
    struct stat descriptor{}, pathname{};
    if (::fstat(fd_, &descriptor) || ::stat(path_.c_str(), &pathname) ||
        !same(initial_, descriptor) || !same(initial_, pathname))
      throw std::runtime_error("input changed during replay");
  }
  void finish() const {
    if (consumed_ != size()) throw std::runtime_error("input was not fully consumed");
    validate_unchanged();
  }
  void reject_alias(const std::string& destination) const {
    struct stat out{};
    if (!::stat(destination.c_str(), &out) && out.st_dev == initial_.st_dev &&
        out.st_ino == initial_.st_ino)
      throw std::runtime_error("output aliases input");
  }
 private:
  static bool same(const struct stat& a, const struct stat& b) noexcept {
#if defined(__APPLE__)
    const auto am=a.st_mtimespec, bm=b.st_mtimespec;
    const auto ac=a.st_ctimespec, bc=b.st_ctimespec;
#else
    const auto am=a.st_mtim, bm=b.st_mtim;
    const auto ac=a.st_ctim, bc=b.st_ctim;
#endif
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
           am.tv_sec==bm.tv_sec && am.tv_nsec==bm.tv_nsec &&
           ac.tv_sec==bc.tv_sec && ac.tv_nsec==bc.tv_nsec;
  }
  std::string path_;
  int fd_ = -1;
  struct stat initial_{};
  std::uint64_t consumed_ = 0;
  Sha256 digest_;
};
}  // namespace msim
