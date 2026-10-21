// book_bench: generate, inspect, check, and time command streams.
//
//   book_bench generate --workload W --seed S --commands N --out FILE
//   book_bench shape    --stream FILE
//   book_bench verify   --stream FILE
//   book_bench run      --stream FILE [--book engine|reference]
//                       [--memory monotonic|pool|heap] [--mode
//                       throughput|latency|batch]
//                       [--reps R] [--warmup W] [--json FILE] [--meta
//                       KEY=VALUE]...
//
// Timing rules, shared by every book and memory strategy:
//  - the stream is read and decoded before any timer starts,
//  - book construction is timed apart from the replay,
//  - the timed loop does nothing but apply commands,
//  - outcome counts, the execution digest, and memory growth come from a
//    separate untimed pass over the same stream.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <string>
#include <vector>

#include "build_info.hpp"
#include "msim/command.hpp"
#include "msim/differential.hpp"
#include "msim/digest.hpp"
#include "msim/order_book.hpp"
#include "msim/pmr_utils.hpp"
#include "msim/reference_book.hpp"
#include "msim/workload.hpp"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <sys/resource.h>

// Interpose C++ allocations on this executable's measured calling thread.
// PMR and execution-vector accounting remain separate diagnostic surfaces.
namespace allocation_probe { thread_local bool active=false; thread_local std::uint64_t calls=0; }
void* operator new(std::size_t n) {
  if (allocation_probe::active) ++allocation_probe::calls;
  if (void* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n,std::align_val_t a) {
  if (allocation_probe::active) ++allocation_probe::calls;
  void* p=nullptr;
  const auto alignment=std::max(static_cast<std::size_t>(a),sizeof(void*));
  if (::posix_memalign(&p,alignment,n?n:1)) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p,std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p,std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------- arguments

struct Args {
  std::string verb;
  std::map<std::string, std::string> opt;
  std::vector<std::pair<std::string, std::string>> meta;
  std::string command_line;

  std::string get(const std::string& k, const std::string& def = "") const {
    const auto it = opt.find(k);
    return it == opt.end() ? def : it->second;
  }
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 0; i < argc; ++i)
    a.command_line += (i ? " " : "") + std::string(argv[i]);
  if (argc > 1) a.verb = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    const std::string v = argv[i + 1];
    if (k.rfind("--", 0) != 0) {
      std::fprintf(stderr, "expected an option, got %s\n", k.c_str());
      std::exit(2);
    }
    k = k.substr(2);
    if (k == "meta") {
      const auto eq = v.find('=');
      a.meta.emplace_back(v.substr(0, eq),
                          eq == std::string::npos ? "" : v.substr(eq + 1));
    } else {
      a.opt[k] = v;
    }
  }
  return a;
}

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "book_bench: %s\n", msg.c_str());
  std::exit(2);
}

msim::Workload load(const std::string& path) {
  if (path.empty()) die("--stream is required");
  std::ifstream in(path, std::ios::binary);
  if (!in) die("cannot open " + path);
  std::string err;
  auto w = msim::read_stream(in, &err);
  if (!w) die(path + ": " + err);
  return std::move(*w);
}

std::string hex(std::uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}

// --------------------------------------------------------------------- JSON

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += c;
    }
  }
  return o + "\"";
}

// Nested scopes own comma placement; strings are escaped before emission.
class Json {
 public:
  Json& open(const char* key = nullptr, char brace = '{') {
    sep(key);
    os_ << brace;
    first_.push_back(true);
    return *this;
  }
  Json& close(char brace = '}') {
    os_ << brace;
    first_.pop_back();
    return *this;
  }
  template <typename T>
  Json& num(const char* key, T v) {
    sep(key);
    os_ << v;
    return *this;
  }
  Json& str(const char* key, const std::string& v) {
    sep(key);
    os_ << quote(v);
    return *this;
  }
  std::string text() const { return os_.str(); }

 private:
  void sep(const char* key) {
    if (!first_.empty()) {
      if (!first_.back()) os_ << ',';
      first_.back() = false;
    }
    if (key) os_ << quote(key) << ':';
  }
  std::ostringstream os_;
  std::vector<bool> first_;
};

