#pragma once
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "msim/command.hpp"
#include "msim/itch.hpp"

namespace msim {
// Synthetic subset: A, E, D. One engine price unit is 1/10000 dollar here.
// Engine IDs may be reused; every newly resting order gets a fresh feed
// reference.
class SyntheticItch {
 public:
  using Bytes = std::vector<unsigned char>;
  void validate(const Command& c) const {
    if (c.seq > 0xffffffffffffULL)
      throw std::invalid_argument("timestamp cannot encode sequence");
    if (c.type == CommandType::Add &&
        (c.tick < 1 || c.tick > 2'000'000'000LL || c.qty < 1 ||
         static_cast<std::uint64_t>(c.qty) > UINT32_MAX))
      throw std::invalid_argument(
          "synthetic price or quantity outside ITCH encoding");
  }
  itch::Ref reference(OrderId id) const { return live_.at(id).ref; }
  Bytes encode(const Command& c, const Outcome& outcome,
               const std::vector<Execution>& executions) {
    validate(c);
    Bytes bytes;
    if (c.type == CommandType::Cancel) {
      if (outcome.canceled) {
        auto m = message('D', 19, c.seq);
        put(m, 11, reference(c.id), 8);
        append(bytes, m);
        live_.erase(c.id);
      }
      return bytes;
    }
    if (outcome.add.status != AddStatus::Accepted) return bytes;
    Qty total = 0;
    for (const auto& x : executions) {
      if (x.qty <= 0 || x.qty > c.qty - total ||
          x.qty > live_.at(x.resting_id).qty)
        throw std::logic_error("inconsistent synthetic execution");
      total += x.qty;
    }
    if (total != outcome.add.filled || outcome.add.rested != c.qty - total)
      throw std::logic_error("inconsistent synthetic outcome");
    for (const auto& x : executions) {
      auto& resting = live_.at(x.resting_id);
      auto m = message('E', 31, c.seq);
      put(m, 11, resting.ref, 8);
      put(m, 19, static_cast<std::uint64_t>(x.qty), 4);
      if (match_ == UINT64_MAX)
        throw std::overflow_error("synthetic match exhausted");
      put(m, 23, ++match_, 8);
      append(bytes, m);
      resting.qty -= x.qty;
      if (resting.qty == 0) live_.erase(x.resting_id);
    }
    if (outcome.add.rested) {
      if (next_ == UINT64_MAX)
        throw std::overflow_error("synthetic reference exhausted");
      const auto ref = ++next_;
      auto m = message('A', 36, c.seq);
      put(m, 11, ref, 8);
      m[19] = c.side == Side::BUY ? 'B' : 'S';
      put(m, 20, static_cast<std::uint64_t>(outcome.add.rested), 4);
      put(m, 32, static_cast<std::uint64_t>(c.tick), 4);
      const char stock[] = "SYNTH   ";
      for (std::size_t i = 0; i < 8; ++i)
        m[24 + i] = static_cast<unsigned char>(stock[i]);
      append(bytes, m);
      live_.emplace(c.id, Resting{ref, outcome.add.rested});
    }
    return bytes;
  }

 private:
  struct Resting {
    itch::Ref ref;
    Qty qty;
  };
  std::unordered_map<OrderId, Resting> live_;
  itch::Ref next_ = 0;
  std::uint64_t match_ = 0;
  static void put(Bytes& b, std::size_t at, std::uint64_t value,
                  std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      b[at + width - 1 - i] = static_cast<unsigned char>(value & 255);
      value >>= 8;
    }
  }
  static Bytes message(char type, std::size_t length, std::uint64_t seq) {
    Bytes m(length, 0);
    m[0] = static_cast<unsigned char>(type);
    put(m, 1, 1, 2);
    put(m, 5, seq, 6);
    return m;
  }
  static void append(Bytes& to, const Bytes& m) {
    to.push_back(static_cast<unsigned char>(m.size() >> 8));
    to.push_back(static_cast<unsigned char>(m.size() & 255));
    to.insert(to.end(), m.begin(), m.end());
  }
};
}  // namespace msim
