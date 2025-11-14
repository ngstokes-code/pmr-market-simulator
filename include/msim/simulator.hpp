#pragma once
#include <chrono>
#include <memory_resource>
#include <random>
#include <unordered_map>
#include <vector>

#include "box_muller.hpp"
#include "grpc_storage.hpp"
#include "order_book.hpp"
#include "pmr_utils.hpp"
#include "rng.hpp"
#include "storage.hpp"

namespace msim {

struct SimConfig {
  uint64_t total_events = 100000;
  uint64_t seed = 42;
  std::vector<std::string> symbol_list;  // if empty, auto-pick N=3
  size_t arena_bytes = (1 << 20);        // per-symbol arena
  double sigma = 0.001;                  // base fractional sigma (0.1% of mid)
  double drift_ampl = 0.0;               // 0.0 = off
  uint64_t drift_period = 10000;
  std::string log_path;
  bool print_arena = false;
  int dump_n = 0;           // number of events to print when reading
  int num_threads = 1;      // default single-thread
  std::string grpc_target;  // "" = disabled
};

class Simulator {
 public:
  explicit Simulator(SimConfig cfg);
  void run();
  void run_mt();  // multithreaded / NUMA-aware version
  void set_grpc_sink(GrpcStorage* sink) { grpc_sink_ = sink; }

 private:
  GrpcStorage* grpc_sink_ = nullptr;
  static void bind_to_core(
      size_t cord_id);  // Pins the calling thread to `core_id` to ensure
                        // NUMA-local allocations.

  SimConfig cfg_;
  std::mt19937_64 rng_;

  struct ArenaBundle {
    std::vector<std::byte> buffer;
    CountingResource counter;
    std::pmr::monotonic_buffer_resource arena;
    explicit ArenaBundle(size_t bytes)
        : buffer(bytes),
          counter(std::pmr::new_delete_resource()),
          arena(buffer.data(), buffer.size(), &counter) {}
  };

  struct SymState {
    std::unique_ptr<ArenaBundle> mem;
    std::unique_ptr<OrderBook> book;
    double mid = 100.0;
  };

  struct ThreadContext {
    std::vector<std::string> symbols;  // symbols assigned to this thread
    std::unique_ptr<ArenaBundle> arena;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, double> mid;  // local midprice per symbol
    std::unordered_map<std::string, std::vector<uint64_t>>
        live;  // live order ids, for realistic cancellation logic

    // local RNG avoids cache contention on a shared generator
    Xoroshiro128Plus rng;
    NormalBM
        normal;  // box-muller transform... uniform dist (0,1) -> normal dist

    uint64_t adds = 0;
    uint64_t cancels = 0;
    uint64_t trades = 0;
    uint64_t total_events = 0;  // total events processed by this thread
    double elapsed_ms = 0.0;    // timing for this thread
  };

  std::unordered_map<std::string, SymState> syms_;
  std::unique_ptr<IStorage> storage_;
  std::uniform_int_distribution<int> qty_dist_{1, 100};
  std::bernoulli_distribution side_dist_{0.5};
  std::bernoulli_distribution add_dist_{0.9};
  uint64_t next_order_id_ = 1;

  uint64_t now_ns() const;
  double draw_price(double mid, uint64_t i_event, ThreadContext& ctx);
  void emit_event(const Event& e);
  static std::vector<std::string> default_symbols();
};

}  // namespace msim
