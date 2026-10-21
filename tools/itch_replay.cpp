#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

#include "feed_output.hpp"
#include "msim/input_file.hpp"
#include "msim/replay.hpp"

namespace {
using msim::ReplayResult;
std::string summary(const ReplayResult& r, const msim::ReplayOptions& o,
                    const char* status) {
  std::ostringstream s;
  s.precision(12);
  s << "{\"status\":" << feed_json(status)
    << ",\"protocol\":\"Nasdaq TotalView-ITCH 5.0\",\"scope\":\"complete-supported-session-bytes\""
    << ",\"capture_continuity\":\"UNVERIFIED\",\"accounting_relationships\":\"not-certified-by-displayed-replay\""
    << ",\"input\":{\"bytes\":" << r.bytes << ",\"sha256\":" << feed_json(r.input_sha256) << "}"
    << ",\"messages\":" << r.messages << ",\"rejected_events\":0,\"terminal_event\":\"" << r.terminal_event
    << "\",\"live_orders\":" << r.live_orders << ",\"levels\":" << r.levels
    << ",\"clock_regressions\":" << r.clock_regressions << ",\"worker_count\":" << o.workers
    << ",\"reference_births\":" << r.reference_births << ",\"day_reference_blocks\":" << r.day_reference_blocks
    << ",\"chunk_messages\":" << o.chunk_messages << ",\"buffer_bytes\":" << r.buffer_bytes
    << ",\"reference_index\":\"" << (o.reference_mode==msim::RefMap::Mode::Window ? "window-and-hash" : "hash-only")
    << "\",\"instrumented\":" << (r.instrumented?"true":"false")
    << ",\"timing_seconds\":{\"library_total\":" << r.total_s
    << ",\"setup\":" << r.setup_s << ",\"pipeline\":" << r.pipeline_s
    << ",\"final_validation\":" << r.final_validation_s << ",\"read_and_content_hash\":" << r.read_hash_s
    << ",\"coordinator_framing_validation_including_read\":" << r.coordinator_s
    << ",\"coordinator_pipeline_cpu\":" << r.coordinator_pipeline_cpu_s
    << ",\"coordinator_cpu\":" << r.coordinator_cpu_s << "},\"types\":{";
  bool first=true;
  for (std::size_t i=0;i<r.type_counts.size();++i) if (r.type_counts[i]) {
    if (!first) s << ',';
    first=false;
    s << feed_json(std::string(1,static_cast<char>(i))) << ':' << r.type_counts[i];
  }
  s << "},\"workers\":["; first=true;
  for (const auto& w : r.workers) {
    if (!first) s << ',';
    first=false;
    s << "{\"events\":" << w.events << ",\"lifecycle_events\":" << w.lifecycle_events
      << ",\"busy_s\":" << w.busy_s << ",\"wait_s\":" << w.wait_s << ",\"cpu_s\":" << w.cpu_s
      << ",\"decode_dispatch_residual_s\":" << w.decode_s << ",\"metadata_s\":" << w.metadata_s
      << ",\"mutation_s\":" << w.mutation_s << ",\"peak_orders\":" << w.peak_orders
      << ",\"old_orders\":" << w.old_orders << '}';
  }
  s << "]}";
  return s.str();
}
void cross_csv(std::FILE* file, const ReplayResult& r) {
  std::fputs("locate,stock,listing,type,price,shares\n",file);
  for (const auto& c : r.crosses) {
    const auto stock=feed_csv_field(feed_stock(c.stock.data()));
    const auto listing=feed_csv_field(std::string(1,c.listing));
    std::fprintf(file,"%u,%s,%s,%c,%u.%04u,%llu\n",c.locate,stock.c_str(),listing.c_str(),
        c.type,c.price/10000,c.price%10000,static_cast<unsigned long long>(c.shares));
  }
}
int run(int argc, char** argv) {
  if (argc<2) throw std::invalid_argument(
      "usage: itch_replay INPUT [--threads N] [--chunk N] [--out DIR | --crosses CSV] [--reference] [--profile-stages]");
  msim::ReplayOptions options;
  std::string directory, crosses;
  for (int i=2;i<argc;++i) {
    const std::string flag=argv[i];
    if (flag=="--reference") options.reference=true;
    else if (flag=="--profile-stages") options.instrument_stages=true;
    else {
      if (i+1==argc) throw std::invalid_argument("missing option value");
      const char* value=argv[++i];
      if (flag=="--out") directory=value;
      else if (flag=="--crosses") crosses=value;
      else if (flag=="--index") {
        const std::string mode=value;
        if (mode=="hash") options.reference_mode=msim::RefMap::Mode::HashOnly;
        else if (mode!="window") throw std::invalid_argument("--index must be window or hash");
      } else {
        const auto n=feed_number(value);
        if (n>=UINT32_MAX) throw std::invalid_argument("replay limit exceeds supported range");
        const auto size=static_cast<std::size_t>(n);
        if (flag=="--threads") options.workers=size;
        else if (flag=="--chunk") options.chunk_messages=size;
        else if (flag=="--max-orders") options.global_orders=size;
        else if (flag=="--worker-orders") options.worker_orders=size;
        else if (flag=="--max-levels") options.max_levels=size;
        else if (flag=="--output-records") options.output_records=size;
        else if (flag=="--cross-records") options.cross_records=size;
        else if (flag=="--day-reference-blocks") options.day_reference_blocks=size;
        else if (flag=="--window-bits") options.reference_window_bits=static_cast<unsigned>(n);
        else throw std::invalid_argument("unknown replay option");
      }
    }
  }
  if (!directory.empty() && !crosses.empty())
    throw std::invalid_argument("choose --out or --crosses");
  options.capture_state=!directory.empty();
  options.capture_crosses=!directory.empty() || !crosses.empty();
  msim::InputFile source_guard(argv[1]);
  if (!crosses.empty()) source_guard.reject_alias(crosses);
  std::unique_ptr<FeedOutput> csv;
  std::unique_ptr<FeedDirectory> output;
  if (!crosses.empty()) csv=std::make_unique<FeedOutput>(crosses.c_str());
  if (!directory.empty()) output=std::make_unique<FeedDirectory>(directory);
  const auto result=msim::replay(argv[1],options);
  const auto manifest=summary(result,options,"SUCCESS");
  if (csv) cross_csv(csv->file(),result);
  if (output) {
    auto write=[&](const char* name, const auto& fn) {
      const auto path=(output->path()/name).string();
      FeedOutput file(path.c_str()); fn(file.file()); file.publish(); file.commit();
    };
    write("state.csv",[&](std::FILE* f) {
      std::fputs("locate,side,price,ref,shares\n",f);
      for (const auto& [loc,side,price,ref,shares] : result.state)
        std::fprintf(f,"%u,%c,%u,%llu,%u\n",loc,side==msim::Side::BUY?'B':'S',price,
            static_cast<unsigned long long>(ref),shares);
    });
    write("metadata.csv",[&](std::FILE* f) {
      std::fputs("locate,stock,category,authenticity,round_lot,trading,operational,high_timestamp,clock_regressions,trading_update\n",f);
      for (const auto& row : result.symbols) {
        const auto& m=row.state;
        const auto stock=feed_csv_field(feed_stock(m.stock.data()));
        const auto category=feed_csv_field(std::string(1,m.category));
        const auto authenticity=feed_csv_field(std::string(1,m.authenticity));
        std::fprintf(f,"%u,%s,%s,%s,%u,%c,%c,%llu,%llu,%llu\n",row.locate,stock.c_str(),
            category.c_str(),authenticity.c_str(),m.round_lot,m.trading,m.nasdaq_operation,
            static_cast<unsigned long long>(m.high_timestamp),
            static_cast<unsigned long long>(m.clock_regressions),
            static_cast<unsigned long long>(m.trading_update));
      }
    });
    write("day-references.csv",[&](std::FILE* f) {
      std::fputs("ref\n",f);
      for (auto ref : result.day_references)
        std::fprintf(f,"%llu\n",static_cast<unsigned long long>(ref));
    });
    write("crosses.csv",[&](std::FILE* f) { cross_csv(f,result); });
    write("result.json",[&](std::FILE* f) { std::fprintf(f,"%s\n",manifest.c_str()); });
  }
  source_guard.validate_unchanged();
  if (!crosses.empty()) source_guard.reject_alias(crosses);
  const auto diagnostic=summary(result,options,"VALIDATED");
  std::printf("%s\n",diagnostic.c_str());
  if (std::fflush(stdout) || std::ferror(stdout))
    throw std::runtime_error("console output write failed");
  if (csv) csv->publish();
  if (output) output->publish();
  std::puts("result: SUCCESS scope=complete-supported-session-bytes capture_completeness=unverified");
  if (std::fflush(stdout) || std::ferror(stdout))
    throw std::runtime_error("console output write failed");
  if (csv) csv->commit();
  if (output) output->commit();
  return 0;
}
}  // namespace
int main(int argc,char** argv) {
  std::signal(SIGPIPE,SIG_IGN);
  std::signal(SIGXFSZ,SIG_IGN);
  try { return run(argc,argv); }
  catch (const msim::ReplayError& e) {
    std::fprintf(stderr,"{\"status\":\"INVALID\",\"rejected_events\":1,\"message\":%llu,\"offset\":%llu,\"validated_messages\":%llu,\"applied_messages\":%llu,\"reason\":",
        static_cast<unsigned long long>(e.ordinal),static_cast<unsigned long long>(e.offset),
        static_cast<unsigned long long>(e.validated_messages),static_cast<unsigned long long>(e.applied_messages));
    feed_json_write(stderr,e.what()); std::fputs("}\n",stderr);
  } catch (const std::exception& e) {
    std::fprintf(stderr,"itch_replay: %s\n",e.what());
  } catch (...) { std::fputs("itch_replay: unknown failure\n",stderr); }
  std::fputs("result: INVALID\n",stderr);
  return 2;
}
