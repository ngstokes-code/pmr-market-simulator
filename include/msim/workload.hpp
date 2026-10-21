#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "msim/book_types.hpp"
#include "msim/command.hpp"

namespace msim {

// A named, seeded, synthetic command stream. These are stress workloads for
// the matching engine, not models of any real market: each one is shaped to
// answer one question about cost (see docs/performance.md).
struct Workload {
  std::string name;
  std::uint64_t seed = 0;
  BookLimits limits;
  std::vector<Command> commands;
  // Per-command tag for workloads with phases (bursty: 0 quiet, 1 burst).
  // Empty when the workload has a single phase.
  std::vector<std::uint8_t> phase;
};

const std::vector<std::string>& workload_names();

// Generates `n` commands of the named workload. Returns nullopt for an
// unknown name. The same (name, seed, n) always gives the same stream.
std::optional<Workload> generate_workload(const std::string& name,
                                          std::uint64_t seed, std::size_t n);

// What a stream does to a book, measured by replaying it through the engine.
// Tests use it to hold each workload to its description, and the bench
// prints it beside every timing.
struct WorkloadShape {
  std::size_t commands = 0;
  std::size_t adds = 0;
  std::size_t cancels = 0;
  std::size_t cancel_hits = 0;
  std::size_t rejected = 0;  // adds with any status other than Accepted
  std::size_t executions = 0;
  std::size_t takers = 0;  // adds that executed at least once
  std::size_t max_levels_crossed = 0;
  std::size_t multi_level_takers = 0;  // takers that crossed 5+ levels
  double mean_resting_orders = 0;
  double mean_levels_per_side = 0;
  std::size_t max_levels_per_side = 0;
  std::size_t best_price_changes = 0;
  std::size_t level_creations = 0;
  std::size_t level_removals = 0;
};
WorkloadShape measure_shape(const Workload& w);

// Order-sensitive digest of a stream's commands and phase tags.
std::uint64_t stream_digest(const Workload& w);

// Binary stream file: a fixed header, fixed-size little-endian records, and
// a trailing digest. Byte order is explicit, so a file reads the same on any
// platform.
//
//   header   "MSIMCMD1" | u32 version (1) | u32 record bytes (32)
//            u64 seed | u64 count | u64 limits (orders, levels, max_tick)
//            u16 name length | name bytes | u8 flags (bit 0: phase tags)
//   record   u64 id | i64 tick | i64 qty | u8 type | u8 side | u8 phase | 5 x 0
//   trailer  u64 stream_digest
bool write_stream(std::ostream& os, const Workload& w);
std::optional<Workload> read_stream(std::istream& is,
                                    std::string* error = nullptr);

}  // namespace msim