// ------------------------------------------------------------ clock, system

struct ClockInfo {
  double overhead_ns = 0;          // mean cost of one now_ns() call
  std::int64_t resolution_ns = 0;  // smallest nonzero step observed
};

ClockInfo calibrate_clock() {
  constexpr int kReads = 2'000'000;
  const std::int64_t first = now_ns();
  std::int64_t prev = first, res = INT64_MAX;
  for (int i = 0; i < kReads; ++i) {
    const std::int64_t t = now_ns();
    if (t > prev) res = std::min(res, t - prev);
    prev = t;
  }
  return {static_cast<double>(prev - first) / kReads, res};
}

// Requests macOS QoS as a scheduling hint, without pinning a core. Success
// records the accepted request, not the core type or achieved frequency.
std::string request_fast_core() {
#if defined(__APPLE__)
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0
             ? "macos qos user-interactive"
             : "macos qos request failed";
#else
  return "none";
#endif
}

std::uint64_t max_rss_bytes() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(ru.ru_maxrss);  // bytes on macOS
#else
  return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;  // KiB on Linux
#endif
}

// ------------------------------------------------------------------- memory

// requested -> strategy -> upstream -> heap. `requested` sees what the book
// asks for and gives back. `upstream` sees what the strategy keeps.
struct MemoryStack {
  msim::TallyResource upstream{std::pmr::new_delete_resource()};
  std::unique_ptr<std::pmr::memory_resource> strategy;
  std::unique_ptr<msim::TallyResource> requested;

  explicit MemoryStack(const std::string& kind) {
    if (kind == "monotonic")
      strategy =
          std::make_unique<std::pmr::monotonic_buffer_resource>(&upstream);
    else if (kind == "pool")
      strategy =
          std::make_unique<std::pmr::unsynchronized_pool_resource>(&upstream);
    else if (kind != "heap")
      die("unknown --memory " + kind);
    requested = std::make_unique<msim::TallyResource>(
        strategy ? strategy.get()
                 : static_cast<std::pmr::memory_resource*>(&upstream));
  }
  std::pmr::memory_resource* resource() { return requested.get(); }
};

// One book and the memory under it, built the same way for every rep.
struct Subject {
  Subject()=default;
  Subject(Subject&&)=default;
  Subject& operator=(Subject&& other) noexcept {
    if (this!=&other) {
      // Destroy the old allocator client before its memory resource.
      engine.reset();reference.reset();mem.reset();
      mem=std::move(other.mem);engine=std::move(other.engine);reference=std::move(other.reference);
    }
    return *this;
  }
  std::unique_ptr<MemoryStack> mem;
  std::unique_ptr<msim::OrderBook> engine;
  std::unique_ptr<msim::ReferenceBook> reference;
};

Subject make_subject(const std::string& book, const std::string& memory,
                     const msim::BookLimits& limits) {
  Subject s;
  if (book == "engine") {
    s.mem = std::make_unique<MemoryStack>(memory);
    s.engine =
        std::make_unique<msim::OrderBook>("bench", s.mem->resource(), limits);
  } else if (book == "reference") {
    s.reference = std::make_unique<msim::ReferenceBook>(limits);
  } else {
    die("unknown --book " + book);
  }
  return s;
}

template <typename F>
void with_book(Subject& s, F f) {
  if (s.engine)
    f(*s.engine);
  else
    f(*s.reference);
}

// -------------------------------------------------------------- timed loops

template <typename Book>
double replay(Book& book, const std::vector<msim::Command>& cmds, std::size_t output_bound, const msim::BookSnapshot& expected, const std::vector<msim::Execution>& last_output) {
  std::vector<msim::Execution> out;
  out.reserve(output_bound);
  const auto original_capacity = out.capacity();
  allocation_probe::active=true;
  const auto t0 = Clock::now();
  for (const msim::Command& c : cmds) (void)msim::apply(book, c, out);
  const auto end = Clock::now();
  allocation_probe::active=false;
  if (!(book.snapshot()==expected) || out!=last_output) die("timed final canonical state/output differs from independent reference");
  if (out.capacity() != original_capacity) die("execution output grew during replay");
  return std::chrono::duration<double, std::milli>(end - t0).count();
}

