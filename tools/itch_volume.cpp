#include <sys/resource.h>
#include <csignal>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "feed_output.hpp"
#include "msim/protocol_state.hpp"
#include "msim/itch_file.hpp"
#include "msim/volume.hpp"

namespace fs = std::filesystem;
namespace vol = msim::volume;
namespace itch = msim::itch;
namespace {
constexpr std::size_t record_bytes = 48;
std::array<unsigned char, record_bytes> pack(const vol::Record& r) {
  std::array<unsigned char, record_bytes> b{};
  const std::uint64_t words[] = {r.match, r.ordinal, r.timestamp, r.ref,
                                 r.shares};
  for (std::size_t w = 0; w < 5; ++w)
    for (std::size_t i = 0; i < 8; ++i)
      b[w * 8 + i] = static_cast<unsigned char>((words[w] >> (8 * i)) & 255);
  b[40] = static_cast<unsigned char>(r.locate & 255);
  b[41] = static_cast<unsigned char>(r.locate >> 8);
  b[42] = static_cast<unsigned char>(r.kind);
  return b;
}
vol::Record unpack(const std::array<unsigned char, record_bytes>& b) {
  std::array<std::uint64_t, 5> w{};
  for (std::size_t k = 0; k < 5; ++k)
    for (std::size_t i = 0; i < 8; ++i)
      w[k] |= std::uint64_t{b[k * 8 + i]} << (8 * i);
  return {w[0],
          w[1],
          w[2],
          w[3],
          w[4],
          static_cast<std::uint16_t>(b[40] | b[41] << 8),
          static_cast<char>(b[42])};
}
struct Spool {
  msim::FeedBook book;
  msim::MarketHandler<msim::FeedBook> reconstruction{book};
  std::vector<std::ofstream>& files;
  std::uint64_t ordinal = 0;
  std::vector<std::pair<char, std::uint64_t>> events;
  // Keep CLI stack usage independent of the directory universe.
  std::vector<std::string> names;
  explicit Spool(std::vector<std::ofstream>& streams) : files(streams), names(65536) {}
  void record(const itch::Header& h, std::uint64_t match, std::uint64_t ref,
              std::uint64_t qty, char kind) {
    if (kind != 'B' && kind != 'Q' && qty == 0)
      throw std::runtime_error("zero executed shares");
    auto b = pack({match, ordinal, h.timestamp, ref, qty, h.locate, kind});
    auto& out = files[match % files.size()];
    out.write(reinterpret_cast<const char*>(b.data()), b.size());
    if (!out) throw std::runtime_error("partition write failed");
  }
  template<class M> void on(const M& m) { reconstruction.on(m); }
  void on(const itch::StockDirectory& m) {
    reconstruction.on(m); names[m.locate] = std::string(m.stock, 8);
  }
  void on(const itch::OrderExecuted& m) {
    reconstruction.on(m); record(m, m.match, m.ref, m.shares, 'E');
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    reconstruction.on(m);
    record(m, m.match, m.ref, m.shares, m.printable ? 'Y' : 'N');
  }
  void on(const itch::Trade& m) {
    reconstruction.on(m); record(m, m.match, 0, m.shares, 'P');
  }
  void on(const itch::CrossTrade& m) {
    reconstruction.on(m); record(m, m.match, 0, m.shares, 'Q');
  }
  void on(const itch::BrokenTrade& m) {
    reconstruction.on(m); record(m, m.match, 0, 0, 'B');
  }
  void on(const itch::SystemEvent& m) {
    reconstruction.on(m); events.emplace_back(m.code, m.timestamp);
  }
  void on_other(const itch::Header& h) { reconstruction.on_other(h); }
};
void counts(std::ostream& out, const vol::Counts& c) {
  for (auto q : c) out << ',' << q;
  double fraction = 0;
  out << ',';
  if (vol::fraction(c, fraction))
    out << std::setprecision(12) << fraction;
  else
    out << "NA";
}
void finish(std::ofstream& out) {
  out.close();
  if (!out) throw std::runtime_error("report write failed");
}
int run(int argc, char** argv) {
  if (argc < 3)
    throw std::invalid_argument(
        "usage: itch_volume INPUT OUTDIR [--partitions N] [--budget-mib N]");
  std::size_t partitions = 16, budget = 64 * 1024 * 1024;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 == argc) throw std::invalid_argument("missing option value");
    const auto n = feed_number(argv[i + 1]);
    const std::string option = argv[i];
    if (option == "--partitions" && n > 0 && n <= 256)
      partitions = static_cast<std::size_t>(n);
    else if (option == "--budget-mib" && n > 0 && n <= 1024)
      budget = static_cast<std::size_t>(n) * 1024 * 1024;
    else
      throw std::invalid_argument("invalid study option");
  }
  msim::InputFile input_guard(argv[1]);
  input_guard.reject_alias(argv[2]);
  FeedDirectory publication(argv[2]);
  const auto temporary = publication.path();
    const auto spool = temporary / "partitions";
    fs::create_directory(spool);
    std::vector<std::ofstream> files;
    for (std::size_t i = 0; i < partitions; ++i)
      files.emplace_back(spool / std::to_string(i), std::ios::binary);
    Spool handler(files);
    msim::ItchFile reader(argv[1]);
    msim::ItchFile::Frame frame{};
    std::uint64_t messages = 0;
    while (reader.next(frame)) {
      handler.ordinal = frame.ordinal;
      handler.reconstruction.set_context(frame.offset, frame.ordinal);
      const auto decoded =
          itch::decode_checked(frame.data, frame.size, handler);
      if (decoded != itch::DecodeStatus::Decoded)
        throw std::runtime_error(
            std::string(itch::decode_status_name(decoded)) + " at offset " +
            std::to_string(frame.offset));
      if (handler.reconstruction.failed()) {
        report_feed_fault(handler.reconstruction.lifecycle);
        throw std::runtime_error("invalid displayed-book operation");
      }
      ++messages;
    }
    handler.reconstruction.state.finish();
    reader.finish();
    for (auto& stream : files) finish(stream);
    vol::Result result;
    std::size_t peak_ledger = 0;
    for (std::size_t p = 0; p < partitions; ++p) {
      const auto path = spool / std::to_string(p);
      const auto bytes = fs::file_size(path);
      if (bytes % record_bytes)
        throw std::runtime_error("partial partition record");
      const auto n = static_cast<std::size_t>(bytes / record_bytes);
      if (n > budget / sizeof(vol::Record))
        throw std::runtime_error("ledger budget exceeded; increase partitions");
      peak_ledger = std::max(peak_ledger, n * sizeof(vol::Record));
      std::vector<vol::Record> records;
      records.reserve(n);
      std::ifstream in(path, std::ios::binary);
      std::array<unsigned char, record_bytes> b{};
      for (std::size_t i = 0; i < n; ++i) {
        in.read(reinterpret_cast<char*>(b.data()), b.size());
        if (!in) throw std::runtime_error("partition read failed");
        records.push_back(unpack(b));
      }
      result.reconcile(records);
    }
    std::map<std::uint16_t, vol::Counts> symbols;
    std::map<std::uint64_t, vol::Counts> intervals;
    std::ofstream cells(temporary / "cells.csv");
    cells << "locate,stock,interval_ns,E,CY,P,Q,CN,non_displayed_fraction\n";
    for (const auto& entry : result.cells) {
      const auto loc = entry.first.first;
      auto stock = handler.names[loc];
      if (stock.empty())
        throw std::runtime_error("execution has no stock directory entry");
      while (!stock.empty() && stock.back() == ' ') stock.pop_back();
      cells << loc << ',' << feed_csv_field(stock) << ','
            << entry.first.second * vol::interval_ns;
      counts(cells, entry.second);
      cells << '\n';
      for (std::size_t k = 0; k < vol::Categories; ++k) {
        vol::add(symbols[loc][k], entry.second[k]);
        vol::add(intervals[entry.first.second][k], entry.second[k]);
      }
    }
    finish(cells);
    std::ofstream stocks(temporary / "symbols.csv");
    stocks << "locate,stock,E,CY,P,Q,CN,non_displayed_fraction\n";
    for (const auto& s : symbols) {
      auto name = handler.names[s.first];
      while (!name.empty() && name.back() == ' ') name.pop_back();
      stocks << s.first << ',' << feed_csv_field(name);
      counts(stocks, s.second);
      stocks << '\n';
    }
    finish(stocks);
    std::ofstream bins(temporary / "intervals.csv");
    bins << "interval_ns,E,CY,P,Q,CN,non_displayed_fraction\n";
    for (const auto& bin : intervals) {
      bins << bin.first * vol::interval_ns;
      counts(bins, bin.second);
      bins << '\n';
    }
    finish(bins);
    std::ofstream events(temporary / "system-events.csv");
    events << "event,timestamp_ns\n";
    for (const auto& event : handler.events)
      events << event.first << ',' << event.second << '\n';
    finish(events);
    const auto total = result.total();
    struct rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    std::uint64_t rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#if !defined(__APPLE__)
    rss *= 1024;
