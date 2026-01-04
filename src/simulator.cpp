#define _USE_MATH_DEFINES
#include "msim/simulator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

#include "msim/rng.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static std::mutex io_mtx;
// ─────────────── Thread-safe logging helper ───────────────
template <typename... Args>
void safe_log(Args&&... args) {
  static std::mutex log_mtx;
  std::lock_guard<std::mutex> lock(log_mtx);
  (std::cout << ... << args) << std::endl;
}

template <typename... Args>
void safe_err(Args&&... args) {
  static std::mutex log_mtx;
  std::lock_guard<std::mutex> lock(log_mtx);
  (std::cerr << ... << args) << std::endl;
}
// ──────────────────────────────────────────────────────────

namespace msim {

std::vector<std::string> Simulator::default_symbols() {
  return {"AAPL", "MSFT", "GOOG"};
}

Simulator::Simulator(SimConfig cfg) : cfg_(std::move(cfg)), rng_(cfg_.seed) {
  auto symbols =
      cfg_.symbol_list.empty() ? default_symbols() : cfg_.symbol_list;
  for (auto& s : symbols) {
    auto mem = std::make_unique<ArenaBundle>(cfg_.arena_bytes);
    auto book = std::make_unique<OrderBook>(s, &mem->arena);
    syms_.emplace(s, SymState{std::move(mem), std::move(book), 100.0});
  }
  if (!cfg_.log_path.empty())
    storage_ = make_storage(cfg_.log_path);
  else
    storage_ = make_storage("");  // returns a NullStorage sink
}

void Simulator::bind_to_core(size_t core_id) {
  /** Pins the current thread to a specific physical CPU core
   * so that the OS scheduler stops moving it around between cores.
   *
   * That one move changes how cache, memory, and timing behave.
   *
   * Without affinity:
   *   Windows/Linux can migrate your threads between cores.
   *   Each migration flushes its L1/L2 caches -> cold caches -> jitter.
   *   The thread's working memory may sit on a different NUMA node -> remote
   *    memory latency.
   *
   * With affinity:
   *   The thread ALWAYS runs on that one core.
   *   All allocations it makes (like its PMR arena) are serviced by that core's
   *    NUMA node. Cache lines stay hot and deterministic.
   *
   * In an HFT system, this is the difference between 70ns and 250ns latency
   * spikes.
   */
#ifdef _WIN32
  // bitmask representing which cores this thread may run on
  DWORD_PTR mask = 1ull << core_id;
  if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
    std::cerr << "[WARN] SetThreadAffinityMask failed for core " << core_id
              << " (err=" << GetLastError() << ")\n";
  }

  // --- Log NUMA node for this core ---
  USHORT node = 0;
  PROCESSOR_NUMBER pn{};
  pn.Group = 0;
  pn.Number = static_cast<BYTE>(core_id);
  if (GetNumaProcessorNodeEx(&pn, &node)) {
    safe_log("[Affinity] Thread pinned to core ", core_id, " on NUMA node ",
             node);
  } else {
    safe_log("[Affinity] Thread pinned to core ", core_id,
             " (NUMA node unknown)");
  }
#else
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    std::cerr << "[WARN] pthread_setaffinity_np failed for core " << core_id
              << "(errno=" << rc << ")\n";
  }

  // --- Log NUMA node for this core ---
  int node = -1;
// requires <numaif.h> and linking with -lnuma on Linux
#ifdef __linux__
  if (numa_node_of_cpu(core_id, &node) == 0)
    std::cout << "[Affinity] Thread pinned to core " << core_id
              << " on NUMA node " << node << "\n";
  else
    std::cout << "[Affinity] Thread pinned to core " << core_id
              << " (NUMA node unknown)\n";
#endif
#endif
}

uint64_t Simulator::make_ts(uint64_t i_event, uint32_t thread_id) const {
  if (cfg_.realtime_ts) return now_ns();
  // Deterministic + fast. Unique across threads.
  return (uint64_t(thread_id) << 48) | i_event;
}

