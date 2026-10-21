// Engine vs reference.
//
// 1. Directed cases, each with hand-written expected executions. Agreement
//    between two books proves nothing if both misread the contract, so these
//    pin the contract itself.
// 2. Seeded random sequences over several profiles, compared command by
//    command. A mismatch is written out as a trace, shrunk, and printed.
// 3. A self-test: a deliberately broken book must be caught and shrunk.
//
// Usage: differential_test [--seeds A:B] [--commands N] [--trace-dir DIR]

#include "msim/differential.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory_resource>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"
#include "random_commands.hpp"

using msim::AddStatus;
using msim::Command;
using msim::Execution;
using msim::Outcome;
using msim::Side;

namespace {

Outcome added(msim::Qty filled, msim::Qty rested) {
  Outcome o;
  o.add = {AddStatus::Accepted, filled, rested};
  return o;
}
Outcome refused(AddStatus s) {
  Outcome o;
  o.add.status = s;
  return o;
}
Outcome canceled(bool ok) {
  Outcome o;
  o.type = msim::CommandType::Cancel;
  o.canceled = ok;
  return o;
}

Command add(msim::OrderId id, Side side, msim::Tick tick, msim::Qty qty) {
  return Command::add(0, id, side, tick, qty);
}
Command cxl(msim::OrderId id) { return Command::cancel(0, id); }

constexpr Side B = Side::BUY;
constexpr Side S = Side::SELL;
constexpr msim::Qty kHalf = std::numeric_limits<msim::Qty>::max() / 2 + 1;

struct Step {
  Command cmd;
  Outcome want;
  std::vector<Execution> execs;
};

struct Case {
  const char* name;
  msim::BookLimits limits;
  std::vector<Step> steps;
};

// clang-format off
const std::vector<Case> kCases = {
  {"fifo at one level", {}, {
    {add(1, S, 100, 5), added(0, 5), {}},
    {add(2, S, 100, 5), added(0, 5), {}},
    {add(3, B, 100, 6), added(6, 0), {{1, 3, 100, 5}, {2, 3, 100, 1}}},
  }},
  {"better price beats earlier time", {}, {
    {add(1, S, 102, 1), added(0, 1), {}},
    {add(2, S, 101, 1), added(0, 1), {}},
    {add(3, B, 102, 1), added(1, 0), {{2, 3, 101, 1}}},
  }},
  {"buy sweeps three levels", {}, {
    {add(1, S, 101, 2), added(0, 2), {}},
    {add(2, S, 102, 3), added(0, 3), {}},
    {add(3, S, 103, 4), added(0, 4), {}},
    {add(9, B, 103, 8), added(8, 0), {{1, 9, 101, 2}, {2, 9, 102, 3}, {3, 9, 103, 3}}},
  }},
  {"sell sweeps three levels", {}, {
    {add(1, B, 99, 2), added(0, 2), {}},
    {add(2, B, 98, 3), added(0, 3), {}},
    {add(3, B, 97, 4), added(0, 4), {}},
    {add(9, S, 97, 8), added(8, 0), {{1, 9, 99, 2}, {2, 9, 98, 3}, {3, 9, 97, 3}}},
  }},
  {"remainder rests at its limit and executes at its own price later", {}, {
    {add(1, S, 101, 2), added(0, 2), {}},
    {add(9, B, 105, 5), added(2, 3), {{1, 9, 101, 2}}},
    {add(10, S, 104, 1), added(1, 0), {{9, 10, 105, 1}}},
  }},
  {"cancel middle, tail, head", {}, {
    {add(1, B, 50, 1), added(0, 1), {}},
    {add(2, B, 50, 1), added(0, 1), {}},
    {add(3, B, 50, 1), added(0, 1), {}},
    {cxl(2), canceled(true), {}},
    {cxl(3), canceled(true), {}},
    {cxl(1), canceled(true), {}},
    {cxl(1), canceled(false), {}},
    {add(9, S, 50, 1), added(0, 1), {}},
  }},
  {"best bid recomputed after cancel at best", {}, {
    {add(1, B, 99, 1), added(0, 1), {}},
    {add(2, B, 97, 1), added(0, 1), {}},
    {add(3, B, 98, 1), added(0, 1), {}},
    {cxl(1), canceled(true), {}},
    {add(9, S, 97, 2), added(2, 0), {{3, 9, 98, 1}, {2, 9, 97, 1}}},
  }},
  {"book empties and refills", {}, {
    {add(1, S, 100, 1), added(0, 1), {}},
    {add(2, B, 100, 1), added(1, 0), {{1, 2, 100, 1}}},
    {add(3, B, 100, 1), added(0, 1), {}},
    {add(4, S, 101, 1), added(0, 1), {}},
    {cxl(3), canceled(true), {}},
    {cxl(4), canceled(true), {}},
    {add(5, B, 101, 1), added(0, 1), {}},
  }},
  {"rejections change nothing", {100, 100, 1000}, {
    {add(1, S, 100, 5), added(0, 5), {}},
    {add(2, B, 100, 0), refused(AddStatus::InvalidQty), {}},
    {add(2, B, 100, -1), refused(AddStatus::InvalidQty), {}},
    {add(2, B, 0, 1), refused(AddStatus::InvalidTick), {}},
    {add(2, B, 1001, 1), refused(AddStatus::InvalidTick), {}},
    {add(1, B, 100, 5), refused(AddStatus::DuplicateId), {}},
    {add(2, B, 1000, 5), added(5, 0), {{1, 2, 100, 5}}},
    {add(1, B, 90, 1), added(0, 1), {}},  // id 1 is free again
  }},
  {"level limit refuses the whole order", {10, 1, 1000}, {
    {add(1, S, 100, 5), added(0, 5), {}},
    {add(2, B, 99, 5), added(0, 5), {}},
    {add(3, B, 100, 8), refused(AddStatus::CapacityFull), {}},
    {add(3, B, 100, 5), added(5, 0), {{1, 3, 100, 5}}},
  }},
  {"order limit counts slots that matching frees", {2, 10, 1000}, {
    {add(1, S, 100, 5), added(0, 5), {}},
    {add(2, B, 90, 1), added(0, 1), {}},
    {add(3, B, 95, 1), refused(AddStatus::CapacityFull), {}},
    {add(4, B, 100, 8), added(5, 3), {{1, 4, 100, 5}}},
    {add(5, S, 100, 1), added(1, 0), {{4, 5, 100, 1}}},
  }},
  {"resting quantities that sum past the Qty range", {2, 4, 1000}, {
    {add(1, S, 10, kHalf), added(0, kHalf), {}},
    {add(2, S, 10, kHalf), added(0, kHalf), {}},
    {add(3, B, 10, 1), added(1, 0), {{1, 3, 10, 1}}},
  }},
};
// clang-format on

template <typename Book>
void run_case(Book& book, const Case& c, const char* book_name) {
  std::vector<Execution> out;
  for (std::size_t i = 0; i < c.steps.size(); ++i) {
    const Step& s = c.steps[i];
    const Outcome got = msim::apply(book, s.cmd, out);
    const bool ok = got == s.want && out == s.execs;
    if (!CHECK(ok))
      std::fprintf(stderr,
                   "  %s, %s, step %zu (%s): got %s with %zu executions\n",
                   book_name, c.name, i, msim::to_string(s.cmd).c_str(),
                   msim::to_string(got).c_str(), out.size());
  }
}

void test_directed_cases() {
  for (const Case& c : kCases) {
    std::pmr::unsynchronized_pool_resource mr;
    msim::OrderBook engine("X", &mr, c.limits);
    msim::ReferenceBook reference(c.limits);
    run_case(engine, c, "engine");
    run_case(reference, c, "reference");
    std::string why;
    CHECK(engine.check_invariants(&why));
    CHECK(engine.snapshot() == reference.snapshot());
    std::vector<Command> commands;
    for (const auto& step : c.steps) commands.push_back(step.cmd);
    CHECK(!msim::engine_vs_reference(commands, c.limits));
  }
}

struct Options {
  std::uint64_t seed_lo = 0, seed_hi = 40;
  std::size_t commands = 2000;
  // A failure writes its traces here; the temp directory by default, so a
  // failing run does not litter the working tree.
  std::string trace_dir = std::filesystem::temp_directory_path().string();
};

void save(const std::string& path, const std::vector<Command>& cmds,
          const msim::BookLimits& limits) {
  std::ofstream f(path);
  msim::write_trace(f, cmds, &limits);
}

void report_failure(const testgen::Profile& p, std::uint64_t seed,
                    std::vector<Command> cmds, const msim::Divergence& d,
                    const Options& opt) {
  const std::string base =
      opt.trace_dir + "/diff-" + p.name + "-seed" + std::to_string(seed);
  renumber(cmds);
  save(base + ".trace", cmds, p.limits);

  const auto fails = [&](const std::vector<Command>& c) {
    return msim::engine_vs_reference(c, p.limits).has_value();
  };
  const auto small = msim::shrink(cmds, fails);
  save(base + ".min.trace", small, p.limits);

  std::fprintf(stderr,
               "MISMATCH profile=%s seed=%llu at command %zu: %s\n"
               "  full trace:  %s.trace (%zu commands)\n"
               "  shrunk to %zu commands (%s.min.trace):\n",
               p.name, static_cast<unsigned long long>(seed), d.index,
               d.what.c_str(), base.c_str(), cmds.size(), small.size(),
               base.c_str());
  for (const Command& c : small)
    std::fprintf(stderr, "    %s\n", msim::to_string(c).c_str());
  if (auto again = msim::engine_vs_reference(small, p.limits))
    std::fprintf(stderr, "  shrunk failure: %s\n", again->what.c_str());
}

// How often each outcome occurred, so a passing run also shows that it
// reached the cases it claims to test.
struct Coverage {
  std::size_t status[5] = {};
  std::size_t cancel_hit = 0, cancel_miss = 0, sweeps = 0, max_sweep = 0;
  std::size_t freed_slot = 0;  // rested although the index was full before

