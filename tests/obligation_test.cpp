#include <memory_resource>
#include <vector>

#include "check.hpp"
#include "msim/buffer_epoch.hpp"
#include "msim/differential.hpp"
#include "msim/feed_book.hpp"
#include "msim/itch.hpp"
#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"

namespace {
class LostCancel {
 public:
  explicit LostCancel(std::pmr::memory_resource* mr) : book("mutant", mr) {}
  msim::OrderBook book;
  auto add(const msim::Order& o, std::vector<msim::Execution>& e) {
    return book.add(o, e);
  }
  bool cancel(msim::OrderId) {
    return true;
  }  // planted: reports removal, keeps state
  auto snapshot() const { return book.snapshot(); }
};
struct Ignore {
  template <class T>
  void on(const T&) {}
  void on_other(const msim::itch::Header&) {}
};
}  // namespace
int main() {
  using msim::Command;
  using msim::Side;
  std::pmr::unsynchronized_pool_resource memory;
  LostCancel broken(&memory);
  msim::ReferenceBook reference;
  const std::vector<Command> commands{Command::add(1, 1, Side::BUY, 100, 10),
                                      Command::cancel(2, 1)};
  const auto failure = msim::first_divergence(broken, reference, commands);
  CHECK(failure && failure->index == 1);
  // Planted premature mutation, compared with an independently specified
  // pre-state.
  msim::FeedBook book({4, 4});
  book.add(1, 1, Side::BUY, 100, 100);
  book.add(1, 2, Side::BUY, 50, 100);
  const auto before = book.digest();
  book.remove(
      1, 1);  // planted: removes the original before checking the replacement
  CHECK(book.replace(1, 1, 2, 100, 100) == msim::FeedBook::Status::UnknownRef);
  CHECK(book.digest() != before && book.orders() != 2);
  // Planted parser lies about truncation. Consumed bytes remain an independent
  // obligation.
  const unsigned char truncated[] = {0, 12, 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  Ignore ignore;
  auto parsed = msim::itch::parse(truncated, sizeof truncated, ignore);
  CHECK(parsed.truncated);
  parsed.truncated = false;
  CHECK(parsed.bytes != sizeof truncated);
  msim::BufferEpoch epochs;
  epochs.reading(0);
  bool caught = false;
  try {
    epochs.writing(0);
  } catch (const std::logic_error&) {
    caught = true;
  }
  CHECK(caught);  // planted: producer omits retirement before reuse
  epochs.retired(0);
  epochs.writing(0);
  return check::finish(
      "obligations: cancellation, premature mutation, parser, buffer lease");
}
