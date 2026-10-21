// Each workload is held to the description in its doc comment, so the names
// in the docs and the benchmark tables mean what they say.

#include "msim/workload.hpp"

#include <cstdio>
#include <sstream>

#include "check.hpp"
#include "msim/differential.hpp"

using msim::Workload;
using msim::WorkloadShape;

namespace {

constexpr std::size_t kN = 200'000;

double pct(std::size_t part, std::size_t whole) {
  return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole)
               : 0.0;
}

WorkloadShape shape_of(const char* name) {
  const auto w = msim::generate_workload(name, 7, kN);
  if (!CHECK(w.has_value())) return {};
  const WorkloadShape s = msim::measure_shape(*w);
  std::printf(
      "  %-18s depth=%6.0f levels/side=%6.1f (max %4zu) cancels=%4.1f%% "
      "hit=%4.1f%% takers=%5zu 5+level=%5zu max_crossed=%2zu best_moves=%zu "
      "rejected=%zu\n",
      name, s.mean_resting_orders, s.mean_levels_per_side,
      s.max_levels_per_side, pct(s.cancels, s.commands),
      pct(s.cancel_hits, s.cancels), s.takers, s.multi_level_takers,
      s.max_levels_crossed, s.best_price_changes, s.rejected);
  return s;
}

void test_shapes_match_their_descriptions() {
  const auto stable = shape_of("stable_depth");
  CHECK(stable.mean_resting_orders > 800 && stable.mean_resting_orders < 1400);
  CHECK(stable.cancel_hits * 10 >
        stable.cancels * 9);  // cancels target live orders

  const auto churn = shape_of("cancel_churn");
  CHECK(churn.cancels * 100 > churn.commands * 40);
  CHECK(churn.mean_levels_per_side > 150);  // sparse: many distinct prices

  const auto sweep = shape_of("multi_level_sweep");
  CHECK(sweep.multi_level_takers > kN / 60);
  CHECK(sweep.max_levels_crossed >= 40);

  const auto wide = shape_of("many_prices");
  CHECK(wide.mean_levels_per_side > 500);
  CHECK(wide.best_price_changes > kN / 4);

  const auto bursty = shape_of("bursty");
  CHECK(bursty.takers > kN / 40);

  for (const auto* s : {&stable, &churn, &sweep, &wide, &bursty}) {
    CHECK(s->rejected == 0);  // streams stay inside the default limits
    // Every level removed was created first, and what is left at the end
    // fits on two sides.
    CHECK(s->level_creations >= s->level_removals);
    CHECK(s->level_creations - s->level_removals <= 2 * s->max_levels_per_side);
  }
}

void test_bursty_phases_are_tagged() {
  const auto w = msim::generate_workload("bursty", 1, 10'000);
  REQUIRE(w.has_value());
  REQUIRE(w->phase.size() == w->commands.size());
  std::size_t burst = 0;
  for (auto p : w->phase) burst += p;
  CHECK(burst > 1500 && burst < 2000);  // 400 of every 2,400
}

void test_generation_is_deterministic() {
  for (const auto& name : msim::workload_names()) {
    const auto a = msim::generate_workload(name, 3, 20'000);
    const auto b = msim::generate_workload(name, 3, 20'000);
    const auto c = msim::generate_workload(name, 4, 20'000);
    REQUIRE(a && b && c);
    CHECK(a->commands.size() == 20'000);
    CHECK(msim::stream_digest(*a) == msim::stream_digest(*b));
    CHECK(msim::stream_digest(*a) != msim::stream_digest(*c));
  }
  CHECK(!msim::generate_workload("no_such_workload", 1, 10));
  CHECK(!msim::generate_workload("stable_depth/width=10", 1, 10));
  CHECK(!msim::generate_workload("many_prices/width=100", 1, 10));
  CHECK(!msim::generate_workload("price_band/width=0", 1, 10));
  CHECK(!msim::generate_workload("price_band/depth=10", 1, 10));
  CHECK(!msim::generate_workload("price_band/width=x", 1, 10));

  // Width sets how many levels each side holds.
  const auto narrow = msim::generate_workload("price_band/width=20", 2, 50'000);
  const auto wide = msim::generate_workload("price_band/width=600", 2, 50'000);
  REQUIRE(narrow && wide);
  CHECK(narrow->name == "price_band/width=20");
  const auto sn = msim::measure_shape(*narrow), sw = msim::measure_shape(*wide);
  CHECK(sn.max_levels_per_side <= 40);
  CHECK(sw.mean_levels_per_side > 10 * sn.mean_levels_per_side);
}

void test_stream_file_round_trip_and_corruption() {
  const auto w = msim::generate_workload("bursty", 9, 5'000);
  REQUIRE(w.has_value());
  std::stringstream ss;
  REQUIRE(msim::write_stream(ss, *w));
  const std::string bytes = ss.str();
  CHECK(bytes.size() == 8 + 4 + 4 + 5 * 8 + 2 + w->name.size() + 1 +
                            w->commands.size() * 32 + 8);

  std::istringstream in(bytes);
  std::string err;
  const auto back = msim::read_stream(in, &err);
  REQUIRE(back.has_value());
  CHECK(back->name == w->name && back->seed == w->seed);
  CHECK(back->commands == w->commands);
  CHECK(back->phase == w->phase);

  // One flipped bit in a record must be caught by the digest.
  std::string bad = bytes;
  bad[bytes.size() / 2] = static_cast<char>(bad[bytes.size() / 2] ^ 0x10);
  std::istringstream corrupt(bad);
  CHECK(!msim::read_stream(corrupt, &err).has_value());

  std::istringstream truncated(bytes.substr(0, bytes.size() - 9));
  CHECK(!msim::read_stream(truncated, &err).has_value());
}

// The generator watches the engine, so its streams must also be ones the
// engine and the reference agree on.
void test_streams_agree_with_reference() {
  for (const auto& name : msim::workload_names()) {
    const auto w = msim::generate_workload(name, 11, 30'000);
    REQUIRE(w.has_value());
    msim::CompareOptions opt;
    opt.snapshot_every = 5'000;
    opt.check_invariants = false;
    const auto d = msim::engine_vs_reference(w->commands, w->limits, opt);
    if (!CHECK(!d))
      std::fprintf(stderr, "  %s: %s\n", name.c_str(), d->what.c_str());
  }
}

}  // namespace

int main() {
  test_shapes_match_their_descriptions();
  test_bursty_phases_are_tagged();
  test_generation_is_deterministic();
  test_stream_file_round_trip_and_corruption();
  test_streams_agree_with_reference();
  return check::finish("workload");
}
