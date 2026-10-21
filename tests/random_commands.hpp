#pragma once

// Seeded random command sequences for differential and metamorphic tests.
// Each profile leans on a different part of the contract. Nothing here needs
// a book: cancels pick from ids issued so far, so some target orders that
// already executed or were never accepted, which is the point.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "msim/book_types.hpp"
#include "msim/command.hpp"
#include "msim/rng.hpp"

namespace testgen {

struct Profile {
  const char* name;
  msim::BookLimits limits;
  msim::Tick lo, hi;  // tick range for valid adds
  msim::Qty max_qty;  // ordinary add size is 1..max_qty
  double p_add;       // otherwise cancel
  double p_big;       // add that is 5..20 times max_qty, to sweep levels
  double p_dup;       // add that reuses an issued id
  double p_invalid;   // add with a bad tick or qty
  double p_unknown;   // cancel of an id never issued
  double p_huge;      // add near the top of the Qty range
};

// clang-format off
inline const Profile kProfiles[] = {
  //  name           limits                  lo      hi     qty  add   big   dup   bad   unknown huge
  {"narrow",       {12000, 1600, 1000000},   95,     105,   20,  0.60, 0.05, 0.00, 0.00, 0.00, 0.00},
  {"wide",         {12000, 1600, 1000000},   1,      3000,  100, 0.55, 0.02, 0.00, 0.00, 0.00, 0.00},
  {"tiny_limits",  {8, 3, 200},              90,     110,   10,  0.65, 0.10, 0.00, 0.00, 0.02, 0.00},
  {"cancel_heavy", {12000, 1600, 1000000},   480,    520,   30,  0.40, 0.03, 0.00, 0.00, 0.05, 0.00},
  {"hostile",      {64, 16, 1000},           1,      1000,  50,  0.60, 0.05, 0.15, 0.08, 0.10, 0.00},
  {"sweeps",       {500, 40, 100000},        49980,  50020, 15,  0.55, 0.20, 0.00, 0.00, 0.00, 0.00},
  {"huge_qty",     {16, 4, 200},             95,     105,   20,  0.65, 0.05, 0.00, 0.00, 0.02, 0.30},
};
// clang-format on

inline std::vector<msim::Command> random_commands(const Profile& p,
                                                  std::uint64_t seed,
                                                  std::size_t n) {
  Xoroshiro128Plus rng(seed * 0x9E3779B97F4A7C15ULL + 1);
  std::vector<msim::Command> out;
  std::vector<msim::OrderId> issued;
  msim::OrderId next_id = 1;
  out.reserve(n);

  const auto pick = [&](const auto& v) { return v[rand_index(rng, v.size())]; };

  for (std::uint64_t seq = 1; seq <= n; ++seq) {
    if (rand_bool(rng, p.p_add) || issued.empty()) {
      const msim::OrderId id = (!issued.empty() && rand_bool(rng, p.p_dup))
                                   ? pick(issued)
                                   : next_id++;
      const msim::Side side =
          rand_bool(rng, 0.5) ? msim::Side::BUY : msim::Side::SELL;
      msim::Tick tick =
          p.lo + static_cast<msim::Tick>(rand_index(
                     rng, static_cast<std::size_t>(p.hi - p.lo + 1)));
      msim::Qty qty = rand_int(rng, 1, static_cast<int>(p.max_qty));
      if (rand_bool(rng, p.p_big)) qty *= rand_int(rng, 5, 20);
      // Two such orders resting together sum past the Qty range. Profiles
      // without huge adds skip the draw, so their sequences stay as they were.
      if (p.p_huge > 0 && rand_bool(rng, p.p_huge))
        qty = std::numeric_limits<msim::Qty>::max() - qty;

      if (rand_bool(rng, p.p_invalid)) {
        const msim::Tick bad_ticks[] = {0, -1, p.limits.max_tick + 1,
                                        p.limits.max_tick};  // last is valid
        const msim::Qty bad_qtys[] = {0, -1, qty};
        tick = bad_ticks[rand_index(rng, 4)];
        qty = bad_qtys[rand_index(rng, 3)];
      }
      issued.push_back(id);
      out.push_back(msim::Command::add(seq, id, side, tick, qty));
    } else {
      // Recent ids are the ones most likely still resting, so most cancels
      // aim there. The rest reach back anywhere, to hit executed orders.
      msim::OrderId id = 0;
      if (rand_bool(rng, p.p_unknown))
        id = next_id + 1'000'000;
      else if (rand_bool(rng, 0.7))
        id = issued[issued.size() - 1 -
                    rand_index(rng, std::min<std::size_t>(issued.size(), 24))];
      else
        id = pick(issued);
      out.push_back(msim::Command::cancel(seq, id));
    }
  }
  return out;
}

}  // namespace testgen