// What one command turned out to be, for service time by kind.
enum Kind : std::uint8_t { Rest, Take, CancelHit, CancelMiss, Reject, kKinds };
constexpr const char* kKindNames[kKinds] = {"rest", "take", "cancel_hit",
                                            "cancel_miss", "reject"};

Kind kind_of(const msim::Outcome& o, const std::vector<msim::Execution>& out) {
  if (o.type == msim::CommandType::Cancel)
    return o.canceled ? CancelHit : CancelMiss;
  if (o.add.status != msim::AddStatus::Accepted) return Reject;
  return out.empty() ? Rest : Take;
}

// stamps[i] is read just before command i; stamps[n] after the last one.
// One clock read per boundary keeps the probe effect to a single call. The
// kind of each command is a one-byte store, kept for every arm alike.
template <typename Book>
void replay_stamped(Book& book, const std::vector<msim::Command>& cmds,
                    std::vector<std::int64_t>& stamps,
                    std::vector<std::uint8_t>& kinds, std::size_t output_bound, const msim::BookSnapshot& expected, const std::vector<msim::Execution>& last_output) {
  std::vector<msim::Execution> out;
  out.reserve(output_bound);
  const auto original_capacity = out.capacity();
  stamps.resize(cmds.size() + 1);
  kinds.resize(cmds.size());
  allocation_probe::active=true;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    stamps[i] = now_ns();
    kinds[i] = kind_of(msim::apply(book, cmds[i], out), out);
  }
  stamps[cmds.size()] = now_ns();
  allocation_probe::active=false;
  if (!(book.snapshot()==expected) || out!=last_output) die("timed final canonical state/output differs from independent reference");
  if (out.capacity() != original_capacity) die("execution output grew during stamped replay");
}

// Two timestamps per batch; no per-command classification/store. Batch
// averages cannot identify individual service-time tails.
template<class Book>
double replay_batched(Book& book,const std::vector<msim::Command>& cmds,
                      std::size_t bound,std::size_t batch,std::vector<double>& samples,const msim::BookSnapshot& expected,const std::vector<msim::Execution>& last_output) {
  std::vector<msim::Execution> out;out.reserve(bound);
  samples.reserve((cmds.size()+batch-1)/batch);
  allocation_probe::active=true;
  const auto original_capacity=out.capacity();
  const auto start=Clock::now();
  for (std::size_t first=0;first<cmds.size();first+=batch) {
    const auto last=std::min(first+batch,cmds.size());
    const auto before=now_ns();
    for (std::size_t i=first;i<last;++i) (void)msim::apply(book,cmds[i],out);
    const auto after=now_ns();
    samples.push_back(static_cast<double>(after-before)/static_cast<double>(last-first));
  }
  const auto end=Clock::now();allocation_probe::active=false;
  if (!(book.snapshot()==expected) || out!=last_output) die("batch final canonical state/output differs from independent reference");
  if (out.capacity()!=original_capacity) die("execution output grew during batch replay");
  return std::chrono::duration<double,std::milli>(end-start).count();
}

// ---------------------------------------------------------- untimed pass

struct Counts {
  std::uint64_t adds = 0, cancels = 0, cancel_hits = 0, executions = 0;
  std::uint64_t executed_qty = 0, rested_qty = 0;
  std::uint64_t status[6] = {};
  std::uint64_t digest = 0;
  std::size_t output_capacity = 0, output_growths = 0;
};

struct MemSample {
  std::size_t command;
  std::size_t requested_live;
  std::size_t upstream_live;
};

