
#include <iostream>
#include <sstream>

#include "msim/lmdb_reader.hpp"
#include "msim/simulator.hpp"

using namespace msim;

static std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ','))
    if (!item.empty()) out.push_back(item);
  return out;
}

int main(int argc, char** argv) {
  SimConfig cfg;
  bool no_log = false;
  bool read_mode = false;
  std::string read_path;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--events" && i + 1 < argc)
      cfg.total_events = std::stoull(argv[++i]);
    else if (a == "--seed" && i + 1 < argc)
      cfg.seed = std::stoull(argv[++i]);
    else if (a == "--symbols" && i + 1 < argc)
      cfg.symbol_list = split_csv(argv[++i]);
    else if (a == "--arena-bytes" && i + 1 < argc)
      cfg.arena_bytes = std::stoull(argv[++i]);
    else if (a == "--sigma" && i + 1 < argc)
      cfg.sigma = std::stod(argv[++i]);
    else if (a == "--drift-ampl" && i + 1 < argc)
      cfg.drift_ampl = std::stod(argv[++i]);
    else if (a == "--drift-period" && i + 1 < argc)
      cfg.drift_period = std::stoull(argv[++i]);
    else if (a == "--log" && i + 1 < argc)
      cfg.log_path = argv[++i];
    else if (a == "--print-arena")
      cfg.print_arena = true;
    else if (a == "--dump" && i + 1 < argc)
      cfg.dump_n = std::stoi(argv[++i]);
    else if (a == "--read" && i + 1 < argc) {
      read_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        read_path = argv[++i];
      else
        read_path = "store.mdb";
    } else if (a == "--threads" && i + 1 < argc)
      cfg.num_threads = std::stoi(argv[++i]);
    else if (a == "--no-log") {
      no_log = true;
      cfg.log_path.clear();
    } else if (a == "--help") {
      std::cout
          << "Usage: ./market_sim [options]\n"
          << "  --events N           Total events (default 100000)\n"
          << "  --symbols CSV        Symbol list (default AAPL,MSFT,GOOG)\n"
          << "  --seed S             RNG seed\n"
          << "  --arena-bytes BYTES  Per-symbol arena size (default 1<<20)\n"
          << "  --sigma X            Gaussian sigma as fraction of mid "
             "(default 0.001)\n"
          << "  --drift-ampl A       Volatility drift amplitude (default 0.0)\n"
          << "  --drift-period P     Drift period in events (default 10000)\n"
          << "  --log PATH           Append-only event log path\n"
          << "  --print-arena        Print arena upstream usage\n"
          << "  --read PATH          Read and dump LMDB log instead of sim\n"
          << "  --dump N             Number of events to print per-symbol "
             "(default 0)\n";
      return 0;
    }
  }

  try {
    if (no_log) cfg.log_path.clear();

    if (cfg.num_threads > 1 && cfg.log_path.find(".mdb") != std::string::npos) {
      std::cerr << "[WARN] LMDB logging not thread-safe; disabling logging\n";
      cfg.log_path.clear();
    }

    if (read_mode) {
      LMDBReader reader(
          (read_path.empty() ? std::string("store.mdb") : read_path));
      auto symbols = reader.list_symbols();
      if (symbols.empty()) {
        std::cout << "No symbols found in " << read_path << "\n";
        return 0;
      }

      std::cout << "Found " << symbols.size() << " symbol(s): ";
      for (auto& s : symbols) std::cout << s << " ";
      std::cout << "\n";

      for (auto& sym : symbols) {
        auto events = reader.read_all(sym);
        std::cout << sym << ": " << events.size() << " events\n";

        if (!events.empty() && cfg.dump_n > 0) {
          int n = std::min<int>(cfg.dump_n, events.size());
          std::cout << "First " << n << " events:\n";
          for (int i = 0; i < n; ++i)
            std::cout << " " << events[i].to_string() << "\n";
        }
      }
      return 0;
    }
    std::cout.setf(std::ios::unitbuf);
    Simulator sim(cfg);
    (cfg.num_threads > 1) ? sim.run_mt() : sim.run();
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
