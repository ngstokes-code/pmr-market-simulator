#define _USE_MATH_DEFINES
#include "msim/simulator.hpp"

#include <cmath>
#include <iostream>

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
  storage_ = make_storage(cfg_.log_path);
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
}

}  // namespace msim