template <typename Book>
Counts observe(Book& book, const std::vector<msim::Command>& cmds,
               MemoryStack* mem, std::vector<MemSample>* samples, std::size_t output_bound) {
  Counts k;
  msim::Digest d;
  std::vector<msim::Execution> out;
  out.reserve(output_bound);
  k.output_capacity = out.capacity();
  const std::size_t every = std::max<std::size_t>(cmds.size() / 256, 1);
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const msim::Outcome o = msim::apply(book, cmds[i], out);
    if (out.capacity() != k.output_capacity) { ++k.output_growths; k.output_capacity = out.capacity(); }
    if (cmds[i].type == msim::CommandType::Cancel) {
      ++k.cancels;
      k.cancel_hits += o.canceled ? 1 : 0;
      d.mix(o.canceled ? 1 : 0);
    } else {
      ++k.adds;
      ++k.status[static_cast<int>(o.add.status)];
      if (static_cast<std::uint64_t>(o.add.rested) > UINT64_MAX - k.rested_qty) die("rested quantity counter overflow");
      k.rested_qty += static_cast<std::uint64_t>(o.add.rested);
      d.mix(static_cast<std::uint64_t>(o.add.status) << 56 |
            static_cast<std::uint64_t>(o.add.filled));
    }
    for (const msim::Execution& x : out) {
      ++k.executions;
      if (static_cast<std::uint64_t>(x.qty) > UINT64_MAX - k.executed_qty) die("execution quantity counter overflow");
      k.executed_qty += static_cast<std::uint64_t>(x.qty);
      d.mix(x.resting_id);
      d.mix(x.incoming_id);
      d.mix(static_cast<std::uint64_t>(x.tick));
      d.mix(static_cast<std::uint64_t>(x.qty));
    }
    if (mem && samples && (i % every == 0 || i + 1 == cmds.size()))
      samples->push_back({i + 1, mem->requested->live(), mem->upstream.live()});
  }
  k.digest = d.value();
  return k;
}

// ---------------------------------------------------------------- statistics

struct Dist {
  std::size_t n = 0;
  double mean = 0;
  double p50=0,p90=0,p99=0,p999=0,p9999=0,max=0;
};

template<class T> Dist distribution(std::vector<T> v) {
  Dist d;
  d.n = v.size();
  if (v.empty()) return d;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (auto x : v) sum += static_cast<double>(x);
  d.mean = sum / static_cast<double>(v.size());
  const auto q = [&](double p) {
    const auto i =
        static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return static_cast<double>(v[i]);
  };
  d.p50 = q(0.50);
  d.p90 = q(0.90);
  d.p99 = q(0.99);
  d.p999 = q(0.999);
  d.p9999 = q(0.9999);
  d.max = static_cast<double>(v.back());
  return d;
}

