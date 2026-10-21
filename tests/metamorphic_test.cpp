// Metamorphic tests: properties the matching rules must have, checked by
// transforming the input and predicting the output. They need no reference
// book, so they still catch a defect that the engine and the reference share
// because both read the contract the same wrong way.
//
//   mirror  tick -> max_tick + 1 - tick, and BUY <-> SELL.
//           Price-time priority has no preferred side, so executions mirror.
//   shift   tick -> tick + k. Only price differences matter.
//   rename  id -> a bijection of id. Ids are labels, nothing more.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "check.hpp"
#include "msim/command.hpp"
#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"
#include "random_commands.hpp"

using msim::Command;
using msim::Execution;
using msim::OrderId;
using msim::Tick;

namespace {

struct Transform {
  const char* name;
  std::function<Command(Command)> input;
  std::function<Execution(Execution)> output;
  std::function<msim::BookSnapshot(msim::BookSnapshot)> state;
};

msim::BookSnapshot map_snapshot(msim::BookSnapshot s, bool swap_sides,
                                const std::function<Tick(Tick)>& tick,
                                const std::function<OrderId(OrderId)>& id) {
  for (auto* side : {&s.bids, &s.asks})
    for (auto& lvl : *side) {
      lvl.tick = tick(lvl.tick);
      for (auto& o : lvl.orders) o.first = id(o.first);
    }
  if (swap_sides) std::swap(s.bids, s.asks);
  return s;
}

Transform mirror(Tick max_tick) {
  const auto t = [max_tick](Tick x) { return max_tick + 1 - x; };
  const auto same = [](OrderId x) { return x; };
  return {"mirror",
          [t](Command c) {
            c.tick = c.type == msim::CommandType::Add ? t(c.tick) : c.tick;
            c.side = msim::opposite(c.side);
            return c;
          },
          [t](Execution x) {
            x.tick = t(x.tick);
            return x;
          },
          [t, same](msim::BookSnapshot s) {
            return map_snapshot(std::move(s), true, t, same);
          }};
}

Transform shift(Tick k) {
  const auto t = [k](Tick x) { return x + k; };
  const auto same = [](OrderId x) { return x; };
  return {"shift",
          [t](Command c) {
            if (c.type == msim::CommandType::Add) c.tick = t(c.tick);
            return c;
          },
          [t](Execution x) {
            x.tick = t(x.tick);
            return x;
          },
          [t, same](msim::BookSnapshot s) {
            return map_snapshot(std::move(s), false, t, same);
          }};
}

Transform rename() {
  // Multiplying by an odd constant is a bijection on 64-bit integers.
  const auto f = [](OrderId x) { return x * 0x9E3779B97F4A7C15ULL + 12345; };
  const auto same = [](Tick x) { return x; };
  return {"rename",
          [f](Command c) {
            c.id = f(c.id);
            return c;
          },
          [f](Execution x) {
            x.resting_id = f(x.resting_id);
            x.incoming_id = f(x.incoming_id);
            return x;
          },
          [f, same](msim::BookSnapshot s) {
            return map_snapshot(std::move(s), false, same, f);
          }};
}

// Runs cmds on one book and transformed cmds on another, and checks that
// every outcome is equal and every execution and the final state map
// through the transform.
template <typename MakeBook>
std::size_t violations(const std::vector<Command>& cmds, const Transform& tf,
                       MakeBook make, bool quiet = false) {
  auto a = make();
  auto b = make();
  std::vector<Execution> ea, eb;
  std::size_t bad = 0;
  for (const Command& c : cmds) {
    const auto oa = msim::apply(*a, c, ea);
    const auto ob = msim::apply(*b, tf.input(c), eb);
    for (auto& x : ea) x = tf.output(x);
    if (oa != ob || ea != eb) {
      if (bad++ == 0 && !quiet)
        std::fprintf(stderr, "  %s broke at %s: %s vs %s\n", tf.name,
                     msim::to_string(c).c_str(), msim::to_string(oa).c_str(),
                     msim::to_string(ob).c_str());
    }
  }
  if (!(tf.state(a->snapshot()) == b->snapshot())) ++bad;
  return bad;
}

void test_profiles() {
  for (const testgen::Profile& p : testgen::kProfiles) {
    const Tick max_tick = p.limits.max_tick;
    // A shift must keep every valid tick valid and every invalid one invalid,
    // so it applies only to profiles without deliberately bad ticks.
    std::vector<Transform> transforms = {mirror(max_tick), rename()};
    if (p.p_invalid == 0.0 && p.hi + 7 <= max_tick)
      transforms.push_back(shift(7));

    for (std::uint64_t seed = 0; seed < 10; ++seed) {
      const auto cmds = testgen::random_commands(p, seed, 1500);
      for (const Transform& tf : transforms) {
        std::pmr::unsynchronized_pool_resource mr;
        const auto engine = [&] {
          return std::make_unique<msim::OrderBook>("X", &mr, p.limits);
        };
        const auto reference = [&] {
          return std::make_unique<msim::ReferenceBook>(p.limits);
        };
        const std::size_t e = violations(cmds, tf, engine);
        const std::size_t r = violations(cmds, tf, reference);
        if (!CHECK(e == 0 && r == 0))
          std::fprintf(stderr, "  profile=%s seed=%llu transform=%s\n", p.name,
                       static_cast<unsigned long long>(seed), tf.name);
      }
    }
  }
}

// The harness must be able to fail: a book that favours one side breaks the
// mirror even though it agrees with itself on every unmirrored input.
class BidsFirstAtTies {
 public:
  explicit BidsFirstAtTies(std::pmr::memory_resource* mr) : book_("X", mr) {}
  msim::AddResult add(const msim::Order& o, std::vector<Execution>& out) {
    // Planted asymmetry: a sell that would rest exactly one tick above the
    // best bid is nudged down by one tick. Buys get no such treatment.
    msim::Order m = o;
    if (o.side == msim::Side::SELL && book_.best_bid() &&
        o.tick == *book_.best_bid() + 1)
      m.tick -= 1;
    return book_.add(m, out);
  }
  bool cancel(OrderId id) { return book_.cancel(id); }
  msim::BookSnapshot snapshot() const { return book_.snapshot(); }

 private:
  msim::OrderBook book_;
};

void test_mirror_catches_side_asymmetry() {
  const testgen::Profile& p = testgen::kProfiles[0];
  const auto cmds = testgen::random_commands(p, 5, 1500);
  std::pmr::unsynchronized_pool_resource mr;
  const auto make = [&] { return std::make_unique<BidsFirstAtTies>(&mr); };
  CHECK(violations(cmds, mirror(p.limits.max_tick), make, true) > 0);
  CHECK(violations(cmds, rename(), make, true) == 0);  // renaming cannot see it
}

}  // namespace

int main() {
  test_profiles();
  test_mirror_catches_side_asymmetry();
  return check::finish("metamorphic");
}
