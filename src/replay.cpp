#include "msim/replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <time.h>

#include "msim/buffer_epoch.hpp"
#include "msim/feed_identity.hpp"
#include "msim/feed_reference.hpp"
#include "msim/input_file.hpp"

namespace msim {
namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b-a).count();
}
double cpu_time() noexcept {
  struct timespec t{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
    return -1;
  return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec)*1e-9;
}
class Barrier {
 public:
  explicit Barrier(std::size_t n) : participants_(n) {}
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto generation = generation_;
    if (++waiting_ == participants_) {
      waiting_ = 0; ++generation_; changed_.notify_all();
    } else changed_.wait(lock, [&] { return generation_ != generation; });
  }
 private:
  const std::size_t participants_;
  std::size_t waiting_ = 0, generation_ = 0;
  std::mutex mutex_;
  std::condition_variable changed_;
};
struct Frame { std::size_t offset; std::uint64_t ordinal; };
struct Chunk {
  std::vector<unsigned char> bytes;
  std::vector<std::vector<Frame>> offsets;
  std::size_t size = 0, end = 0;
  std::uint64_t file_offset = 0, first_message = 0, messages = 0;
  bool last = false;
  Chunk(std::size_t capacity, std::size_t workers) : bytes(capacity), offsets(workers) {}
};
struct Coordinator {
  ProtocolState state;
  std::unique_ptr<FeedIdentity> identities;
  std::unique_ptr<FeedHandler<FeedIdentity>> handler;
  std::array<std::uint64_t,256> counts{};
  explicit Coordinator(const ReplayOptions& o)
      : state(ProtocolState::Scope::CompleteSession,o.day_reference_blocks,
              o.experimental_without_identity) {
    if (!o.experimental_without_identity) {
      identities = std::make_unique<FeedIdentity>(o.global_orders,
          o.reference_mode, o.reference_window_bits);
      handler = std::make_unique<FeedHandler<FeedIdentity>>(*identities);
    }
  }
  template <class M> void on(const M& m) {
    state.validate(m);
    if (handler) {
      handler->on(m);
      if (handler->failed())
        throw std::runtime_error(FeedBook::status_name(handler->status));
    }
    state.commit(m); ++counts[static_cast<unsigned char>(m.type)];
  }
  void on_other(const itch::Header& h) { state.validate(h); state.commit(h); ++counts[static_cast<unsigned char>(h.type)]; }
  void context(std::uint64_t offset, std::uint64_t ordinal) {
    state.set_ordinal(ordinal);
    if (handler) handler->set_context(static_cast<std::size_t>(offset), ordinal);
  }
};
void frame_chunk(InputFile& file, Chunk& c, const Chunk* previous,
                 Coordinator& coordinator, const ReplayOptions& o,
                 double& read_s) {
  c.size = 0; c.end = 0; c.messages = 0; c.last = false;
  c.file_offset = previous ? previous->file_offset + previous->end : 0;
  c.first_message = previous ? previous->first_message + previous->messages : 0;
  for (auto& offsets : c.offsets) offsets.clear();
  if (previous) {
    c.size = previous->size - previous->end;
    std::memcpy(c.bytes.data(), previous->bytes.data() + previous->end, c.size);
  }
  auto ensure = [&](std::size_t need) {
    while (c.size < need) {
      if (c.size == c.bytes.size()) throw std::runtime_error("frame exceeds buffer capacity");
      const auto before = Clock::now();
      const auto target = std::max(need,c.end+
          (o.chunk_messages-static_cast<std::size_t>(c.messages))*52);
      const auto n = file.read(c.bytes.data() + c.size,
          std::min<std::size_t>({4U << 20,c.bytes.size()-c.size,target-c.size}));
      read_s += seconds(before, Clock::now());
      if (!n) return false;
      c.size += n;
    }
    return true;
  };
  while (c.messages < o.chunk_messages) {
    if (!ensure(c.end + 2)) {
      if (c.end != c.size) throw ReplayError("truncated length prefix",
          c.first_message+c.messages+1, c.file_offset+c.end,
          coordinator.state.messages(), 0);
      c.last = true; break;
    }
    const auto length = itch::detail::be16(c.bytes.data() + c.end);
    const auto n = c.first_message + c.messages + 1;
    const auto at = c.file_offset + c.end;
    if (length < 11 || !ensure(c.end + 2 + length))
      throw ReplayError("invalid or truncated frame", n, at, coordinator.state.messages(), 0);
    const auto* data = c.bytes.data() + c.end + 2;
    coordinator.context(at, n);
    try {
      const auto status = itch::decode_checked(data, length, coordinator);
      if (status != itch::DecodeStatus::Decoded)
        throw std::runtime_error(itch::decode_status_name(status));
    } catch (const std::exception& e) {
      throw ReplayError(e.what(), n, at, coordinator.state.messages(), 0);
    }
    const auto locate = itch::detail::be16(data+1);
    // Session and market-wide circuit-breaker context reaches EVERY owner.
    if (data[0] == 'S' || data[0] == 'W') {
      for (auto& offsets : c.offsets) offsets.push_back({c.end, n});
    } else {
      c.offsets[locate % o.workers].push_back({c.end, n});
    }
    c.end += 2 + length; ++c.messages;
  }
  if (file.consumed() == file.size() && c.end == c.size) c.last = true;
}
struct Worker {
  FeedBook book;
  MarketHandler<FeedBook> market;
  std::unique_ptr<FeedReference> reference;
  std::unique_ptr<FeedHandler<FeedReference>> reference_handler;
  WorkerProfile profile;
  std::vector<ReplayCross> crosses;
  std::vector<std::uint16_t> locates;
  const ReplayOptions& options;
  std::uint64_t ordinal = 0, offset = 0;
  double callback_s = 0;
  std::exception_ptr exception;
  Worker(FeedBook::Options limits, const ReplayOptions& o)
      : book(limits), market(book, ProtocolState::Scope::BoundedFragment), options(o) {
    if (o.reference) {
      reference = std::make_unique<FeedReference>();
      reference_handler = std::make_unique<FeedHandler<FeedReference>>(*reference);
    }
  }
  template <class M> void on(const M& m) {
    const auto begin = options.instrument_stages ? Clock::now() : Clock::time_point{};
    market.state.validate(m);
    const auto validated = options.instrument_stages ? Clock::now() : Clock::time_point{};
    market.lifecycle.on(m);
    if (market.lifecycle.failed())
      throw std::runtime_error(FeedBook::status_name(market.lifecycle.status));
    const auto applied = options.instrument_stages ? Clock::now() : Clock::time_point{};
    market.state.commit(m);
    if (options.instrument_stages) {
      const auto committed = Clock::now();
      profile.metadata_s += seconds(begin, validated) + seconds(applied, committed);
      profile.mutation_s += seconds(validated, applied);
      callback_s += seconds(begin, committed);
    }
    if (reference_handler) {
      reference_handler->on(m);
      if (reference_handler->failed()) throw std::runtime_error("independent feed reference rejected event");
    }
  }
  void on(const itch::StockDirectory& m) {
    const bool fresh = !market.state.symbol(m.locate).directory;
    generic(m); if (fresh) locates.push_back(m.locate);
  }
  void on(const itch::CrossTrade& m) {
    generic(m);
    if (options.capture_crosses) {
      if (crosses.size() == options.cross_records)
        throw std::runtime_error("cross output capacity exhausted");
      ReplayCross c{ordinal,m.timestamp,m.match,m.shares,m.locate,{},m.price,
                    market.state.symbol(m.locate).category,m.cross_type};
      std::memcpy(c.stock.data(), m.stock, 8); crosses.push_back(c);
    }
  }
  template <class M> void generic(const M& m) { this->template on<M>(m); }
  void on_other(const itch::Header& h) {
    market.state.validate(h); market.state.commit(h);
  }
  void compare_reference() {
    if (!reference) return;
    if (book.orders() != reference->orders()) throw std::runtime_error("reference order count differs");
    for (auto loc : locates) for (Side side : {Side::BUY,Side::SELL}) {
      std::vector<std::tuple<std::uint32_t,std::uint64_t,std::uint32_t>> actual, expected;
      book.for_each(loc,side,[&](auto p, auto r, auto q) { actual.emplace_back(p,r,q); });
      reference->for_each(loc,side,[&](auto p, auto r, auto q) { expected.emplace_back(p,r,q); });
      if (actual != expected) throw std::runtime_error("exact independent feed state differs");
    }
  }
};
bool same_metadata(const ProtocolState::Symbol& a, const ProtocolState::Symbol& b) {
  return a.stock==b.stock && a.named==b.named && a.directory==b.directory &&
      a.category==b.category && a.authenticity==b.authenticity && a.trading==b.trading &&
      a.nasdaq_operation==b.nasdaq_operation && a.round_lot==b.round_lot &&
      a.high_timestamp==b.high_timestamp && a.clock_regressions==b.clock_regressions &&
      a.trading_update==b.trading_update;
}
}  // namespace

