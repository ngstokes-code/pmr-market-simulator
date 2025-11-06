
#pragma once
#include "order_book.hpp"
#include "storage.hpp"
#include "pmr_utils.hpp"
#include <unordered_map>
#include <random>
#include <chrono>
#include <memory_resource>
#include <vector>

namespace msim {

struct SimConfig {
    uint64_t total_events = 100000;
    uint64_t seed = 42;
    std::vector<std::string> symbol_list; // if empty, auto-pick N=3
    size_t arena_bytes = (1<<20); // per-symbol arena
    double sigma = 0.001;         // base fractional sigma (0.1% of mid)
    double drift_ampl = 0.0;      // 0.0 = off
    uint64_t drift_period = 10000;
    std::string log_path;
    bool print_arena = false;
};

class Simulator {
public:
    explicit Simulator(SimConfig cfg);
    void run();

private:
    SimConfig cfg_;
    std::mt19937_64 rng_;

    struct ArenaBundle {
        std::vector<std::byte> buffer;
        CountingResource counter;
        std::pmr::monotonic_buffer_resource arena;
        explicit ArenaBundle(size_t bytes)
        : buffer(bytes)
        , counter(std::pmr::new_delete_resource())
        , arena(buffer.data(), buffer.size(), &counter) {}
    };

    struct SymState {
        std::unique_ptr<ArenaBundle> mem;
        std::unique_ptr<OrderBook> book;
        double mid = 100.0;
    };

    std::unordered_map<std::string, SymState> syms_;
    std::unique_ptr<IStorage> storage_;
    std::uniform_int_distribution<int> qty_dist_{1, 100};
    std::bernoulli_distribution side_dist_{0.5};
    std::bernoulli_distribution add_dist_{0.9};

    uint64_t next_order_id_ = 1;

    uint64_t now_ns() const;
    double draw_price(const std::string& s, uint64_t i_event);
    void emit_event(const Event& e);
    static std::vector<std::string> default_symbols();
};

} // namespace msim