#endif
    std::ofstream summary(temporary / "summary.json");
    summary << "{\n\"status\":\"SUCCESS\",\n\"protocol\":\"ITCH-5.0\","
            << "\n\"terminal_event\":\"C\",\n\"rejected_events\":0,"
            << "\n\"input_bytes\":" << reader.input().size()
            << ",\n\"input_sha256\":" << feed_json(reader.input().sha256())
            << ",\n\"reference_births\":" << handler.reconstruction.state.reference_births()
            << ",\n\"messages\":" << messages
            << ",\n\"records\":" << result.records
            << ",\n\"breaks\":" << result.breaks
            << ",\n\"corrected_shares\":" << result.corrected_shares
            << ",\n\"companion_groups\":" << result.companion_groups
            << ",\n\"corrected_nonprintable_shares\":"
            << result.corrected_nonprintable_shares
            << ",\n\"peak_ledger_bytes\":" << peak_ledger
            << ",\n\"max_rss_bytes\":" << rss
            << ",\n\"partitions\":" << partitions
            << ",\n\"E\":" << total[vol::E] << ",\n\"CY\":" << total[vol::CY]
            << ",\n\"P\":" << total[vol::P] << ",\n\"Q\":" << total[vol::Q]
            << ",\n\"CN\":" << total[vol::CN]
            << ",\n\"capture_completeness\":\"unverified\"\n}\n";
    finish(summary);
    fs::remove_all(spool);
    reader.finish();
    input_guard.validate_unchanged();
    std::cout << "messages=" << messages << " records=" << result.records
              << " breaks=" << result.breaks
              << " peak_ledger_bytes=" << peak_ledger
              << " max_rss_bytes=" << rss << '\n';
    std::cout.flush();
    if (!std::cout) throw std::runtime_error("stdout write failed");
    publication.publish();
    std::cout << "result: SUCCESS scope=complete-session "
                 "capture_completeness=unverified\n";
    std::cout.flush();
    if (!std::cout) throw std::runtime_error("stdout write failed");
    publication.commit();
    return 0;
}
}  // namespace
int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGXFSZ, SIG_IGN);
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "itch_volume: %s\nresult: INVALID\n", e.what());
    return 2;
  } catch (...) {
    std::fputs("itch_volume: unknown failure\nresult: INVALID\n", stderr);
    return 2;
  }
}
