#pragma once

#include <cstddef>
#include <cstdint>

#include "msim/feed_book.hpp"

namespace msim {

// Latches the first failed transition. Later callbacks cannot mutate the
// book. Byte offsets are zero based; message ordinals are one based.
template <typename Book>
struct FeedHandler {
  explicit FeedHandler(Book& b) : book(b) {}
  Book& book;
  FeedBook::Status status = FeedBook::Status::Applied;
  itch::Header fault_header{};
  itch::Ref fault_ref = 0;
  std::size_t base_offset = 0, offset = 0;
  std::uint64_t base_message = 0, ordinal = 0;

  bool failed() const noexcept { return status != FeedBook::Status::Applied; }
  void set_context(std::size_t at, std::uint64_t n) noexcept {
    if (!failed()) { offset = base_offset + at; ordinal = base_message + n; }
  }
  void save(FeedBook::Status result, const itch::Header& h, itch::Ref ref) {
    if (result != FeedBook::Status::Applied) {
      status = result; fault_header = h; fault_ref = ref;
    }
  }
  void on(const itch::AddOrder& m) {
    if (!failed()) save(book.add(m.locate, m.ref,
        m.side == 'B' ? Side::BUY : m.side == 'S' ? Side::SELL : static_cast<Side>(0),
        m.shares, m.price), m, m.ref);
  }
  void on(const itch::OrderExecuted& m) {
    if (!failed()) save(book.execute(m.locate, m.ref, m.shares), m, m.ref);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    if (!failed()) save(book.execute(m.locate, m.ref, m.shares), m, m.ref);
  }
  void on(const itch::OrderCancel& m) {
    if (!failed()) save(book.cancel(m.locate, m.ref, m.shares), m, m.ref);
  }
  void on(const itch::OrderDelete& m) {
    if (!failed()) save(book.remove(m.locate, m.ref), m, m.ref);
  }
  void on(const itch::OrderReplace& m) {
    if (!failed()) save(book.replace(m.locate, m.old_ref, m.new_ref,
                                    m.shares, m.price), m, m.old_ref);
  }
  template <typename M> void on(const M&) {}
  void on_other(const itch::Header&) {}
};

}  // namespace msim
