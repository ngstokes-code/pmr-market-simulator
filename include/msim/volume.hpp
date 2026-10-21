#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace msim::volume {
constexpr std::uint64_t interval_ns = 300ULL * 1'000'000'000;
enum Category : std::size_t { E, CY, P, Q, CN, Categories };
struct Record {
  std::uint64_t match = 0, ordinal = 0, timestamp = 0, ref = 0, shares = 0;
  std::uint16_t locate = 0;
  char kind = 0;
};
using Counts = std::array<std::uint64_t, Categories>;
using Key = std::pair<std::uint16_t, std::uint64_t>;
inline void add(std::uint64_t& to, std::uint64_t value) {
  if (value > UINT64_MAX - to)
    throw std::overflow_error("volume accounting overflow");
  to += value;
}
inline std::size_t category(char kind) {
  switch (kind) {
    case 'E':
      return E;
    case 'Y':
      return CY;
    case 'P':
      return P;
    case 'Q':
      return Q;
    case 'N':
      return CN;
  }
  throw std::logic_error("unknown volume category");
}
struct Result {
  std::map<Key, Counts> cells;
  std::uint64_t records = 0, breaks = 0, corrected_shares = 0,
                companion_groups = 0, corrected_nonprintable_shares = 0;
  void reconcile(std::vector<Record>& records_in) {
    std::sort(
        records_in.begin(), records_in.end(), [](const auto& a, const auto& b) {
          return a.match != b.match ? a.match < b.match : a.ordinal < b.ordinal;
        });
    for (std::size_t begin = 0; begin < records_in.size();) {
      std::size_t end = begin + 1;
      while (end < records_in.size() &&
             records_in[end].match == records_in[begin].match)
        ++end;
      const Record* included = nullptr;
      const Record* broken = nullptr;
      std::set<std::uint64_t> nonprintable_refs;
      bool has_original = false;
      std::uint16_t locate = 0;
      std::uint64_t last_original = 0;
      for (std::size_t i = begin; i < end; ++i) {
        const auto& r = records_in[i];
        add(records, 1);
        if (r.timestamp >= 86400ULL * 1'000'000'000)
          throw std::runtime_error("record timestamp outside day");
        if (r.kind == 'B') {
          if (broken)
            throw std::runtime_error("duplicate break match " +
                                     std::to_string(r.match));
          broken = &r;
          continue;
        }
        category(r.kind);
        if (has_original && locate != r.locate)
          throw std::runtime_error("match crosses symbols");
        has_original = true;
        locate = r.locate;
        last_original = r.ordinal;
        if (r.kind == 'N') {
          if (!nonprintable_refs.insert(r.ref).second)
            throw std::runtime_error("duplicate non-printable execution");
        } else {
          if (included)
            throw std::runtime_error("duplicate volume-bearing match " +
                                     std::to_string(r.match));
          included = &r;
        }
      }
      if (!has_original)
        throw std::runtime_error("unknown break match " +
                                 std::to_string(records_in[begin].match));
      if (broken && broken->locate != locate)
        throw std::runtime_error("break crosses symbols");
      if (included && !nonprintable_refs.empty()) add(companion_groups, 1);
      if (broken) {
        if (broken->ordinal <= last_original)
          throw std::runtime_error("break precedes execution");
        if (included && included->kind == 'Q')
          throw std::runtime_error("unsupported cross break");
        add(breaks, 1);
        if (included) add(corrected_shares, included->shares);
        for (std::size_t i = begin; i < end; ++i)
          if (records_in[i].kind == 'N')
            add(corrected_nonprintable_shares, records_in[i].shares);
      }
      for (std::size_t i = begin; i < end; ++i) {
        const auto& r = records_in[i];
        if (r.kind == 'B') continue;
        auto& cell = cells[{r.locate, r.timestamp / interval_ns}];
        if (!broken) add(cell[category(r.kind)], r.shares);
      }
      begin = end;
    }
  }
  Counts total() const {
    Counts sum{};
    for (const auto& entry : cells)
      for (std::size_t i = 0; i < Categories; ++i) add(sum[i], entry.second[i]);
    return sum;
  }
};
inline bool fraction(const Counts& c, double& out) {
  std::uint64_t denominator = c[E];
  add(denominator, c[CY]);
  add(denominator, c[P]);
  if (!denominator) return false;
  out = static_cast<double>(c[P]) / static_cast<double>(denominator);
  return true;
}
}  // namespace msim::volume
