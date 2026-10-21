#include "msim/workload.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <istream>
#include <map>
#include <memory_resource>
#include <ostream>
#include <unordered_map>

#include "msim/digest.hpp"
#include "msim/order_book.hpp"
#include "msim/rng.hpp"

namespace msim {

namespace {

// Builds a stream while replaying it through the engine, so the generator
// can see the book it is shaping: which orders still rest, at which levels,
// and with how much quantity. The engine here is only a viewpoint. Every
// stream is checked later against the reference book.
class Generator {
 public:
  Generator(std::uint64_t seed, BookLimits limits)
      : rng_(seed), limits_(limits), book_("gen", &mr_, limits) {}

  Xoroshiro128Plus& rng() { return rng_; }
  std::size_t live() const { return live_.size(); }
  std::size_t size() const { return cmds_.size(); }

  double u() { return rng_.next_uniform01(); }
  Qty qty(Qty lo, Qty hi) {
    return lo + static_cast<Qty>(
                    rand_index(rng_, static_cast<std::size_t>(hi - lo + 1)));
  }
  Tick offset(Tick lo, Tick hi) { return qty(lo, hi); }

  std::optional<Tick> best(Side s) const {
    return s == Side::BUY ? book_.best_bid() : book_.best_ask();
  }

  // Midpoint of the touch, or a drifting anchor when a side is empty. An
  // anchored generator always uses the anchor, so that its orders stay
  // within a fixed band and the band's width bounds the level count.
  Tick reference() const {
    if (anchored_) return anchor_;
    const auto b = book_.best_bid(), a = book_.best_ask();
    if (b && a) return (*b + *a) / 2;
    if (b) return *b + 1;
    if (a) return *a - 1;
    return anchor_;
  }

  // A resting order `off` ticks behind the reference, never crossing.
  void passive(Side s, Tick off, Qty q) {
    Tick t = s == Side::BUY ? reference() - off : reference() + off;
    const auto other = best(opposite(s));
    if (other)
      t = s == Side::BUY ? std::min(t, *other - 1) : std::max(t, *other + 1);
    add(s, std::clamp<Tick>(t, 1, limits_.max_tick), q);
  }

  // An order at the opposite best price, sized up to `max_q`.
  bool take(Side s, Qty max_q) {
    const auto other = best(opposite(s));
    if (!other) return false;
    add(s, *other, qty(1, max_q));
    return true;
  }

  // Exactly enough quantity, at the right limit, to consume the best `k`
  // levels of the opposite side.
  bool sweep(Side s, std::size_t k) {
    const auto& lv = levels(opposite(s));
    if (lv.empty()) return false;
    Qty total = 0;
    Tick limit = 0;
    std::size_t i = 0;
    const auto visit = [&](const auto& m) {
      for (const auto& [tick, level] : m) {
        if (i++ == k) break;
        total += level.qty;
        limit = tick;
      }
    };
    if (s == Side::BUY)
      visit(asks_);
    else
      visit(bids_);
    add(s, limit, total);
    return true;
  }

  // Consumes the whole best level of side `s` with one incoming order.
  bool clear_best(Side s) { return sweep(opposite(s), 1); }

  bool cancel_random() {
    if (live_.empty()) return false;
    cancel(live_[rand_index(rng_, live_.size())]);
    return true;
  }

  bool cancel_at_best(Side s) {
    const auto b = best(s);
    if (!b) return false;
    const auto& ids = levels(s).at(*b).ids;
    cancel(ids[rand_index(rng_, ids.size())]);
    return true;
  }

  void anchor() { anchored_ = true; }
  void set_phase(std::uint8_t p) { phase_ = p; }
  void enable_phases() { phased_ = true; }

  Workload finish(std::string name, std::uint64_t seed) {
    Workload w;
    w.name = std::move(name);
    w.seed = seed;
    w.limits = limits_;
    w.commands = std::move(cmds_);
    if (phased_) w.phase = std::move(phases_);
    return w;
  }

 private:
  struct Level {
    Qty qty = 0;
    std::vector<OrderId> ids;
  };
  struct Where {
    Side side;
    Tick tick;
    Qty qty;
  };
  using BidLevels = std::map<Tick, Level, std::greater<Tick>>;
  using AskLevels = std::map<Tick, Level, std::less<Tick>>;