ReplayResult replay(const std::string& path, const ReplayOptions& o) {
  if (!o.workers || o.workers > 64 || !o.chunk_messages || o.chunk_messages > 2'000'000 ||
      o.global_orders >= UINT32_MAX || o.max_levels >= UINT32_MAX ||
      (o.reference && o.workers != 1)) throw std::invalid_argument("unsupported replay limits");
  const auto beginning = Clock::now(); const auto cpu_begin = cpu_time();
  InputFile file(path);
  Coordinator coordinator(o);
  const auto capacity = o.chunk_messages * 52 + 65537;
  Chunk chunks[2]{{capacity,o.workers},{capacity,o.workers}};
  const auto per_worker = o.worker_orders ? o.worker_orders : std::clamp<std::size_t>(
      o.global_orders*2/o.workers, std::min<std::size_t>(1'000'000,o.global_orders), o.global_orders);
  std::vector<std::unique_ptr<Worker>> workers;
  for (std::size_t i = 0; i < o.workers; ++i)
    workers.push_back(std::make_unique<Worker>(FeedBook::Options{
        per_worker,o.max_levels,o.reference_mode,o.reference_window_bits},o));
  ReplayResult result;
  result.buffer_bytes = 2*capacity;
  result.experimental_without_identity = o.experimental_without_identity;
  result.instrumented = o.instrument_stages || o.hooks || o.reference;
  auto fill = [&](Chunk& c, const Chunk* previous) {
    const auto before = Clock::now();
    frame_chunk(file,c,previous,coordinator,o,result.read_hash_s);
    result.coordinator_s += seconds(before,Clock::now());
  };
  const auto pipeline_begin = Clock::now();
  const auto pipeline_cpu_begin=cpu_time();
  result.setup_s = seconds(beginning,pipeline_begin);
  fill(chunks[0],nullptr);
  Barrier start(o.workers+1), done(o.workers+1);
  std::mutex launch_mutex; std::condition_variable launch_changed;
  bool launched = false, cancelled = false;
  std::atomic<bool> stop{false}, failed{false};
  std::vector<std::thread> pool; pool.reserve(o.workers);
  try {
    for (std::size_t index = 0; index < o.workers; ++index) {
      if (o.hooks && o.hooks->before_launch) o.hooks->before_launch(index);
      pool.emplace_back([&,index] {
        {
          std::unique_lock<std::mutex> gate(launch_mutex);
          launch_changed.wait(gate,[&] { return launched || cancelled; });
          if (cancelled) return;
        }
        auto& w = *workers[index]; const auto cpu_start = cpu_time();
        std::size_t generation = 0;
        for (;;) {
          auto before = Clock::now(); start.wait();
          w.profile.wait_s += seconds(before,Clock::now());
          if (stop.load(std::memory_order_relaxed)) break;
          const auto& c = chunks[generation % 2]; before = Clock::now();
          try {
            for (const auto frame : c.offsets[index]) {
              if (failed.load(std::memory_order_relaxed)) break;
              w.ordinal = frame.ordinal; w.offset = c.file_offset + frame.offset;
              if (o.reference && w.ordinal > 2000)
                throw std::runtime_error("exact reference route is bounded to 2000 messages");
              if (o.hooks && o.hooks->before_worker_event)
                o.hooks->before_worker_event(index,w.ordinal);
              w.market.set_context(static_cast<std::size_t>(w.offset),w.ordinal);
              const auto* data = c.bytes.data()+frame.offset+2;
              const auto stamp = o.instrument_stages ? Clock::now() : Clock::time_point{};
              const auto previous_callback = w.callback_s;
              const auto status = itch::decode_checked(data,
                  itch::detail::be16(c.bytes.data()+frame.offset),w);
              if (status != itch::DecodeStatus::Decoded)
                throw std::runtime_error(itch::decode_status_name(status));
              if (o.instrument_stages)
                w.profile.decode_s += seconds(stamp,Clock::now()) - (w.callback_s-previous_callback);
              const bool broadcast = data[0]=='S' || data[0]=='W';
              if (!broadcast || index==0) ++w.profile.events;
              if (data[0]=='A' || data[0]=='F' || data[0]=='E' || data[0]=='C' ||
                  data[0]=='X' || data[0]=='D' || data[0]=='U') ++w.profile.lifecycle_events;
              w.compare_reference();
            }
          } catch (...) { w.exception=std::current_exception(); failed.store(true,std::memory_order_relaxed); }
          w.profile.busy_s += seconds(before,Clock::now());
          before=Clock::now(); done.wait();
          w.profile.wait_s += seconds(before,Clock::now()); ++generation;
        }
        const auto cpu_end = cpu_time();
        w.profile.cpu_s = cpu_start < 0 || cpu_end < 0 ? -1 : cpu_end-cpu_start;
      });
    }
  } catch (...) {
    { std::lock_guard<std::mutex> gate(launch_mutex); cancelled=true; }
    launch_changed.notify_all();
    for (auto& thread : pool) thread.join();
    throw;
  }
  { std::lock_guard<std::mutex> gate(launch_mutex); launched=true; }
  launch_changed.notify_all();
  auto shutdown = [&] {
    stop.store(true,std::memory_order_relaxed); start.wait();
    for (auto& thread : pool) thread.join();
  };
  std::exception_ptr producer_error;
  BufferEpoch epochs;
  try {
    for (std::size_t generation=0;;++generation) {
      const auto current=generation%2, next=(generation+1)%2;
      const auto& c=chunks[current];
      epochs.reading(current); start.wait();
      if (!c.last) {
        try { epochs.writing(next); fill(chunks[next],&c); }
        catch (...) { producer_error=std::current_exception(); failed.store(true,std::memory_order_relaxed); }
      }
      done.wait(); epochs.retired(current);
      if (failed.load(std::memory_order_relaxed) || c.last) break;
    }
  } catch (...) { shutdown(); throw; }
  shutdown();
  const auto validation_begin = Clock::now();
  const auto pipeline_cpu_end=cpu_time();
  result.coordinator_pipeline_cpu_s=pipeline_cpu_begin<0||pipeline_cpu_end<0?-1:pipeline_cpu_end-pipeline_cpu_begin;
  result.pipeline_s = seconds(pipeline_begin,validation_begin);
  std::uint64_t applied=0;
  for (const auto& w : workers) applied+=w->profile.events;
  if (producer_error) {
    try { std::rethrow_exception(producer_error); }
    catch (const ReplayError& e) { throw ReplayError(e.what(),e.ordinal,e.offset,e.validated_messages,applied); }
  }
  for (const auto& w : workers) if (w->exception) {
    try { std::rethrow_exception(w->exception); }
    catch (const std::exception& e) { throw ReplayError(e.what(),w->ordinal,w->offset,coordinator.state.messages(),applied); }
  }
  coordinator.state.finish(); file.finish();
  if (applied != coordinator.state.messages()) throw std::logic_error("dispatched-event accounting differs");
  if (coordinator.identities && !coordinator.identities->check_invariants())
    throw std::logic_error("global identity storage invariant");
  for (auto& w : workers) {
    std::string why;
    if (!w->book.check_invariants(&why)) throw std::logic_error("worker storage: "+why);
    if (!w->market.state.complete() || w->market.state.session()!=coordinator.state.session())
      throw std::logic_error("worker session context differs");
    result.live_orders+=w->book.orders(); result.levels+=w->book.levels();
    w->profile.peak_orders=w->book.peak_orders(); w->profile.old_orders=w->book.old_orders();
    result.workers.push_back(w->profile);
    if (w->crosses.size() > o.cross_records-result.crosses.size())
      throw std::runtime_error("cross output capacity exhausted");
    result.crosses.insert(result.crosses.end(),w->crosses.begin(),w->crosses.end());
  }
  if (o.capture_state && result.live_orders > o.output_records)
    throw std::runtime_error("canonical state output capacity exhausted");
  if (o.capture_state && coordinator.state.reference_births() > o.output_records)
    throw std::runtime_error("canonical identity output capacity exhausted");
  if (o.capture_state) result.state.reserve(result.live_orders);
  if (o.capture_state) {
    result.day_references.reserve(static_cast<std::size_t>(coordinator.state.reference_births()));
    coordinator.state.for_each_reference([&](auto ref) { result.day_references.push_back(ref); });
  }
  for (std::size_t loc=1;loc<65536;++loc) {
    const auto locate=static_cast<std::uint16_t>(loc);
    const auto& expected=coordinator.state.symbol(locate);
    auto& owner=*workers[loc%o.workers];
    if (!same_metadata(expected,owner.market.state.symbol(locate)))
      throw std::logic_error("worker metadata differs from ordered coordinator");
    if (expected.directory) result.symbols.push_back({locate,expected});
    for (Side side : {Side::BUY,Side::SELL}) owner.book.for_each(locate,side,
        [&](auto price,auto ref,auto shares) {
          if (coordinator.identities && !coordinator.identities->agrees(ref,locate,shares))
            throw std::logic_error("worker differs from global live identity");
          if (o.capture_state) result.state.emplace_back(locate,side,price,ref,shares);
        });
  }
  if (coordinator.identities && result.live_orders!=coordinator.identities->size())
    throw std::logic_error("global live identity count differs");
  std::sort(result.crosses.begin(),result.crosses.end(),[](const auto& a,const auto& b) {return a.ordinal<b.ordinal;});
  result.bytes=file.size(); result.input_sha256=file.sha256();
  result.messages=coordinator.state.messages(); result.type_counts=coordinator.counts;
  result.terminal_event=coordinator.state.session(); result.clock_regressions=coordinator.state.clock_regressions();
  result.day_reference_blocks=coordinator.state.day_reference_blocks();
  result.reference_births=coordinator.state.reference_births();
  file.finish();  // include the potentially long canonical-finalization interval
  const auto cpu_end=cpu_time();
  result.coordinator_cpu_s=cpu_begin < 0 || cpu_end < 0 ? -1 : cpu_end-cpu_begin;
  const auto finished=Clock::now();
  result.final_validation_s=seconds(validation_begin,finished);
  result.total_s=seconds(beginning,finished);
  return result;
}
}  // namespace msim
