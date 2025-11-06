
#include "simulator.hpp"
#include <iostream>
#include <sstream>

using namespace msim;

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) if(!item.empty()) out.push_back(item);
    return out;
}

int main(int argc, char** argv) {
    SimConfig cfg;
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a=="--events" && i+1<argc) cfg.total_events = std::stoull(argv[++i]);
        else if (a=="--seed" && i+1<argc) cfg.seed = std::stoull(argv[++i]);
        else if (a=="--symbols" && i+1<argc) cfg.symbol_list = split_csv(argv[++i]);
        else if (a=="--arena-bytes" && i+1<argc) cfg.arena_bytes = std::stoull(argv[++i]);
        else if (a=="--sigma" && i+1<argc) cfg.sigma = std::stod(argv[++i]);
        else if (a=="--drift-ampl" && i+1<argc) cfg.drift_ampl = std::stod(argv[++i]);
        else if (a=="--drift-period" && i+1<argc) cfg.drift_period = std::stoull(argv[++i]);
        else if (a=="--log" && i+1<argc) cfg.log_path = argv[++i];
        else if (a=="--print-arena") cfg.print_arena = true;
        else if (a=="--help") {
            std::cout << "Usage: ./market_sim [options]\n"
                      << "  --events N           Total events (default 100000)\n"
                      << "  --symbols CSV        Symbol list (default AAPL,MSFT,GOOG)\n"
                      << "  --seed S             RNG seed\n"
                      << "  --arena-bytes BYTES  Per-symbol arena size (default 1<<20)\n"
                      << "  --sigma X            Gaussian sigma as fraction of mid (default 0.001)\n"
                      << "  --drift-ampl A       Volatility drift amplitude (default 0.0)\n"
                      << "  --drift-period P     Drift period in events (default 10000)\n"
                      << "  --log PATH           Append-only event log path\n"
                      << "  --print-arena        Print arena upstream usage\n";
            return 0;
        }
    }
    Simulator sim(cfg);
    sim.run();
    return 0;
}