uint64_t Simulator::now_ns() const {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

double Simulator::draw_price(double mid, uint64_t i_event, ThreadContext& ctx) {
  double sigma = cfg_.sigma;

  if (cfg_.drift_ampl > 0.0 && cfg_.drift_period > 0) {
    double phase =
        double(i_event % cfg_.drift_period) / double(cfg_.drift_period);
    sigma *= (1.0 + cfg_.drift_ampl * std::sin(phase * 2.0 * M_PI));
  }

  return ctx.normal(ctx.rng, mid, mid * sigma);
}

void Simulator::emit_event(const Event& e) {
  storage_->write(e);

#ifdef MSIM_WITH_GRPC
  if (grpc_sink_) grpc_sink_->write_event(e);
#endif
}

void Simulator::run() {
  using clock = std::chrono::high_resolution_clock;
  auto t0 = clock::now();

  uint64_t adds = 0, cancels = 0, trades = 0;

  ThreadContext ctx;
  ctx.rng = Xoroshiro128Plus(cfg_.seed);
  ctx.normal = NormalBM{};

  // Build stable arrays so we don't hash strings in the loop
  std::vector<std::string> names;
  std::vector<SymState*> states;
  names.reserve(syms_.size());
  states.reserve(syms_.size());
  for (auto& kv : syms_) {
    names.push_back(kv.first);
    states.push_back(&kv.second);
  }

  // Per-symbol live id list (may contain stale ids; we clean on failed cancel)
  std::vector<std::vector<uint64_t>> live(states.size());

  for (uint64_t i = 0; i < cfg_.total_events; ++i) {
    const size_t si = rand_index(ctx.rng, states.size());
    SymState& st = *states[si];
    OrderBook& book = *st.book;

    auto& live_ids = live[si];
    const bool do_add = rand_bool(ctx.rng, 0.5);

    if (do_add || live_ids.empty()) {
      const Side side = rand_bool(ctx.rng, 0.5) ? Side::BUY : Side::SELL;
      const double p = draw_price(st.mid, i, ctx);
      const int qty = rand_int(ctx.rng, 1, 100);

      const uint64_t id = next_order_id_++;
      const uint64_t ts = make_ts(i, /*thread_id=*/0);

      Order o{id, p, qty, side, ts};

      double trade_px = 0.0;
      const int matched = book.add_order(o, trade_px);

      Event e;
      e.ts_ns = ts;
      e.symbol = names[si];
      e.price = o.price;
      e.qty = o.qty;
      e.side = o.side;

      if (matched > 0) {
        e.type = EventType::TRADE;
        e.price = trade_px;
        e.qty = matched;
        emit_event(e);
        ++trades;
      } else {
        e.type = EventType::ORDER_ADD;
        emit_event(e);
        ++adds;
      }

      // If not fully filled, the order rests and can be canceled later
      if (matched < qty) live_ids.push_back(id);

      // Mid update
      auto bb = book.best_bid();
      auto ba = book.best_ask();
      if (bb && ba)
        st.mid = (*bb + *ba) * 0.5;
      else if (bb)
        st.mid = *bb;
      else if (ba)
        st.mid = *ba;

    } else {
      // Cancel a random known-live id. If it's stale, we drop it.
      const size_t li = rand_index(ctx.rng, live_ids.size());
      const uint64_t victim = live_ids[li];
      live_ids[li] = live_ids.back();
      live_ids.pop_back();

      if (book.cancel_order(victim)) {
        Event e;
        e.ts_ns = make_ts(i, /*thread_id=*/0);
        e.type = EventType::ORDER_CANCEL;
        e.symbol = names[si];
        e.price = 0.0;
        e.qty = 0;
        e.side = Side::BUY;
        emit_event(e);
        ++cancels;
      }
    }
  }

  storage_->flush();
  auto t1 = clock::now();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  double evps = (cfg_.total_events * 1e6) / (double)us;

  std::cout << "MarketSim (PMR) Report\n"
            << "---------------------------\n"
            << "Symbols:           " << syms_.size() << "\n"
            << "Total events:      " << cfg_.total_events << "\n"
            << "Adds:              " << adds << "\n"
            << "Cancels:           " << cancels << "\n"
            << "Trades:            " << trades << "\n"
            << "Elapsed:           " << us / 1000.0 << " ms\n"
            << "Throughput:        " << (uint64_t)evps << " ev/s\n";

  if (cfg_.print_arena) {
    std::cout << "Arena usage (upstream bytes requested):\n";
    for (auto& kv : syms_) {
      std::cout << "  " << kv.first << ": "
                << kv.second.mem->counter.bytes_allocated() << " bytes\n";
    }
  }
  std::cout << "---------------------------\n";
}  // Simulator::run

void Simulator::run_mt() {
  using clock = std::chrono::high_resolution_clock;
  auto t0 = clock::now();

  const size_t n_symbols = syms_.size();
  size_t n_threads =
      (cfg_.num_threads > 0)
          ? static_cast<size_t>(cfg_.num_threads)
          : std::min(n_symbols, static_cast<size_t>(std::thread::hardware_concurrency()));

  n_threads = std::max<size_t>(1, std::min(n_threads, n_symbols));
  const size_t per_thread = (n_symbols + n_threads - 1) / n_threads;

  // Gather symbol names once
  std::vector<std::string> all_syms;
  all_syms.reserve(n_symbols);
  for (auto& kv : syms_) all_syms.push_back(kv.first);

  std::vector<std::thread> workers;
  std::vector<ThreadContext> contexts(n_threads);

  // Build per-thread contexts (symbols + books + mids + live lists)
  size_t idx = 0;
  for (size_t t = 0; t < n_threads; ++t) {
    ThreadContext& ctx = contexts[t];

    const size_t start = idx;
    const size_t end = std::min(start + per_thread, n_symbols);
    idx = end;

    ctx.symbols.reserve(end - start);
    for (size_t i = start; i < end; ++i) ctx.symbols.push_back(all_syms[i]);

    ctx.rng = Xoroshiro128Plus(cfg_.seed + static_cast<uint64_t>(t));
    ctx.normal = NormalBM{};
    ctx.arena = std::make_unique<ArenaBundle>(cfg_.arena_bytes);

    ctx.books.reserve(ctx.symbols.size());
    ctx.mid.resize(ctx.symbols.size(), 100.0);
    ctx.live.resize(ctx.symbols.size());

    for (size_t i = 0; i < ctx.symbols.size(); ++i) {
      ctx.books.emplace_back(std::make_unique<OrderBook>(ctx.symbols[i], &ctx.arena->arena));
    }
  }

  // Launch workers
  workers.reserve(n_threads);
  for (size_t t = 0; t < n_threads; ++t) {
    workers.emplace_back([this, &contexts, t, n_threads]() {
      ThreadContext& ctx = contexts[t];
      if (ctx.symbols.empty()) return;

      this->bind_to_core(t);

      auto t0_thread = clock::now();

      const uint64_t base = cfg_.total_events / n_threads;
      const uint64_t rem  = cfg_.total_events % n_threads;
      const uint64_t iters = base + (t == n_threads - 1 ? rem : 0);

      uint64_t local_id = 1;  // thread-local ids; no contention

      for (uint64_t i = 0; i < iters; ++i) {
        const size_t si = rand_index(ctx.rng, ctx.symbols.size());
        OrderBook& book = *ctx.books[si];
        auto& live_ids = ctx.live[si];

        const bool do_add = rand_bool(ctx.rng, 0.5);

        if (do_add || live_ids.empty()) {
          const Side side = rand_bool(ctx.rng, 0.5) ? Side::BUY : Side::SELL;
          const double p = draw_price(ctx.mid[si], i, ctx);
          const int qty = rand_int(ctx.rng, 1, 100);

          const uint64_t id = (uint64_t(t) << 56) | local_id++;
          const uint64_t ts = make_ts(i, static_cast<uint32_t>(t));

          Order o{id, p, qty, side, ts};

          double trade_px = 0.0;
          const int matched = book.add_order(o, trade_px);

          Event e;
          e.ts_ns = ts;
          e.symbol = ctx.symbols[si];
          e.price = o.price;
          e.qty = o.qty;
          e.side = o.side;

          if (matched > 0) {
            e.type = EventType::TRADE;
            e.price = trade_px;
            e.qty = matched;
            emit_event(e);
            ++ctx.trades;
          } else {
            e.type = EventType::ORDER_ADD;
            emit_event(e);
            ++ctx.adds;
          }

          if (matched < qty) live_ids.push_back(id);

          auto bb = book.best_bid();
          auto ba = book.best_ask();
          if (bb && ba)
            ctx.mid[si] = (*bb + *ba) * 0.5;
          else if (bb)
            ctx.mid[si] = *bb;
          else if (ba)
            ctx.mid[si] = *ba;

        } else {
          const size_t li = rand_index(ctx.rng, live_ids.size());
          const uint64_t victim = live_ids[li];
          live_ids[li] = live_ids.back();
          live_ids.pop_back();

          if (book.cancel_order(victim)) {
            Event e;
            e.ts_ns = make_ts(i, static_cast<uint32_t>(t));
            e.type = EventType::ORDER_CANCEL;
            e.symbol = ctx.symbols[si];
            e.price = 0.0;
            e.qty = 0;
            e.side = Side::BUY;
            emit_event(e);
            ++ctx.cancels;
          }
        }
      }

      auto t1_thread = clock::now();
      ctx.elapsed_ms =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1_thread - t0_thread)
              .count();
    });
  }

  for (auto& th : workers) th.join();

  uint64_t adds = 0, cancels = 0, trades = 0;
  double max_ms = 0.0;
  for (auto& c : contexts) {
    adds += c.adds;
    cancels += c.cancels;
    trades += c.trades;
    max_ms = std::max(max_ms, c.elapsed_ms);
  }

  auto t1 = clock::now();
  const double elapsed_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
  const double evps = (cfg_.total_events * 1000.0) / elapsed_ms;

  std::cout << "\nPer-Thread Summary\n-------------------------------\n";
  for (size_t t = 0; t < n_threads; ++t) {
    const auto& c = contexts[t];
    std::cout << "[Thread " << t << "] Symbols=" << c.symbols.size()
              << " Adds=" << c.adds << " Cancels=" << c.cancels
              << " Trades=" << c.trades << " Time=" << c.elapsed_ms << " ms\n";
  }
  std::cout << "-------------------------------\n"
            << "Threads:       " << n_threads << "\n"
            << "Total events:  " << cfg_.total_events << "\n"
            << "Adds:          " << adds << "\n"
            << "Cancels:       " << cancels << "\n"
            << "Trades:        " << trades << "\n"
            << "Elapsed (max): " << max_ms << " ms\n"
            << "Throughput:    " << static_cast<uint64_t>(evps) << " ev/s\n"
            << "-------------------------------\n";
}  // Simulator::run_mt (multi-threaded)

}  // namespace msim