  void add(Side s, Tick t, Qty q) {
    const Command c = Command::add(cmds_.size() + 1, next_id_++, s, t, q);
    record(c);
    std::vector<Execution> ex;
    const AddResult r = book_.add(Order{c.id, s, t, q}, ex);
    for (const Execution& x : ex) consume(x.resting_id, x.qty);
    if (r.rested > 0) rest(c.id, s, t, r.rested);
  }

  void cancel(OrderId id) {
    record(Command::cancel(cmds_.size() + 1, id));
    if (book_.cancel(id)) remove(id, where_.at(id).qty);
  }

  void record(const Command& c) {
    cmds_.push_back(c);
    if (phased_) phases_.push_back(phase_);
  }

  void rest(OrderId id, Side s, Tick t, Qty q) {
    where_[id] = Where{s, t, q};
    pos_[id] = live_.size();
    live_.push_back(id);
    Level& lvl = level_ref(s, t);
    lvl.qty += q;
    lvl.ids.push_back(id);
  }

  void consume(OrderId id, Qty q) {
    Where& w = where_.at(id);
    if (w.qty == q) {
      remove(id, q);
      return;
    }
    w.qty -= q;
    level_ref(w.side, w.tick).qty -= q;
  }

  void remove(OrderId id, Qty q) {
    const Where w = where_.at(id);
    Level& lvl = level_ref(w.side, w.tick);
    lvl.qty -= q;
    lvl.ids.erase(std::find(lvl.ids.begin(), lvl.ids.end(), id));
    if (lvl.ids.empty()) {
      if (w.side == Side::BUY)
        bids_.erase(w.tick);
      else
        asks_.erase(w.tick);
    }
    const std::size_t p = pos_.at(id);
    live_[p] = live_.back();
    pos_[live_[p]] = p;
    live_.pop_back();
    pos_.erase(id);
    where_.erase(id);
  }

  Level& level_ref(Side s, Tick t) {
    return s == Side::BUY ? bids_[t] : asks_[t];
  }

 public:
  // Best-first view of one side's levels, for sweep sizing.
  struct SideView {
    const BidLevels* bids = nullptr;
    const AskLevels* asks = nullptr;
    bool empty() const { return bids ? bids->empty() : asks->empty(); }
    const Level& at(Tick t) const { return bids ? bids->at(t) : asks->at(t); }
  };
  SideView levels(Side s) const {
    SideView v;
    if (s == Side::BUY)
      v.bids = &bids_;
    else
      v.asks = &asks_;
    return v;
  }

 private:
  Xoroshiro128Plus rng_;
  BookLimits limits_;
  std::pmr::unsynchronized_pool_resource mr_;
  OrderBook book_;

  std::vector<Command> cmds_;
  std::vector<std::uint8_t> phases_;
  std::uint8_t phase_ = 0;
  bool phased_ = false;
  bool anchored_ = false;
  OrderId next_id_ = 1;
  Tick anchor_ = 100'000;

