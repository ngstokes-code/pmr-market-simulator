#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__APPLE__) && !defined(MSIM_PORTABLE_SHA256)
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <limits>
#endif

namespace msim {
// SHA-256 content identity, FIPS 180-4. This is not an authentication scheme.
// Retain the portable path for non-Apple hosts and matched provider checks.
class PortableSha256 {
 public:
  static constexpr const char* provider() noexcept { return "portable"; }
  void update(const unsigned char* data, std::size_t n) noexcept {
    bytes_ += n;
    while (n) {
      const auto take = (64 - used_ < n) ? 64 - used_ : n;
      for (std::size_t i = 0; i < take; ++i) block_[used_ + i] = data[i];
      used_ += take; data += take; n -= take;
      if (used_ == 64) { compress(); used_ = 0; }
    }
  }
  std::string hex() const {
    PortableSha256 copy = *this;
    const std::uint64_t bits = bytes_ * 8;
    const unsigned char marker = 128, zero = 0;
    copy.update(&marker, 1);
    while (copy.used_ != 56) copy.update(&zero, 1);
    std::array<unsigned char, 8> size{};
    for (unsigned i = 0; i < 8; ++i)
      size[i] = static_cast<unsigned char>(bits >> (56 - 8*i));
    copy.update(size.data(), size.size());
    std::string out; out.reserve(64);
    static constexpr char digits[] = "0123456789abcdef";
    for (auto word : copy.state_)
      for (unsigned shift = 32; shift; shift -= 4)
        out.push_back(digits[(word >> (shift - 4)) & 15]);
    return out;
  }
 private:
  static std::uint32_t rotate(std::uint32_t x, unsigned r) noexcept {
    return (x >> r) | (x << (32 - r));
  }
  void compress() noexcept {
    static constexpr std::array<std::uint32_t, 64> k{{
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
      w[i] = std::uint32_t{block_[4*i]} << 24 |
             std::uint32_t{block_[4*i+1]} << 16 |
             std::uint32_t{block_[4*i+2]} << 8 | block_[4*i+3];
    for (std::size_t i = 16; i < 64; ++i) {
      const auto x = w[i-15], y = w[i-2];
      w[i] = w[i-16] + (rotate(x,7)^rotate(x,18)^(x>>3)) + w[i-7] +
             (rotate(y,17)^rotate(y,19)^(y>>10));
    }
    auto a=state_[0], b=state_[1], c=state_[2], d=state_[3];
    auto e=state_[4], f=state_[5], g=state_[6], h=state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
      const auto t1 = h + (rotate(e,6)^rotate(e,11)^rotate(e,25)) +
                      ((e&f)^(~e&g)) + k[i] + w[i];
      const auto t2 = (rotate(a,2)^rotate(a,13)^rotate(a,22)) +
                      ((a&b)^(a&c)^(b&c));
      h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
    state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
  }
  std::array<std::uint32_t,8> state_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
  std::array<unsigned char,64> block_{};
  std::size_t used_ = 0;
  std::uint64_t bytes_ = 0;
};

#if defined(__APPLE__) && !defined(MSIM_PORTABLE_SHA256)
class Sha256 {
 public:
  // CommonCrypto's streaming functions always return success; their context
  // is caller-owned value storage, with no separate handle to share or free.
  Sha256() noexcept { (void)CC_SHA256_Init(&context_); }
  static constexpr const char* provider() noexcept { return "commoncrypto"; }
  void update(const unsigned char* data, std::size_t n) noexcept {
    while (n) {
      // CC_LONG is narrower than size_t on 64-bit hosts. A cast of the full
      // request would silently omit bytes once an update exceeds UINT32_MAX.
      const auto take = std::min(n, static_cast<std::size_t>(
          std::numeric_limits<CC_LONG>::max()));
      (void)CC_SHA256_Update(&context_, data, static_cast<CC_LONG>(take));
      data += take; n -= take;
    }
  }
  std::string hex() const {
    // Final erases its context. Finalize a copy so snapshots neither consume
    // the prefix nor prevent subsequent updates or independent value copies.
    auto copy = context_;
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    (void)CC_SHA256_Final(digest.data(), &copy);
    static constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(2 * digest.size());
    for (auto byte : digest) {
      out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]);
    }
    return out;
  }
 private:
  CC_SHA256_CTX context_{};
};
#else
using Sha256 = PortableSha256;
#endif
}  // namespace msim
