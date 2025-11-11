#define _USE_MATH_DEFINES
#include "msim/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>

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

uint64_t Simulator::now_ns() const {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

double Simulator::draw_price(const std::string& s, uint64_t i_event) {
  auto& st = syms_.at(s);
  double mid = st.mid;
  double sigma = cfg_.sigma;
  if (cfg_.drift_ampl > 0.0 && cfg_.drift_period > 0) {
    double phase =
        (double)(i_event % cfg_.drift_period) / (double)cfg_.drift_period;
    sigma *= (1.0 + cfg_.drift_ampl * std::sin(phase * 2.0 * M_PI));
  }
  std::normal_distribution<double> dist(mid, mid * sigma);
  return dist(rng_);
}

double Simulator::draw_price(double mid, uint64_t i_event,
                             std::mt19937_64& rng) {
  double sigma = cfg_.sigma;
  if (cfg_.drift_ampl > 0.0 && cfg_.drift_period > 0) {
    double phase =
        (double)(i_event % cfg_.drift_period) / (double)cfg_.drift_period;
    sigma *= (1.0 + cfg_.drift_ampl * std::sin(phase * 2.0 * M_PI));
  }
  std::normal_distribution<double> dist(mid, mid * sigma);
  return dist(rng);
}

void Simulator::emit_event(const Event& e) { storage_->write(e); }

void Simulator::run() {
  using clock = std::chrono::high_resolution_clock;
  auto t0 = clock::now();
  uint64_t adds = 0, cancels = 0, trades = 0;

  std::vector<std::string> names;
  names.reserve(syms_.size());
  for (auto& kv : syms_) names.push_back(kv.first);
  std::uniform_int_distribution<size_t> sym_pick(0, names.size() - 1);

  for (uint64_t i = 0; i < cfg_.total_events; ++i) {
    auto& s = names[sym_pick(rng_)];
    auto& st = syms_.at(s);
    auto& book = *st.book;

    bool is_add = add_dist_(rng_);
    if (is_add || next_order_id_ <= 10) {
      char side = side_dist_(rng_) ? 'B' : 'S';
      double p = draw_price(s, i);
      int qty = qty_dist_(rng_);
      uint64_t id = next_order_id_++;
      Order o{id, p, qty, side, now_ns()};

      double trade_px = 0.0;
      int matched = book.add_order(o, trade_px);

      Event e;
      e.ts_ns = o.ts_ns;
      e.symbol = s;
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

      auto bb = book.best_bid();
      auto ba = book.best_ask();
      if (bb && ba)
        st.mid = (*bb + *ba) * 0.5;
      else if (bb)
        st.mid = *bb;
      else if (ba)
        st.mid = *ba;
    } else {
      uint64_t victim = (next_order_id_ > 1) ? (next_order_id_ - 1) : 1;
      bool ok = book.cancel_order(victim);
      if (ok) {
        Event e{now_ns(), EventType::ORDER_CANCEL, s, 0.0, 0, 'B'};
        emit_event(e);
        ++cancels;
      }
    }
  }
  storage_->flush();
  auto t1 = clock::now();
  auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
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
  std::ios::sync_with_stdio(false);
  std::cout.setf(std::ios::unitbuf);

  using clock = std::chrono::high_resolution_clock;
  auto t0 = clock::now();

  int T = std::min<int>(cfg_.num_threads, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  std::vector<ThreadContext> ctxs(T);

  // Partition symbols among threads
  size_t i = 0;
  for (auto& kv : syms_) {
    ctxs[i % T].symbols.push_back(kv.first);
    i++;
  }

  // Launch workers
  safe_log("[Partitioning]");
  for (int tid = 0; tid < T; ++tid) {
    threads.emplace_back([&, tid]() {
      try {
        bind_to_core(tid % std::thread::hardware_concurrency());
        auto& ctx = ctxs[tid];
        ctx.rng.seed(cfg_.seed + tid);

        if (ctx.symbols.empty()) {
          safe_log("[Thread ", tid, "] no symbols, exiting.\n");
          return;
        }

        // Local per-thread OrderBooks and arenas
        ctx.arena = std::make_unique<ArenaBundle>(cfg_.arena_bytes);
        for (auto& sym : ctx.symbols) {
          ctx.books[sym] = std::make_unique<OrderBook>(sym, &ctx.arena->arena);
          ctx.mid[sym] = 100.0;  // initialize per-symbol midprice
          ctx.live[sym] = {};
          ctx.live[sym].reserve(1 << 16);
        }

        std::uniform_int_distribution<size_t> sym_pick(0,
                                                       ctx.symbols.size() - 1);
        std::bernoulli_distribution add_dist(0.9);
        std::bernoulli_distribution side_dist(0.5);
        std::uniform_int_distribution<int> qty_dist(1, 100);

        auto start = clock::now();
        uint64_t local_events = 0;

        for (uint64_t ev = tid; ev < cfg_.total_events; ev += T) {
          if (ctx.symbols.empty()) {
            safe_err("[Thread ", tid, "] no symbols assigned — exiting early");
            return;
          }
          const auto& s = ctx.symbols[sym_pick(ctx.rng)];
          auto& book = *ctx.books[s];
          bool is_add = add_dist(ctx.rng);

          if (is_add) {
            char side = side_dist(ctx.rng) ? 'B' : 'S';
            double p = draw_price(ctx.mid[s], ev, ctx.rng);
            int qty = qty_dist(ctx.rng);
            uint64_t id = ev + 1;
            Order o{id, p, qty, side, now_ns()};
            double trade_px = 0.0;
            int matched = book.add_order(o, trade_px);
            ctx.live[s].push_back(id);

            auto bb = book.best_bid();
            auto ba = book.best_ask();
            if (bb && ba)
              ctx.mid[s] = (*bb + *ba) * 0.5;
            else if (bb)
              ctx.mid[s] = *bb;
            else if (ba)
              ctx.mid[s] = *ba;

            Event e{o.ts_ns,
                    matched > 0 ? EventType::TRADE : EventType::ORDER_ADD,
                    s,
                    matched > 0 ? trade_px : o.price,
                    matched > 0 ? matched : o.qty,
                    side};
            emit_event(e);
            matched > 0 ? ++ctx.trades : ++ctx.adds;
          } else {
            auto& pool = ctx.live[s];
            if (!pool.empty()) {
              size_t k = std::uniform_int_distribution<size_t>(
                  0, pool.size() - 1)(ctx.rng);
              uint64_t victim = pool[k];
              pool[k] = pool.back();
              pool.pop_back();
              if (pool.capacity() > 65536 && pool.size() < 32768) {
                pool.shrink_to_fit();  // keep `live[s]` capacity stable, reduce
                                       // unnecessary heap growth
              }

              bool ok = book.cancel_order(victim);
              if (ok) {
                Event e{now_ns(), EventType::ORDER_CANCEL, s, 0.0, 0, 'B'};
                emit_event(e);
                ++ctx.cancels;
              }
            }
          }
          ++local_events;
        }

        auto end = clock::now();
        ctx.elapsed_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count() /
            1000.0;
        ctx.total_events = local_events;

        safe_log("[Thread ", tid, "] done. Events=", ctx.total_events,
                 " Time=", ctx.elapsed_ms, " ms\n");
      } catch (const std::exception& ex) {
        safe_err("[Thread ", tid, "] ERROR: ", ex.what(), "\n");
      } catch (...) {
        safe_err("[Thread ", tid, "] ERROR: unknown exception\n");
      }
    });
  }

  std::cout << "[Main] Waiting for " << threads.size()
            << " threads to finish...\n";
  for (auto& t : threads) {
    std::cerr << "[Main] Joining thread...\n";
    t.join();
  }
  std::cerr << "[Main] All threads joined.\n";
  storage_->flush();
  std::cout << "[Main] All threads joined. Aggregating metrics...\n";

  // ─────────────── Aggregate Metrics ───────────────
  uint64_t adds = 0, cancels = 0, trades = 0, total_events = 0;
  double total_ms = 0.0;

  std::cout << "\nPer-Thread Summary\n";
  std::cout << "-------------------------------\n";
  for (int tid = 0; tid < T; ++tid) {
    auto& c = ctxs[tid];
    double evps =
        (c.elapsed_ms > 0.0) ? (c.total_events / c.elapsed_ms) * 1000.0 : 0.0;
    adds += c.adds;
    cancels += c.cancels;
    trades += c.trades;
    total_events += c.total_events;
    total_ms = std::max(total_ms, c.elapsed_ms);

    std::cout << "[Thread " << tid << "] "
              << "Symbols=" << c.symbols.size() << "  Adds=" << c.adds
              << "  Cancels=" << c.cancels << "  Trades=" << c.trades
              << "  Events=" << c.total_events << "  Time=" << c.elapsed_ms
              << " ms"
              << "  -> " << static_cast<uint64_t>(evps) << " ev/s\n";
  }

  double global_evps =
      (total_ms > 0.0) ? (total_events / total_ms) * 1000.0 : 0.0;

  std::cout << "-------------------------------\n";
  std::cout << "Threads:       " << T << "\n"
            << "Total events:  " << total_events << "\n"
            << "Adds:          " << adds << "\n"
            << "Cancels:       " << cancels << "\n"
            << "Trades:        " << trades << "\n"
            << "Elapsed (max): " << total_ms << " ms\n"
            << "Throughput:    " << static_cast<uint64_t>(global_evps)
            << " ev/s\n";
  std::cout << "-------------------------------\n";
}  // Simulator::run_mt (multi-threaded)

}  // namespace msim