  void count(const std::vector<Command>& cmds, const msim::BookLimits& lim) {
    std::pmr::unsynchronized_pool_resource mr;
    msim::OrderBook book("X", &mr, lim);
    std::vector<Execution> out;
    for (const Command& c : cmds) {
      const bool full_before = book.resting_orders() == lim.max_resting_orders;
      const Outcome o = msim::apply(book, c, out);
      if (c.type == msim::CommandType::Cancel) {
        ++(o.canceled ? cancel_hit : cancel_miss);
        continue;
      }
      ++status[static_cast<int>(o.add.status)];
      if (full_before && o.add.rested > 0) ++freed_slot;
      std::size_t levels = 0;
      for (std::size_t i = 0; i < out.size(); ++i)
        levels += i == 0 || out[i].tick != out[i - 1].tick;
      sweeps += levels > 1;
      max_sweep = std::max(max_sweep, levels);
    }
  }
};

void test_random_sequences(const Options& opt) {
  std::size_t runs = 0, commands = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const testgen::Profile& p : testgen::kProfiles) {
    Coverage cov;
    for (std::uint64_t seed = opt.seed_lo; seed < opt.seed_hi; ++seed) {
      const auto cmds = testgen::random_commands(p, seed, opt.commands);
      const auto d = msim::engine_vs_reference(cmds, p.limits);
      ++runs;
      commands += cmds.size();
      if (!CHECK(!d)) report_failure(p, seed, cmds, *d, opt);
      cov.count(cmds, p.limits);
    }
    std::printf(
        "  %-12s accepted=%zu invalid=%zu duplicate=%zu capacity=%zu "
        "freed-slot=%zu cancel hit/miss=%zu/%zu sweeps=%zu (max %zu "
        "levels)\n",
        p.name, cov.status[0], cov.status[1] + cov.status[2], cov.status[3],
        cov.status[4], cov.freed_slot, cov.cancel_hit, cov.cancel_miss,
        cov.sweeps, cov.max_sweep);
  }
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  std::printf("random: %zu sequences, %zu commands, %.2f s\n", runs, commands,
              s);
}