  std::vector<OrderId> live_;
  std::unordered_map<OrderId, std::size_t> pos_;
  std::unordered_map<OrderId, Where> where_;
  BidLevels bids_;
  AskLevels asks_;
};

Side coin(Generator& g) { return g.u() < 0.5 ? Side::BUY : Side::SELL; }

// Roughly geometric distance from the touch: most orders join near it.
Tick near_offset(Generator& g, double mean, Tick cap) {
  const auto x = static_cast<Tick>(-std::log(1.0 - g.u()) * mean);
  return std::min<Tick>(1 + x, cap);
}

// Steady depth of about 900 orders over about 20 levels per side, near the
// touch. Passive adds, cancels of random resting orders, and small orders
// that take from the best level but rarely empty it.
void stable_depth(Generator& g, std::size_t n, Tick) {
  while (g.size() < n) {
    const double r = g.u();
    if (g.live() < 900 || r < 0.45)
      g.passive(coin(g), near_offset(g, 4.0, 50), g.qty(1, 100));
    else if (r < 0.85)
      g.cancel_random();
    else if (!g.take(coin(g), 60))
      g.passive(coin(g), 1, g.qty(1, 100));
  }
}

// About 2,000 resting orders spread 5 to 300 ticks from the touch, with half
// of all commands cancelling one of them. Levels are sparse, so most adds
// create a level and most cancels destroy one.
void cancel_churn(Generator& g, std::size_t n, Tick) {
  while (g.size() < n) {
    const double r = g.u();
    if (g.live() < 1800 || (r >= 0.50 && r < 0.95))
      g.passive(coin(g), g.offset(5, 300), g.qty(1, 100));
    else if (r < 0.50)
      g.cancel_random();
    else if (!g.take(coin(g), 30))
      g.passive(coin(g), 5, g.qty(1, 100));
  }
}

// About 1,000 resting orders placed 1 to 100 ticks from the touch. Every
// 40th command is one order sized to consume exactly 5 to 50 levels of the
// opposite side. Each sweep moves the touch, so later adds land around the
// new price and the book spreads over a few hundred levels per side.
void multi_level_sweep(Generator& g, std::size_t n, Tick) {
  while (g.size() < n) {
    if (g.size() % 40 == 39 && g.live() > 400) {
      const std::size_t k = 5 + rand_index(g.rng(), 46);
      if (g.sweep(coin(g), k)) continue;
    }
    if (g.live() < 1200 || g.u() < 0.6)
      g.passive(coin(g), g.offset(1, 100), g.qty(1, 50));
    else
      g.cancel_random();
  }
}

// About 3,000 orders spread 1 to 1,200 ticks from the touch, so each side
// holds several hundred levels and sometimes more than a thousand. A quarter
// of commands cancel at the best price and a sixth consume the whole best
// level. Best-price removals pop the ladder's end; newly opened levels can
// still move a suffix, depending on their distance from the best end.
void many_prices(Generator& g, std::size_t n, Tick width) {
  while (g.size() < n) {
    const double r = g.u();
    if (g.live() < 3000 || r < 0.45)
      g.passive(coin(g), g.offset(1, width), g.qty(1, 100));
    else if (r < 0.70)
      g.cancel_at_best(coin(g));
    else if (r < 0.85)
      g.cancel_random();
    else
      g.clear_best(coin(g));
  }
}

// 2,000 quiet commands, then 400 burst commands, repeated. Quiet commands
// add and cancel away from the touch. Burst commands take liquidity, join
// the best price, and cancel at it. Every command carries its phase tag.
void bursty(Generator& g, std::size_t n, Tick) {
  g.enable_phases();
  while (g.size() < n) {
    const bool burst = g.size() % 2400 >= 2000;
    g.set_phase(burst ? 1 : 0);
    const double r = g.u();
    if (g.live() < 900) {
      g.passive(coin(g), g.offset(5, 50), g.qty(1, 100));
    } else if (!burst) {
      if (r < 0.5)
        g.passive(coin(g), g.offset(5, 50), g.qty(1, 100));
      else
        g.cancel_random();
    } else if (r < 0.60) {
      if (!g.take(coin(g), 200)) g.passive(coin(g), 1, g.qty(1, 100));
    } else if (r < 0.85) {
      g.passive(coin(g), g.offset(1, 2), g.qty(1, 100));
    } else if (!g.cancel_at_best(coin(g))) {
      g.cancel_random();
    }
  }
}

// The many_prices command mix, with every order placed 1 to `width` ticks
// either side of a fixed center. The touch cannot wander away from the band,
// so `width` bounds the levels per side. Vary it to separate price depth
// from the command mix: "price_band/width=100".
void price_band(Generator& g, std::size_t n, Tick width) {
  g.anchor();
  many_prices(g, n, width);
}

using Builder = void (*)(Generator&, std::size_t, Tick);

struct Entry {
  const char* name;
  Builder build;
  Tick default_width;  // 0: the workload takes no width
};

constexpr Entry kWorkloads[] = {
    {"stable_depth", stable_depth, 0},
    {"cancel_churn", cancel_churn, 0},
    {"multi_level_sweep", multi_level_sweep, 0},
    {"many_prices", many_prices, 0},
    {"bursty", bursty, 0},
    {"price_band", price_band, 300},
};

void put_u64(std::ostream& os, std::uint64_t v) {
  char b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  os.write(b, 8);
}
void put_u32(std::ostream& os, std::uint32_t v) {
  char b[4];
  for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  os.write(b, 4);
}
void put_u16(std::ostream& os, std::uint16_t v) {
  const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>(v >> 8)};
  os.write(b, 2);
}

bool get(std::istream& is, void* p, std::size_t n) {
  is.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(is.gcount()) == n;
}
template <typename T>
bool get_le(std::istream& is, T& v) {
  unsigned char b[sizeof(T)];
  if (!get(is, b, sizeof(T))) return false;
  std::uint64_t x = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    x |= static_cast<std::uint64_t>(b[i]) << (8 * i);
  v = static_cast<T>(x);
  return true;
}

constexpr char kMagic[8] = {'M', 'S', 'I', 'M', 'C', 'M', 'D', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kRecordBytes = 32;

}  // namespace

const std::vector<std::string>& workload_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (const Entry& e : kWorkloads) v.emplace_back(e.name);
    return v;
  }();
  return names;
}

