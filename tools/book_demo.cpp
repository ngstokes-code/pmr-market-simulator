// A short, fixed scenario that shows the matching rules one at a time. Each
// step prints the command, what the book did, and the book afterwards.
// The output is deterministic; docs/demo.txt retains it, and
// scripts/audit.py compares it with the built executable.

#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

#include "msim/command.hpp"
#include "msim/order_book.hpp"

namespace {

using msim::Side;

constexpr double kTick = 0.01;  // prices print in dollars and cents

std::string px(msim::Tick t) {
  char b[32];
  std::snprintf(b, sizeof(b), "%6.2f", static_cast<double>(t) * kTick);
  return b;
}

void ladder(const msim::OrderBook& book) {
  const msim::BookSnapshot s = book.snapshot();
  for (auto it = s.asks.rbegin(); it != s.asks.rend(); ++it) {
    std::printf("      ask %s |", px(it->tick).c_str());
    for (const auto& [id, qty] : it->orders)
      std::printf(" #%llu x%lld", static_cast<unsigned long long>(id),
                  static_cast<long long>(qty));
    std::printf("\n");
  }
  const auto bb = book.best_bid(), ba = book.best_ask();
  if (bb && ba)
    std::printf("      ------------ spread %lld tick%s\n",
                static_cast<long long>(*ba - *bb), *ba - *bb == 1 ? "" : "s");
  else
    std::printf("      ------------\n");
  for (const auto& lvl : s.bids) {
    std::printf("      bid %s |", px(lvl.tick).c_str());
    for (const auto& [id, qty] : lvl.orders)
      std::printf(" #%llu x%lld", static_cast<unsigned long long>(id),
                  static_cast<long long>(qty));
    std::printf("\n");
  }
}

struct Demo {
  std::pmr::unsynchronized_pool_resource mr;
  msim::OrderBook book{"DEMO", &mr};
  std::vector<msim::Execution> out;
  std::uint64_t seq = 0;
  bool first_note = true;

  void note(const char* text) {
    std::printf(first_note ? "%s\n" : "\n%s\n", text);
    first_note = false;
  }

  void add(msim::OrderId id, Side side, msim::Tick tick, msim::Qty qty) {
    const auto o =
        msim::apply(book, msim::Command::add(++seq, id, side, tick, qty), out);
    std::printf(
        "  > %s #%llu %lld @ %s: %s", side == Side::BUY ? "buy " : "sell",
        static_cast<unsigned long long>(id), static_cast<long long>(qty),
        px(tick).c_str(), msim::to_string(o.add.status));
    if (o.add.status == msim::AddStatus::Accepted)
      std::printf(", executed %lld, rested %lld",
                  static_cast<long long>(o.add.filled),
                  static_cast<long long>(o.add.rested));
    std::printf("\n");
    report_executions(side);
  }

  void cancel(msim::OrderId id) {
    const auto o = msim::apply(book, msim::Command::cancel(++seq, id), out);
    std::printf("  > cancel #%llu: %s\n", static_cast<unsigned long long>(id),
                o.canceled ? "removed" : "not resting");
  }

  void report_executions(Side incoming) {
    if (out.empty()) return;
    msim::Qty qty = 0;
    msim::Tick notional = 0;
    for (const auto& x : out) {
      std::printf("      execution: #%llu x%lld @ %s against resting #%llu\n",
                  static_cast<unsigned long long>(x.incoming_id),
                  static_cast<long long>(x.qty), px(x.tick).c_str(),
                  static_cast<unsigned long long>(x.resting_id));
      qty += x.qty;
      notional += x.qty * x.tick;
    }
    if (out.front().tick != out.back().tick) {
      // Cost of walking the book: the average price against the first, best
      // price, in ticks per unit. Positive is worse for the incoming order.
      const double avg =
          static_cast<double>(notional) / static_cast<double>(qty);
      const double first = static_cast<double>(out.front().tick);
      const double cost = incoming == Side::BUY ? avg - first : first - avg;
      std::printf(
          "      average price %.4f, %.2f ticks per unit worse than the best "
          "price it met\n",
          avg * kTick, cost);
    }
  }

  void show() { ladder(book); }
};

}  // namespace

int main() {
  Demo d;

  d.note("1. Sellers and buyers rest. Two sells share the price 100.02.");
  d.add(1, Side::SELL, 10003, 5);
  d.add(2, Side::SELL, 10002, 3);
  d.add(3, Side::SELL, 10002, 4);
  d.add(4, Side::SELL, 10005, 10);
  d.add(5, Side::BUY, 9998, 6);
  d.add(6, Side::BUY, 9999, 2);
  d.show();

  d.note(
      "2. A buy for 5 at 100.02. Price first: it takes the lowest ask. Then "
      "time: at 100.02, #2 arrived before #3, so #2 executes in full "
      "first.");
  d.add(7, Side::BUY, 10002, 5);
  d.show();

  d.note("3. #3 cancels. Its level empties, so the best ask moves up a tick.");
  d.cancel(3);
  d.show();

  d.note(
      "4. A sell for 10 at 99.98 walks the book. It executes against every "
      "bid at or above its limit, best price first, and the last 2 rest "
      "as the new best ask.");
  d.add(8, Side::SELL, 9998, 10);
  d.show();

  d.note(
      "5. An order whose id is resting now is refused, and nothing changes.");
  d.add(8, Side::BUY, 9990, 1);

  d.note(
      "6. Queue position. Two buys join at 99.97, #9 before #10. A sell for 3 "
      "reaches #9 only. #10 waits behind it, at the same price.");
  d.add(9, Side::BUY, 9997, 3);
  d.add(10, Side::BUY, 9997, 3);
  d.add(11, Side::SELL, 9997, 3);
  d.show();
  return 0;
}