// A book that is wrong in one narrow way: it reports a sweep's executions in
// reverse order. The harness must notice, and the shrinker must reduce the
// evidence to the few commands that show it.
class ReversedSweeps {
 public:
  explicit ReversedSweeps(std::pmr::memory_resource* mr) : book_("X", mr) {}
  msim::AddResult add(const msim::Order& o, std::vector<Execution>& out) {
    const msim::AddResult r = book_.add(o, out);
    std::reverse(out.begin(), out.end());
    return r;
  }
  bool cancel(msim::OrderId id) { return book_.cancel(id); }
  msim::BookSnapshot snapshot() const { return book_.snapshot(); }

 private:
  msim::OrderBook book_;
};

void test_harness_catches_a_planted_defect() {
  const auto cmds = testgen::random_commands(testgen::kProfiles[0], 1, 2000);
  const auto fails = [](const std::vector<Command>& c) {
    std::pmr::unsynchronized_pool_resource mr;
    ReversedSweeps broken(&mr);
    msim::ReferenceBook reference;
    return msim::first_divergence(broken, reference, c).has_value();
  };
  REQUIRE(fails(cmds));
  const auto small = msim::shrink(cmds, fails);
  CHECK(fails(small));
  // The least evidence is two resting orders and one incoming order that
  // executes against both. Greedy shrinking guarantees only that no single
  // command can be dropped, so allow a little slack above that.
  CHECK(small.size() >= 3 && small.size() <= 5);
}

void test_trace_round_trip() {
  const auto cmds = testgen::random_commands(testgen::kProfiles[4], 3, 500);
  std::stringstream ss;
  msim::write_trace(ss, cmds);
  const auto back = msim::read_trace(ss);
  REQUIRE(back.has_value());
  CHECK(*back == cmds);

  const msim::BookLimits lim{8, 3, 200};
  std::stringstream with_limits;
  msim::write_trace(with_limits, cmds, &lim);
  msim::BookLimits got;
  REQUIRE(msim::read_trace(with_limits, nullptr, &got).has_value());
  CHECK(got.max_resting_orders == 8 && got.max_levels_per_side == 3 &&
        got.max_tick == 200);

  for (const char* bad : {"1 add 5 X 100 1", "1 add 5 B 100", "1 frob 5",
                          "1 cancel 5 extra", "x add 1 B 1 1"}) {
    std::stringstream in(bad);
    std::string err;
    CHECK(!msim::read_trace(in, &err).has_value());
    CHECK(!err.empty());
  }
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i], v = argv[i + 1];
    if (a == "--seeds") {
      const auto colon = v.find(':');
      o.seed_lo = std::stoull(v.substr(0, colon));
      o.seed_hi = std::stoull(v.substr(colon + 1));
    } else if (a == "--commands") {
      o.commands = std::stoul(v);
    } else if (a == "--trace-dir") {
      o.trace_dir = v;
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      std::exit(2);
    }
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  test_directed_cases();
  test_trace_round_trip();
  test_harness_catches_a_planted_defect();
  test_random_sequences(opt);
  return check::finish("differential");
}