std::optional<Workload> generate_workload(const std::string& name,
                                          std::uint64_t seed, std::size_t n) {
  // "base" or "base/width=W"
  const auto slash = name.find('/');
  const std::string base = name.substr(0, slash);
  Tick width = 0;
  if (slash != std::string::npos) {
    const std::string param = name.substr(slash + 1);
    if (param.rfind("width=", 0) != 0) return std::nullopt;
    try {
      width = std::stoll(param.substr(6));
    } catch (const std::exception&) {
      return std::nullopt;
    }
    if (width < 1 || width > 100'000) return std::nullopt;
  }
  for (const Entry& e : kWorkloads) {
    if (base != e.name) continue;
    if (width && !e.default_width) return std::nullopt;
    Generator g(seed, BookLimits{});
    // many_prices takes no width parameter; 1,200 is part of its definition.
    const Tick band =
        width ? width : (e.default_width ? e.default_width : 1200);
    e.build(g, n, band);
    Workload w = g.finish(name, seed);
    // Builders can emit one command past n in their last step.
    w.commands.resize(std::min(w.commands.size(), n));
    if (!w.phase.empty()) w.phase.resize(w.commands.size());
    return w;
  }
  return std::nullopt;
}

WorkloadShape measure_shape(const Workload& w) {
  std::pmr::unsynchronized_pool_resource mr;
  OrderBook book("shape", &mr, w.limits);
  std::vector<Execution> out;
  WorkloadShape s;
  double orders = 0, levels = 0;
  auto bb = book.best_bid();
  auto ba = book.best_ask();
  for (const Command& c : w.commands) {
    const std::size_t lb = book.levels(Side::BUY), ls = book.levels(Side::SELL);
    const Outcome o = apply(book, c, out);
    // A command removes levels from at most one side and creates at most one
    // level on its own side, so before/after counts give both exactly.
    const std::size_t nb = book.levels(Side::BUY), ns = book.levels(Side::SELL);
    if (c.type == CommandType::Cancel) {
      s.level_removals += (lb - nb) + (ls - ns);
    } else if (o.add.status == AddStatus::Accepted) {
      const bool buy = c.side == Side::BUY;
      const std::size_t own_before = buy ? lb : ls, own_after = buy ? nb : ns;
      const std::size_t opp_before = buy ? ls : lb, opp_after = buy ? ns : nb;
      s.level_removals += opp_before - opp_after;
      s.level_creations += own_after - own_before;
    }
    ++s.commands;
    if (c.type == CommandType::Cancel) {
      ++s.cancels;
      s.cancel_hits += o.canceled ? 1 : 0;
    } else {
      ++s.adds;
      if (o.add.status != AddStatus::Accepted) ++s.rejected;
      s.executions += out.size();
      std::size_t crossed = 0;
      for (std::size_t i = 0; i < out.size(); ++i)
        crossed += i == 0 || out[i].tick != out[i - 1].tick;
      s.takers += !out.empty();
      s.multi_level_takers += crossed >= 5;
      s.max_levels_crossed = std::max(s.max_levels_crossed, crossed);
    }
    orders += static_cast<double>(book.resting_orders());
    const std::size_t lv = book.levels(Side::BUY) + book.levels(Side::SELL);
    levels += static_cast<double>(lv) / 2.0;
    s.max_levels_per_side =
        std::max({s.max_levels_per_side, book.levels(Side::BUY),
                  book.levels(Side::SELL)});
    const auto new_bid = book.best_bid(), new_ask = book.best_ask();
    s.best_price_changes += static_cast<std::size_t>(new_bid != bb) +
                            static_cast<std::size_t>(new_ask != ba);
    bb = new_bid;
    ba = new_ask;
  }
  if (s.commands > 0) {
    s.mean_resting_orders = orders / static_cast<double>(s.commands);
    s.mean_levels_per_side = levels / static_cast<double>(s.commands);
  }
  return s;
}

std::uint64_t stream_digest(const Workload& w) {
  Digest d;
  d.mix(w.commands.size());
  for (std::size_t i = 0; i < w.commands.size(); ++i) {
    const Command& c = w.commands[i];
    d.mix(c.id);
    d.mix(static_cast<std::uint64_t>(c.tick));
    d.mix(static_cast<std::uint64_t>(c.qty));
    d.mix(static_cast<std::uint64_t>(c.type) << 16 |
          static_cast<std::uint64_t>(c.side) << 8 |
          (w.phase.empty() ? 0u : w.phase[i]));
  }
  return d.value();
}

bool write_stream(std::ostream& os, const Workload& w) {
  os.write(kMagic, 8);
  put_u32(os, kVersion);
  put_u32(os, kRecordBytes);
  put_u64(os, w.seed);
  put_u64(os, w.commands.size());
  put_u64(os, w.limits.max_resting_orders);
  put_u64(os, w.limits.max_levels_per_side);
  put_u64(os, static_cast<std::uint64_t>(w.limits.max_tick));
  put_u16(os, static_cast<std::uint16_t>(w.name.size()));
  os.write(w.name.data(), static_cast<std::streamsize>(w.name.size()));
  const char flags = w.phase.empty() ? 0 : 1;
  os.write(&flags, 1);
  for (std::size_t i = 0; i < w.commands.size(); ++i) {
    const Command& c = w.commands[i];
    put_u64(os, c.id);
    put_u64(os, static_cast<std::uint64_t>(c.tick));
    put_u64(os, static_cast<std::uint64_t>(c.qty));
    const char tail[8] = {static_cast<char>(c.type),
                          static_cast<char>(c.side),
                          static_cast<char>(w.phase.empty() ? 0 : w.phase[i]),
                          0,
                          0,
                          0,
                          0,
                          0};
    os.write(tail, 8);
  }
  put_u64(os, stream_digest(w));
  return static_cast<bool>(os);
}

std::optional<Workload> read_stream(std::istream& is, std::string* error) {
  const auto fail = [error](const char* why) -> std::optional<Workload> {
    if (error) *error = why;
    return std::nullopt;
  };
  char magic[8];
  if (!get(is, magic, 8) || !std::equal(magic, magic + 8, kMagic))
    return fail("not a command stream (bad magic)");
  std::uint32_t version = 0, record = 0;
  if (!get_le(is, version) || version != kVersion)
    return fail("unsupported stream version");
  if (!get_le(is, record) || record != kRecordBytes)
    return fail("unexpected record size");

  Workload w;
  std::uint64_t count = 0, orders = 0, levels = 0, max_tick = 0;
  std::uint16_t name_len = 0;
  if (!get_le(is, w.seed) || !get_le(is, count) || !get_le(is, orders) ||
      !get_le(is, levels) || !get_le(is, max_tick) || !get_le(is, name_len))
    return fail("truncated header");
  w.limits = {orders, levels, static_cast<Tick>(max_tick)};
  w.name.resize(name_len);
  unsigned char flags = 0;
  if (!get(is, w.name.data(), name_len) || !get(is, &flags, 1))
    return fail("truncated name");
  const bool phased = flags & 1;

  w.commands.reserve(count);
  if (phased) w.phase.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t id = 0, tick = 0, qty = 0;
    unsigned char tail[8];
    if (!get_le(is, id) || !get_le(is, tick) || !get_le(is, qty) ||
        !get(is, tail, 8))
      return fail("truncated record");
    if (tail[0] != 1 && tail[0] != 2) return fail("bad command type");
    if (tail[1] != 1 && tail[1] != 2) return fail("bad side");
    Command c;
    c.seq = i + 1;
    c.type = static_cast<CommandType>(tail[0]);
    c.id = id;
    c.side = static_cast<Side>(tail[1]);
    c.tick = static_cast<Tick>(tick);
    c.qty = static_cast<Qty>(qty);
    w.commands.push_back(c);
    if (phased) w.phase.push_back(tail[2]);
  }

  std::uint64_t digest = 0;
  if (!get_le(is, digest)) return fail("missing digest");
  if (digest != stream_digest(w)) return fail("digest mismatch");
  return w;
}

}  // namespace msim
