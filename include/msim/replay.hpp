#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "msim/feed_book.hpp"
#include "msim/protocol_state.hpp"

namespace msim {
struct ReplayHooks {
  // Failure-campaign hooks. Normal replay and timing leave this null.
  std::function<void(std::size_t)> before_launch;
  std::function<void(std::size_t, std::uint64_t)> before_worker_event;
};
struct ReplayOptions {
  std::size_t workers = 1, chunk_messages = 100'000;
  std::size_t global_orders = 4'000'000, worker_orders = 0;
  std::size_t max_levels = 1'000'000, output_records = 4'000'000;
  std::size_t cross_records = 1'000'000;
  std::size_t day_reference_blocks = 65536;
  RefMap::Mode reference_mode = RefMap::Mode::Window;
  unsigned reference_window_bits = 24;
  bool capture_state = false, capture_crosses = false, reference = false;
  bool instrument_stages = false;
  // Only the mechanism benchmark disables global validation. Its output
  // explicitly carries experimental scope; the public replay CLI cannot.
  bool experimental_without_identity = false;
  const ReplayHooks* hooks = nullptr;
};
using ReplayRow = std::tuple<std::uint16_t, Side, std::uint32_t,
                             std::uint64_t, std::uint32_t>;
struct ReplaySymbol { std::uint16_t locate; ProtocolState::Symbol state; };
struct ReplayCross {
  std::uint64_t ordinal, timestamp, match, shares;
  std::uint16_t locate;
  std::array<char,8> stock;
  std::uint32_t price;
  char listing, type;
};
struct WorkerProfile {
  std::uint64_t events = 0, lifecycle_events = 0;
  double busy_s = 0, wait_s = 0, cpu_s = 0;
  double decode_s = 0, metadata_s = 0, mutation_s = 0;
  std::size_t peak_orders = 0, old_orders = 0;
};
struct ReplayResult {
  std::uint64_t bytes = 0, messages = 0, clock_regressions = 0;
  std::string input_sha256;
  std::array<std::uint64_t,256> type_counts{};
  char terminal_event = '?';
  std::size_t live_orders = 0, levels = 0, buffer_bytes = 0;
  std::size_t day_reference_blocks = 0;
  std::uint64_t reference_births = 0;
  double total_s = 0, read_hash_s = 0, coordinator_s = 0, coordinator_cpu_s = 0;
  double setup_s = 0, pipeline_s = 0, final_validation_s = 0;
  double coordinator_pipeline_cpu_s = 0;
  bool experimental_without_identity = false, instrumented = false;
  std::vector<WorkerProfile> workers;
  std::vector<ReplayRow> state;
  std::vector<std::uint64_t> day_references;
  std::vector<ReplaySymbol> symbols;
  std::vector<ReplayCross> crosses;
};
struct ReplayError : std::runtime_error {
  std::uint64_t ordinal, offset, validated_messages, applied_messages;
  ReplayError(const std::string& reason, std::uint64_t n, std::uint64_t at,
              std::uint64_t validated, std::uint64_t applied)
      : std::runtime_error(reason), ordinal(n), offset(at),
        validated_messages(validated), applied_messages(applied) {}
};
// Same coordinator/worker pipeline for every supported worker count.
// A successful return requires complete supported session bytes, unchanged
// input, global identity agreement and valid worker-owned concrete state.
// Exceptions publish nothing; no partially populated result is returned.
ReplayResult replay(const std::string& path, const ReplayOptions& options = {});
}  // namespace msim
