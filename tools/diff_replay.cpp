// Replays a trace through the engine and the reference book, one command at
// a time, and prints what each book did. The first command on which they
// disagree is marked, and both books' final state is printed as a ladder.
//
// Usage: diff_replay TRACE [--limits ORDERS,LEVELS,MAX_TICK] [--quiet|--json]
//
// Limits come from --limits, else from the trace's limits line, else the
// defaults.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <string>
#include <vector>

#include "msim/command.hpp"
#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"

namespace {

void print_ladder(const char* name, const msim::BookSnapshot& s) {
  std::printf("%s\n", name);
  for (auto it = s.asks.rbegin(); it != s.asks.rend(); ++it) {
    std::printf("    ask %8lld |", static_cast<long long>(it->tick));
    for (const auto& [id, qty] : it->orders)
      std::printf(" #%llu:%lld", static_cast<unsigned long long>(id),
                  static_cast<long long>(qty));
    std::printf("\n");
  }
  std::printf("    ------------+\n");
  for (const auto& lvl : s.bids) {
    std::printf("    bid %8lld |", static_cast<long long>(lvl.tick));
    for (const auto& [id, qty] : lvl.orders)
      std::printf(" #%llu:%lld", static_cast<unsigned long long>(id),
                  static_cast<long long>(qty));
    std::printf("\n");
  }
}

std::string executions(const std::vector<msim::Execution>& xs) {
  std::ostringstream os;
  for (const auto& x : xs)
    os << " [" << x.resting_id << "@" << x.tick << "x" << x.qty << "]";
  return os.str();
}

void json_levels(std::ostream& out, const std::vector<msim::LevelSnapshot>& levels) {
  out << '[';
  bool first_level = true;
  for (const auto& level : levels) {
    if (!first_level) out << ',';
    first_level = false;
    out << '[' << level.tick << ",[";
    bool first_order = true;
    for (const auto& [id, qty] : level.orders) {
      if (!first_order) out << ',';
      first_order = false;
      out << "[\"" << id << "\"," << qty << ']';
    }
    out << "]]";
  }
  out << ']';
}

int json_replay(const std::vector<msim::Command>& commands, msim::BookLimits limits) {
  // Full state is diagnostic output, not a hot-path export or a benchmark.
  // Bound the checked demonstration before constructing its owned storage.
  if (commands.size() > 2000 || limits.max_resting_orders > 512 ||
      limits.max_levels_per_side > 32)
    throw std::invalid_argument("JSON replay is limited to 2000 commands, 512 orders and 32 levels per side");
  std::pmr::unsynchronized_pool_resource mr;
  msim::OrderBook engine("visualizer", &mr, limits);
  msim::ReferenceBook reference(limits);
  std::vector<msim::Execution> actual, expected;
  std::cout << "{\"schema\":1,\"frames\":[";
  bool first = true;
  for (const auto& command : commands) {
    const auto before = engine.snapshot();
    const auto outcome = msim::apply(engine, command, actual);
    const auto model = msim::apply(reference, command, expected);
    const auto state = engine.snapshot();
    if (outcome != model || actual != expected || !(state == reference.snapshot()) ||
        !engine.check_invariants())
      throw std::runtime_error("JSON replay semantic disagreement at command " + std::to_string(command.seq));
    const bool rejected = command.type == msim::CommandType::Add && outcome.add.status != msim::AddStatus::Accepted;
    if (rejected && (!(state == before) || !actual.empty()))
      throw std::runtime_error("rejected command changed state or executions");
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"seq\":" << command.seq << ",\"id\":\"" << command.id
              << "\",\"kind\":\"" << (command.type == msim::CommandType::Add ? "add" : "cancel")
              << "\",\"side\":\"" << (command.side == msim::Side::BUY ? "B" : "S")
              << "\",\"price\":" << command.tick << ",\"quantity\":" << command.qty
              << ",\"status\":\"" << (command.type == msim::CommandType::Add ? msim::to_string(outcome.add.status) : outcome.canceled ? "removed" : "not_found")
              << "\",\"filled\":" << outcome.add.filled << ",\"rested\":" << outcome.add.rested
              << ",\"executions\":[";
    bool first_execution = true;
    for (const auto& execution : actual) {
      if (!first_execution) std::cout << ',';
      first_execution = false;
      std::cout << "[\"" << execution.resting_id << "\",\"" << execution.incoming_id
                << "\"," << execution.tick << ',' << execution.qty << ']';
    }
    std::cout << "],\"bids\":";
    json_levels(std::cout, state.bids);
    std::cout << ",\"asks\":";
    json_levels(std::cout, state.asks);
    std::cout << '}';
  }
  std::cout << "],\"verified\":true}\n" << std::flush;
  return std::cout ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: diff_replay TRACE [--limits ORDERS,LEVELS,MAX_TICK] "
                 "[--quiet|--json]\n");
    return 2;
  }
  msim::BookLimits limits;
  bool quiet = false, limits_flag = false, json = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--quiet") {
      quiet = true;
    } else if (a == "--json") {
      json = true;
    } else if (a == "--limits" && i + 1 < argc) {
      unsigned long long o = 0, l = 0;
      long long t = 0;
      if (std::sscanf(argv[++i], "%llu,%llu,%lld", &o, &l, &t) != 3) {
        std::fprintf(stderr, "bad --limits\n");
        return 2;
      }
      limits = {o, l, t};
      limits_flag = true;
    }
  }

  std::ifstream in(argv[1]);
  std::string err;
  msim::BookLimits from_trace = limits;
  const auto cmds = msim::read_trace(in, &err, &from_trace);
  if (!limits_flag) limits = from_trace;
  if (!cmds) {
    std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
    return 2;
  }

  if (json) {
    try {
      return json_replay(*cmds, limits);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 2;
    }
  }

  std::pmr::unsynchronized_pool_resource mr;
  msim::OrderBook engine("engine", &mr, limits);
  msim::ReferenceBook reference(limits);
  std::vector<msim::Execution> ea, eb;
  bool diverged = false;

  for (const auto& c : *cmds) {
    const auto oa = msim::apply(engine, c, ea);
    const auto ob = msim::apply(reference, c, eb);
    const bool same = oa == ob && ea == eb;
    if (!quiet || !same) {
      std::printf("%-28s engine: %s%s\n", msim::to_string(c).c_str(),
                  msim::to_string(oa).c_str(), executions(ea).c_str());
      if (!same)
        std::printf("%-28s    ref: %s%s   <-- first divergence\n", "",
                    msim::to_string(ob).c_str(), executions(eb).c_str());
    }
    if (!same) {
      diverged = true;
      break;
    }
  }

  std::printf("\n");
  print_ladder("engine", engine.snapshot());
  print_ladder("reference", reference.snapshot());
  std::printf("\n%s after %zu commands\n", diverged ? "DIVERGED" : "agree",
              cmds->size());
  return diverged ? 1 : 0;
}
