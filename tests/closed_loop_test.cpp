#include <memory_resource>
#include <string>
#include <tuple>

#include "check.hpp"
#include "msim/feed_handler.hpp"
#include "msim/order_book.hpp"
#include "msim/synthetic_itch.hpp"

namespace {
std::vector<unsigned char> hex(const char* s) {
  std::vector<unsigned char> b;
  for (std::size_t i = 0; s[i]; i += 2)
    b.push_back(static_cast<unsigned char>(
        std::stoul(std::string(s + i, 2), nullptr, 16)));
  return b;
}
void scenario() {
  using msim::Command;
  using msim::Side;
  std::pmr::unsynchronized_pool_resource memory;
  msim::OrderBook engine("SYNTH", &memory, {4, 2, 2000000000});
  msim::FeedBook feed({4, 4});
  msim::FeedHandler<msim::FeedBook> handler(feed);
  msim::SyntheticItch encoder;
  std::vector<msim::Execution> executions;
  const std::vector<Command> commands = {
      Command::add(1, 1, Side::SELL, 10000, 3),
      Command::add(2, 2, Side::BUY, 10000, 2),
      Command::add(3, 3, Side::SELL, 10000, 4),
      Command::add(4, 4, Side::SELL, 10001, 2),
      Command::add(5, 5, Side::SELL, 10002,
                   1),  // atomic level-capacity rejection
      Command::add(6, 6, Side::BUY, 10001,
                   6),  // FIFO, partial fill, two prices
      Command::cancel(7, 4),
      Command::cancel(8, 4),  // empty book, then absent cancel
      Command::add(9, 1, Side::BUY, 9999,
                   7),  // reused engine ID, fresh feed reference
      Command::add(10, 7, Side::BUY, 9998, 1),
      Command::add(11, 8, Side::BUY, 9999, 1),
      Command::add(12, 9, Side::BUY, 9998, 1),
      Command::add(13, 10, Side::BUY, 9999, 1),  // order-capacity rejection
      Command::add(14, 11, Side::SELL, 9998,
                   10),  // complete sweep, empty transition
  };
  using Row = std::tuple<Side, unsigned, std::uint64_t, unsigned>;
  const std::vector<std::vector<Row>> handwritten = {
      {{Side::SELL, 10000, 1, 3}},
      {{Side::SELL, 10000, 1, 1}},
      {{Side::SELL, 10000, 1, 1}, {Side::SELL, 10000, 3, 4}},
      {{Side::SELL, 10000, 1, 1},
       {Side::SELL, 10000, 3, 4},
       {Side::SELL, 10001, 4, 2}},
      {{Side::SELL, 10000, 1, 1},
       {Side::SELL, 10000, 3, 4},
       {Side::SELL, 10001, 4, 2}},
      {{Side::SELL, 10001, 4, 1}},
      {},
      {},
      {{Side::BUY, 9999, 1, 7}},
      {{Side::BUY, 9999, 1, 7}, {Side::BUY, 9998, 7, 1}},
      {{Side::BUY, 9999, 1, 7},
       {Side::BUY, 9999, 8, 1},
       {Side::BUY, 9998, 7, 1}},
      {{Side::BUY, 9999, 1, 7},
       {Side::BUY, 9999, 8, 1},
       {Side::BUY, 9998, 7, 1},
       {Side::BUY, 9998, 9, 1}},
      {{Side::BUY, 9999, 1, 7},
       {Side::BUY, 9999, 8, 1},
       {Side::BUY, 9998, 7, 1},
       {Side::BUY, 9998, 9, 1}},
      {}};
  for (const auto& c : commands) {
    encoder.validate(c);
    const auto outcome = msim::apply(engine, c, executions);
    const auto bytes = encoder.encode(c, outcome, executions);
    if (c.seq == 1)
      CHECK(bytes == hex("00244100010000000000000001000000000000000153000000035"
                         "3594e544820202000002710"));
    if (c.seq == 2)
      CHECK(bytes == hex("001f4500010000000000000002000000000000000100000002000"
                         "0000000000001"));
    const auto parsed = msim::itch::parse(bytes.data(), bytes.size(), handler);
    CHECK(!parsed.truncated && !parsed.bad_length && !parsed.bad_field &&
          !parsed.unsupported && parsed.bytes == bytes.size() &&
          !handler.failed());
    std::vector<Row> expected, actual, engine_rows;
    const auto snapshot = engine.snapshot();
    for (Side side : {Side::BUY, Side::SELL}) {
      const auto& levels = side == Side::BUY ? snapshot.bids : snapshot.asks;
      for (const auto& level : levels)
        for (const auto& order : level.orders) {
          engine_rows.emplace_back(side, static_cast<unsigned>(level.tick),
                                   order.first,
                                   static_cast<unsigned>(order.second));
          expected.emplace_back(side, static_cast<unsigned>(level.tick),
                                encoder.reference(order.first),
                                static_cast<unsigned>(order.second));
        }
      feed.for_each(1, side, [&](auto price, auto ref, auto qty) {
        actual.emplace_back(side, price, ref, qty);
      });
    }
    CHECK(expected == actual && engine.resting_orders() == feed.orders());
    CHECK(engine_rows == handwritten.at(static_cast<std::size_t>(c.seq - 1)));
    CHECK(engine.check_invariants() && feed.check_invariants());
  }
  CHECK(feed.orders() == 0);
  for (auto c : {Command::add(1, 1, Side::BUY, 2000000001LL, 1),
                 Command::add(1, 1, Side::BUY, 1,
                              static_cast<msim::Qty>(UINT32_MAX) + 1)}) {
    bool rejected = false;
    try {
      encoder.validate(c);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }
}
}  // namespace
int main() {
  scenario();
  return check::finish("synthetic ITCH closed loop");
}