void put(Json& j, const char* key, const Dist& d) {
  j.open(key)
      .num("n", d.n)
      .num("mean", d.mean)
      .num("p50", d.p50)
      .num("p90", d.p90)
      .num("p99", d.p99)
      .num("p99_9", d.p999)
      .num("p99_99", d.p9999)
      .num("max", d.max)
      .close();
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

// ---------------------------------------------------------------- commands

int cmd_generate(const Args& a) {
  const std::string name = a.get("workload");
  const auto seed = std::stoull(a.get("seed", "1"));
  const auto n = std::stoull(a.get("commands", "1000000"));
  const std::string out = a.get("out");
  if (out.empty()) die("--out is required");

  const auto t0 = Clock::now();
  const auto w = msim::generate_workload(name, seed, n);
  const double gen_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (!w) die("unknown --workload " + name);

  std::ofstream f(out, std::ios::binary);
  if (!f || !msim::write_stream(f, *w)) die("cannot write " + out);
  std::printf("%s seed=%llu commands=%zu digest=%s generate_ms=%.1f -> %s\n",
              name.c_str(), static_cast<unsigned long long>(seed),
              w->commands.size(), hex(msim::stream_digest(*w)).c_str(), gen_ms,
              out.c_str());
  return 0;
}

int cmd_shape(const Args& a) {
  const auto w = load(a.get("stream"));
  const auto s = msim::measure_shape(w);
  std::printf(
      "%s seed=%llu commands=%zu adds=%zu cancels=%zu cancel_hits=%zu "
      "rejected=%zu executions=%zu takers=%zu multi_level_takers=%zu "
      "max_levels_crossed=%zu mean_resting_orders=%.1f "
      "mean_levels_per_side=%.1f max_levels_per_side=%zu "
      "best_price_changes=%zu level_creations=%zu level_removals=%zu\n",
      w.name.c_str(), static_cast<unsigned long long>(w.seed), s.commands,
      s.adds, s.cancels, s.cancel_hits, s.rejected, s.executions, s.takers,
      s.multi_level_takers, s.max_levels_crossed, s.mean_resting_orders,
      s.mean_levels_per_side, s.max_levels_per_side, s.best_price_changes,
      s.level_creations, s.level_removals);
  return 0;
}

int cmd_verify(const Args& a) {
  const auto w = load(a.get("stream"));
  msim::CompareOptions opt;
  opt.snapshot_every = 10'000;
  opt.check_invariants = false;  // Full storage diagnostics can be quadratic.
  const std::string book = "engine";
  const auto t0 = Clock::now();
  const std::optional<msim::Divergence> d =
      msim::engine_vs_reference(w.commands, w.limits, opt);
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  if (d) {
    std::printf("%s: %s DIVERGED at command %zu: %s\n", w.name.c_str(),
                book.c_str(), d->index, d->what.c_str());
    return 1;
  }
  std::printf("%s: %s and reference agree on all %zu commands (%.1f s)\n",
              w.name.c_str(), book.c_str(), w.commands.size(), s);
  return 0;
}

int cmd_run(const Args& a) {
  const std::string stream_path = a.get("stream");
  const std::string book = a.get("book", "engine");
  const std::string memory = a.get("memory", "monotonic");
  const std::string mode = a.get("mode", "throughput");
  const int reps = std::stoi(a.get("reps", "5"));
  const int warmup = std::stoi(a.get("warmup", "1"));
  if (reps <= 0 || warmup < 0) die("reps must be positive and warmup nonnegative");
  if (mode != "throughput" && mode != "latency" && mode != "batch") die("unknown --mode " + mode);

  const auto batch_size=std::stoull(a.get("batch","16"));
  const auto minimum_ms=std::stod(a.get("minimum-ms","0"));
  if (!batch_size || batch_size>1000000 || minimum_ms<0 || minimum_ms>60000)
    die("invalid batch size or minimum duration");
  const std::string qos = request_fast_core();
  const ClockInfo clock = calibrate_clock();
  const auto w = load(stream_path);
  if (w.commands.empty()) die("cannot benchmark an empty stream");

  // Consume each timed subject after its timer, against an independent
  // final state/output. This prevents an unobserved dead replay under LTO.
  msim::ReferenceBook verifier(w.limits);
  std::vector<msim::Execution> last_output;last_output.reserve(w.limits.max_resting_orders);
  for (const auto& command:w.commands) (void)msim::apply(verifier,command,last_output);
  const auto expected_state=verifier.snapshot();

  std::vector<double> setup_ms, run_ms;
  std::vector<std::int64_t> per_cmd;
  std::vector<double> batch_samples;
  std::vector<std::size_t> replay_counts;
  std::uint64_t measured_allocations=0;
  std::map<int, std::vector<std::int64_t>> per_phase;
  std::vector<std::int64_t> per_kind[kKinds];


  for (int r = 0; r < warmup + reps; ++r) {
    const auto t0 = Clock::now();
    Subject s = make_subject(book, memory, w.limits);
    double setup =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    const auto allocations_before_run=allocation_probe::calls;
    if (mode == "throughput") {
      double accumulated=0;std::size_t count=0;
      do {
        double ms=0;
        with_book(s,[&](auto& b) { ms=replay(b,w.commands,w.limits.max_resting_orders,expected_state,last_output); });
        accumulated+=ms;++count;
        if (accumulated<minimum_ms) {
          const auto before_setup=Clock::now();s=make_subject(book,memory,w.limits);
          setup+=std::chrono::duration<double,std::milli>(Clock::now()-before_setup).count();
        }
      } while (accumulated<minimum_ms);
      if (r>=warmup) { setup_ms.push_back(setup);run_ms.push_back(accumulated);replay_counts.push_back(count); }
    } else if (mode=="batch") {
      std::vector<double> samples;
      double ms=0;
      with_book(s,[&](auto& b) { ms=replay_batched(b,w.commands,w.limits.max_resting_orders,static_cast<std::size_t>(batch_size),samples,expected_state,last_output); });
      if (r>=warmup) { setup_ms.push_back(setup);run_ms.push_back(ms);replay_counts.push_back(1);batch_samples.insert(batch_samples.end(),samples.begin(),samples.end()); }
    } else {
      std::vector<std::int64_t> st;std::vector<std::uint8_t> kinds;
      with_book(s,[&](auto& b) { replay_stamped(b,w.commands,st,kinds,w.limits.max_resting_orders,expected_state,last_output); });
      if (r>=warmup) {
        setup_ms.push_back(setup);run_ms.push_back(static_cast<double>(st.back()-st.front())/1e6);replay_counts.push_back(1);
        for (std::size_t i=0;i+1<st.size();++i) {
          const auto d=st[i+1]-st[i];per_cmd.push_back(d);per_kind[kinds[i]].push_back(d);
          if (!w.phase.empty()) per_phase[w.phase[i]].push_back(d);
        }
      }
    }
    const auto allocated=allocation_probe::calls-allocations_before_run;
    if (book=="engine" && allocated) die("C++ allocation on measured matching path");
    if (r>=warmup) measured_allocations+=allocated;
  }

  // Untimed pass on a fresh book: what the stream did, and memory over time.
  Subject s = make_subject(book, memory, w.limits);
  std::vector<MemSample> samples;
  Counts k;
  const auto allocations_before = s.mem ? s.mem->requested->allocations() : 0;
  with_book(
      s, [&](auto& b) { k = observe(b, w.commands, s.mem.get(), &samples, w.limits.max_resting_orders); });

  const auto allocation_delta = s.mem ? s.mem->requested->allocations() - allocations_before : 0;
  if (allocation_delta || k.output_growths) die("benchmark replay allocated internal or execution storage");
  std::vector<double> rate;
  for (std::size_t i=0;i<run_ms.size();++i)
    rate.push_back(static_cast<double>(w.commands.size())*static_cast<double>(replay_counts[i])/(run_ms[i]/1e3));
  const double med_rate = median(rate);

  Json j;
  j.open()
      .str("tool", "book_bench")
      .num("schema", 1)
      .str("command", a.command_line);
  j.open("meta");
  for (const auto& [key, v] : a.meta) j.str(key.c_str(), v);
  j.close();
  j.open("build")
      .str("type", MSIM_BUILD_TYPE)
      .str("compiler", MSIM_CXX_COMPILER)
      .str("flags", MSIM_CXX_FLAGS)
      .str("ipo", MSIM_IPO)
      .str("sanitize", MSIM_SANITIZE_LIST)
      .str("system", MSIM_SYSTEM)
      .close();
  j.open("clock")
      .num("overhead_ns", clock.overhead_ns)
      .num("resolution_ns", clock.resolution_ns)
      .close();
  j.str("scheduling", qos);
  j.open("stream")
      .str("path", stream_path)
      .str("workload", w.name)
      .num("seed", w.seed)
      .num("commands", w.commands.size())
      .str("digest", hex(msim::stream_digest(w)))
      .num("max_resting_orders", w.limits.max_resting_orders)
      .num("max_levels_per_side", w.limits.max_levels_per_side)
      .num("max_tick", w.limits.max_tick)
      .close();
  j.str("book", book).str("memory", s.mem ? memory : "std");
  j.str("mode", mode).num("warmup", warmup);
  j.open("reps", '[');
  for (std::size_t i = 0; i < run_ms.size(); ++i)
    j.open()
        .num("setup_ms", setup_ms[i])
        .num("run_ms", run_ms[i])
        .num("fresh_replays",replay_counts[i])
        .num("commands_per_sec", rate[i])
        .close();
  j.close(']');
  j.open("summary")
      .num("median_commands_per_sec", med_rate)
      .num("min_commands_per_sec", *std::min_element(rate.begin(), rate.end()))
      .num("max_commands_per_sec", *std::max_element(rate.begin(), rate.end()))
      .num("median_ns_per_command", med_rate > 0 ? 1e9 / med_rate : 0)
      .close();
  if (mode == "latency") {
    j.open("latency_ns");
    put(j, "per_command", distribution(per_cmd));

    j.open("kinds");
    for (int kk = 0; kk < kKinds; ++kk)
      if (!per_kind[kk].empty())
        put(j, kKindNames[kk], distribution(std::move(per_kind[kk])));
    j.close();
    if (!per_phase.empty()) {
      j.open("phases");
      for (auto& [p, v] : per_phase)
        put(j, std::to_string(p).c_str(), distribution(std::move(v)));
      j.close();
    }
    j.close();
  }
  if (mode=="batch") { j.num("batch_size",batch_size);put(j,"batch_mean_ns",distribution(batch_samples)); }
  j.num("measured_cpp_new_calls",measured_allocations);
  j.open("outcomes")
      .num("adds", k.adds)
      .num("cancels", k.cancels)
      .num("cancel_hits", k.cancel_hits)
      .num("accepted", k.status[0])
      .num("invalid_qty", k.status[1])
      .num("invalid_tick", k.status[2])
      .num("duplicate_id", k.status[3])
      .num("capacity_full", k.status[4])
      .num("executions", k.executions)
      .num("executed_qty", k.executed_qty)
      .num("rested_qty", k.rested_qty)
      .num("execution_output_capacity", k.output_capacity)
      .num("execution_output_growths", k.output_growths)
      .num("internal_allocations_during_replay", allocation_delta)
      .str("execution_digest", hex(k.digest))
      .close();
  if (s.mem) {
    j.open("memory_bytes")
        .num("requested_peak", s.mem->requested->peak())
        .num("requested_total", s.mem->requested->total())
        .num("requested_allocations", s.mem->requested->allocations())
        .num("upstream_peak", s.mem->upstream.peak())
        .num("upstream_total", s.mem->upstream.total())
        .num("upstream_allocations", s.mem->upstream.allocations());
    j.open("samples", '[');
    for (const auto& m : samples) {
      j.open(nullptr, '[')
          .num(nullptr, m.command)
          .num(nullptr, m.requested_live)
          .num(nullptr, m.upstream_live)
          .close(']');
    }
    j.close(']').close();
  }
  j.num("max_rss_bytes", max_rss_bytes());
  j.close();

  std::printf(
      "%-18s %-9s %-9s %-10s %10.0f cmd/s  %6.1f ns/cmd  reps=%zu  "
      "digest=%s\n",
      w.name.c_str(), book.c_str(), s.mem ? memory.c_str() : "std",
      mode.c_str(), med_rate, med_rate > 0 ? 1e9 / med_rate : 0.0,
      run_ms.size(), hex(k.digest).c_str());
  if (mode == "latency") {
    const Dist d = distribution(per_cmd);
    std::printf(
        "  per-command ns: p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f p99.99=%.1f "
        "max=%.1f (clock step %lld ns, read %.1f ns)\n",
        d.p50, d.p90,
        d.p99, d.p999,
        d.p9999, d.max,
        static_cast<long long>(clock.resolution_ns), clock.overhead_ns);
  }

  const std::string json_path = a.get("json");
  if (!json_path.empty()) {
    std::ofstream f(json_path);
    f << j.text() << '\n';
    f.close();
    if (!f) die("cannot write " + json_path);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse(argc, argv);
  if (a.verb == "allocation-control") {
    const auto before=allocation_probe::calls;allocation_probe::active=true;
    void* a1=::operator new(64);void* a2=::operator new(128,std::align_val_t{64});
    void* a3=::operator new(17,std::align_val_t{alignof(std::uint32_t)});
    allocation_probe::active=false;
    const auto detected=allocation_probe::calls-before;
    const bool aligned=reinterpret_cast<std::uintptr_t>(a3)%alignof(std::uint32_t)==0;
    ::operator delete(a1);::operator delete(a2,std::align_val_t{64});
    ::operator delete(a3,std::align_val_t{alignof(std::uint32_t)});
    std::printf("intentional C++ allocations detected=%llu\n",static_cast<unsigned long long>(detected));
    return detected==3 && aligned?0:2;
  }
  if (a.verb == "generate") return cmd_generate(a);
  if (a.verb == "shape") return cmd_shape(a);
  if (a.verb == "verify") return cmd_verify(a);
  if (a.verb == "run") return cmd_run(a);
  std::fprintf(stderr,
               "usage: book_bench generate|shape|verify|run [options]\n"
               "  see the comment at the top of bench/book_bench.cpp\n");
  return 2;
}
