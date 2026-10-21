// FeedBook: directed cases for each message's effect, then seeded random
// feeds compared against FeedReference message by message.

#include "msim/feed_book.hpp"
#include "msim/feed_identity.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

#include "check.hpp"
#include "msim/feed_reference.hpp"
#include "msim/rng.hpp"

using msim::FeedBook;
using msim::Side;

namespace msim {
struct FeedBookTestAccess {
  static void corrupt(FeedBook& b, unsigned kind) {
    const auto li = b.books_[1].bids.levels.back();
    const auto node = b.levels_[li].head;
    if (kind == 0) b.levels_[li].head = FeedBook::kNil - 1;
    if (kind == 1) b.nodes_[node].next = node;
    if (kind == 2) b.free_node_ = node;
    if (kind == 3) b.free_levels_.push_back(li);
    if (kind == 4) b.free_node_ = FeedBook::kNil - 1;
  }
};
}  // namespace msim

namespace {

constexpr Side B = Side::BUY;
constexpr Side S = Side::SELL;

bool same_position(const std::optional<FeedBook::QueuePosition>& a,
                   const std::optional<FeedBook::QueuePosition>& b) {
  if (!a || !b) return !a && !b;
  return a->at_best == b->at_best && a->ahead == b->ahead &&
         a->queue == b->queue;
}

void test_priority_and_position() {
  FeedBook book;
  book.add(1, 10, B, 100, 5000);
  book.add(1, 11, B, 200, 5000);
  book.add(1, 12, B, 300, 5000);
  book.add(1, 13, B, 100, 4900);
  CHECK(book.best(1, B) == 5000u);
  auto p = book.position(11);
  REQUIRE(p);
  CHECK(p->at_best && p->ahead == 1 && p->queue == 3);
  p = book.position(13);
  CHECK(p && !p->at_best && p->ahead == 0 && p->queue == 1);

  // A partial execution and a partial cancel both keep time priority.
  book.execute(1, 10, 40);
  book.cancel(1, 11, 50);
  CHECK(book.shares(10) == 60u && book.shares(11) == 150u);
  CHECK(book.position(11)->ahead == 1);

  // A replace at the same price is a new order at the back of the queue.
  book.replace(1, 10, 20, 60, 5000);
  CHECK(!book.shares(10));
  p = book.position(20);
  CHECK(p && p->ahead == 2 && p->queue == 3);

  // Removing the whole best level makes the next one best.
  book.remove(1, 11);
  book.execute(1, 12, 300);
  book.remove(1, 20);
  CHECK(book.best(1, B) == 4900u);
  CHECK(book.position(13)->at_best);
  CHECK(book.check_invariants());
  CHECK(book.faults().total() == 0);
}

void test_books_are_separate() {
  FeedBook book;
  book.add(7, 1, S, 100, 1010000);
  book.add(8, 2, S, 100, 990000);
  book.add(7, 3, B, 100, 1000000);
  CHECK(book.best(7, S) == 1010000u && book.best(8, S) == 990000u);
  CHECK(book.best(7, B) == 1000000u && !book.best(8, B));
  CHECK(book.levels(7, S) == 1 && book.levels() == 3);
}

template <typename Book>
auto state(const Book& b) {
  std::vector<std::tuple<unsigned, Side, unsigned, std::uint64_t, unsigned>> out;
  for (unsigned loc : {1u, 2u})
    for (Side side : {B, S})
      b.for_each(static_cast<FeedBook::Locate>(loc), side,
          [&](auto price, auto ref, auto qty) {
            out.emplace_back(loc, side, price, ref, qty);
          });
  return out;
}

template <typename Book>
void fault_contract(Book& book) {
  using Status = FeedBook::Status;
  book.add(1, 1, B, 100, 5000);
  book.add(1, 2, B, 50, 5000);
  book.add(2, 3, S, 25, 6000);
  const auto before = state(book);
  struct Case { const char* name; Status want; std::function<Status()> apply; };
  const Case cases[] = {
    {"duplicate add", Status::DuplicateRef, [&] { return book.add(1, 1, B, 1, 5000); }},
    {"zero add", Status::ZeroShares, [&] { return book.add(1, 4, B, 0, 5000); }},
    {"invalid side", Status::InvalidSide, [&] { return book.add(1, 4, static_cast<Side>(0), 1, 5000); }},
    {"unknown execution", Status::UnknownRef, [&] { return book.execute(1, 99, 10); }},
    {"unknown cancel", Status::UnknownRef, [&] { return book.cancel(1, 99, 10); }},
    {"unknown delete", Status::UnknownRef, [&] { return book.remove(1, 99); }},
    {"unknown replace", Status::UnknownRef, [&] { return book.replace(1, 99, 4, 1, 1); }},
    {"wrong locate execution", Status::LocateMismatch, [&] { return book.execute(2, 1, 10); }},
    {"wrong locate cancel", Status::LocateMismatch, [&] { return book.cancel(2, 1, 10); }},
    {"wrong locate delete", Status::LocateMismatch, [&] { return book.remove(2, 1); }},
    {"wrong locate replace", Status::LocateMismatch, [&] { return book.replace(2, 1, 4, 10, 5000); }},
    {"excess execution", Status::OverExecution, [&] { return book.execute(1, 1, 101); }},
    {"excess cancel", Status::OverExecution, [&] { return book.cancel(1, 1, 101); }},
    {"zero execution", Status::ZeroShares, [&] { return book.execute(1, 1, 0); }},
    {"zero cancel", Status::ZeroShares, [&] { return book.cancel(1, 1, 0); }},
    {"zero replace", Status::ZeroShares, [&] { return book.replace(1, 1, 4, 0, 5000); }},
    {"duplicate replacement", Status::DuplicateRef, [&] { return book.replace(1, 1, 2, 100, 5000); }},
    {"same reference replacement", Status::DuplicateRef, [&] { return book.replace(1, 1, 1, 100, 5000); }},
  };
  for (const auto& c : cases) {
    if (!CHECK(c.apply() == c.want && state(book) == before && book.orders() == 3))
      std::fprintf(stderr, "  fault contract: %s\n", c.name);
  }
  CHECK(book.faults().total() == sizeof(cases) / sizeof(cases[0]));
}

void test_faults_are_counted_not_applied() {
  FeedBook book({8, 8});
  msim::FeedReference reference;
  fault_contract(book);
  fault_contract(reference);
  CHECK(book.check_invariants());
  CHECK(state(book) == state(reference));

  FeedBook tiny({2, 2});
  CHECK(tiny.add(1, 1, B, 1, 100) == FeedBook::Status::Applied);
  CHECK(tiny.add(1, 2, B, 1, 101) == FeedBook::Status::Applied);
  const auto before = state(tiny);
  CHECK(tiny.add(1, 3, B, 1, 100) == FeedBook::Status::Capacity);
  CHECK(state(tiny) == before && tiny.faults().capacity == 1);
  // A valid replacement needs no additional order slot, even at capacity.
  CHECK(tiny.replace(1, 1, 3, 2, 102) == FeedBook::Status::Applied);
  CHECK(tiny.orders() == 2 && tiny.shares(3) == 2u && !tiny.shares(1));
  CHECK(tiny.check_invariants());
}

// Random feeds over a few books and a narrow price band, so levels open and
// close constantly. A share of messages name references that are gone, a
// wrong locate, or too many shares, so the fault paths run too.
void test_random_feeds(std::uint64_t seeds, std::size_t messages,
                       FeedBook::Ref stride, bool exact = false) {
  for (std::uint64_t seed = 0; seed < seeds; ++seed) {
    Xoroshiro128Plus rng(seed * 0x9E3779B97F4A7C15ULL + 7);
    FeedBook book;
    msim::FeedReference ref;
    std::unique_ptr<msim::FeedIdentity> identity;
    if (exact) identity = std::make_unique<msim::FeedIdentity>(messages);
    std::vector<FeedBook::Ref> issued;
    FeedBook::Ref next = 1;
    const auto any_ref = [&] {
      if (issued.empty() || rand_bool(rng, 0.05)) return next + 1000;
      return issued[issued.size() - 1 -
                    rand_index(rng, std::min<std::size_t>(issued.size(), 40))];
    };
    for (std::size_t i = 0; i < messages; ++i) {
      const auto loc = static_cast<FeedBook::Locate>(1 + rand_index(rng, 3));
      const Side side = rand_bool(rng, 0.5) ? B : S;
      const auto price = static_cast<FeedBook::Price>(
          10000 + 100 * rand_index(rng, 12));
      const auto shares = static_cast<FeedBook::Shares>(rand_int(rng, 1, 300));
      const double u = rng.next_uniform01();
      if (u < 0.40 || issued.empty()) {
        const FeedBook::Ref r = rand_bool(rng, 0.01) && !issued.empty()
                                    ? issued.back()
                                    : (next += stride);
        const auto status = book.add(loc, r, side, shares, price);
        CHECK(status == ref.add(loc, r, side, shares, price));
        if (identity) CHECK(status == identity->add(loc, r, side, shares, price));
        issued.push_back(r);
      } else if (u < 0.55) {
        const FeedBook::Ref r = any_ref();
        const auto status = book.execute(loc, r, shares);
        CHECK(status == ref.execute(loc, r, shares));
        if (identity) CHECK(status == identity->execute(loc, r, shares));
      } else if (u < 0.65) {
        const FeedBook::Ref r = any_ref();
        const auto status = book.cancel(loc, r, shares);
        CHECK(status == ref.cancel(loc, r, shares));
        if (identity) CHECK(status == identity->cancel(loc, r, shares));
      } else if (u < 0.88) {
        const FeedBook::Ref r = any_ref();
        const auto status = book.remove(loc, r);
        CHECK(status == ref.remove(loc, r));
        if (identity) CHECK(status == identity->remove(loc, r));
      } else {
        const FeedBook::Ref r = any_ref(), fresh = (next += stride);
        const auto status = book.replace(loc, r, fresh, shares, price);
        CHECK(status == ref.replace(loc, r, fresh, shares, price));
        if (identity) CHECK(status == identity->replace(loc, r, fresh, shares, price));
        issued.push_back(fresh);
      }

      const FeedBook::Ref probe = any_ref();
      const bool ok =
          same_position(book.position(probe), ref.position(probe)) &&
          book.orders() == ref.orders() &&
          book.best(loc, side) == ref.best(loc, side);
      if (!CHECK(ok)) {
        std::fprintf(stderr, "  seed %llu message %zu\n",
                     static_cast<unsigned long long>(seed), i);
        return;
      }
      if (exact) {
        std::vector<msim::FeedIdentity::Row> live;
        for (unsigned symbol : {1u, 2u, 3u}) {
          const auto snapshot = [symbol](const auto& b) {
            std::vector<std::tuple<Side, unsigned, std::uint64_t, unsigned>> v;
            for (Side s : {B, S}) b.for_each(static_cast<FeedBook::Locate>(symbol), s,
                [&](auto px, auto reference, auto qty) { v.emplace_back(s, px, reference, qty); });
            return v;
          };
          CHECK(snapshot(book) == snapshot(ref));
          for (Side s : {B, S}) ref.for_each(static_cast<FeedBook::Locate>(symbol), s,
              [&](auto, auto reference, auto qty) {
                live.emplace_back(reference, static_cast<FeedBook::Locate>(symbol), qty);
              });
        }
        std::sort(live.begin(), live.end());
        CHECK(identity->snapshot() == live && identity->check_invariants());
      }
      if (exact || i % 2048 == 0) {
        std::string why;
        if (!CHECK(book.check_invariants(&why) &&
                   book.digest() == ref.digest())) {
          std::fprintf(stderr, "  seed %llu message %zu: %s\n",
                       static_cast<unsigned long long>(seed), i, why.c_str());
          return;
        }
      }
    }
    const FeedBook::Faults& a = book.faults();
    const FeedBook::Faults& b = ref.faults();
    CHECK(a.unknown_ref == b.unknown_ref &&
          a.duplicate_ref == b.duplicate_ref &&
          a.over_execution == b.over_execution &&
          a.locate_mismatch == b.locate_mismatch && a.capacity == 0);
    CHECK(book.digest() == ref.digest());
  }
}

void test_global_identity() {
  using Identity = msim::FeedIdentity;
  using Status = FeedBook::Status;
  Identity identity(2);
  const auto state = [&](std::vector<Identity::Row> expected) {
    CHECK(identity.snapshot() == expected && identity.check_invariants());
  };
  CHECK(identity.add(1, 1, B, 100, 100) == Status::Applied);
  CHECK(identity.add(2, 1, B, 100, 100) == Status::DuplicateRef);
  state({{1, 1, 100}});
  CHECK(identity.add(2, 2, S, 50, 100) == Status::Applied);
  CHECK(identity.add(3, 3, B, 1, 100) == Status::Capacity);
  CHECK(identity.replace(1, 1, 2, 70, 100) == Status::DuplicateRef);
  CHECK(identity.execute(2, 1, 10) == Status::LocateMismatch);
  CHECK(identity.execute(1, 1, 101) == Status::OverExecution);
  CHECK(identity.execute(1, 1, 0) == Status::ZeroShares);
  state({{1, 1, 100}, {2, 2, 50}});
  CHECK(identity.remove(2, 2) == Status::Applied);
  CHECK(identity.replace(1, 1, 2, 70, 100) == Status::Applied);
  state({{2, 1, 70}});
  CHECK(identity.execute(1, 2, 70) == Status::Applied);
  CHECK(identity.add(2, 2, S, 20, 100) == Status::Applied);
  state({{2, 2, 20}});
  CHECK(identity.cancel(2, 2, 20) == Status::Applied);
  state({});
  const auto far = msim::RefMap::kWindow + msim::RefMap::kMaxJump + 10;
  CHECK(identity.add(1, far, B, 1, 100) == Status::Applied);
  CHECK(identity.add(2, 1, B, 1, 100) == Status::Applied);
  state({{1, 2, 1}, {far, 1, 1}});
  CHECK(identity.remove(1, far) == Status::Applied);
  CHECK(identity.remove(2, 1) == Status::Applied);
  state({});
  const auto beyond_window = msim::RefMap::kWindow + 1;
  CHECK(identity.add(1, 1, B, UINT32_MAX, 100) == Status::Applied);
  CHECK(identity.add(2, beyond_window, S, 1, 100) == Status::Applied);
  CHECK(identity.execute(1, 1, UINT32_MAX - 1) == Status::Applied);
  state({{1, 1, 1}, {beyond_window, 2, 1}});
  CHECK(identity.remove(1, 1) == Status::Applied);
  CHECK(identity.remove(2, beyond_window) == Status::Applied);
  state({});
}

void test_reference_catch_up_and_level_capacity() {
  using Status = FeedBook::Status;
  const auto far = msim::RefMap::kWindow + msim::RefMap::kMaxJump + 5;
  FeedBook book({8, 3});
  msim::FeedIdentity identity(8);
  for (auto ref : {far, far - 6, far + 1}) {
    CHECK(book.add(1, ref, B, 1, 100) == Status::Applied);
    CHECK(identity.add(1, ref, B, 1, 100) == Status::Applied);
  }
  CHECK(book.check_invariants() && identity.check_invariants());
  CHECK(book.add(2, far, S, 1, 100) == Status::DuplicateRef);
  CHECK(identity.add(2, far, S, 1, 100) == Status::DuplicateRef);
  CHECK(book.remove(1, far) == Status::Applied);
  CHECK(identity.remove(1, far) == Status::Applied);
  CHECK(book.level(1, B, 100).shares == 2);
  FeedBook bounded({8, 1});
  CHECK(bounded.add(1, 1, B, 10, 100) == Status::Applied);
  CHECK(bounded.add(1, 2, B, 20, 100) == Status::Applied);
  const auto before = bounded.digest();
  CHECK(bounded.add(1, 3, B, 1, 101) == Status::Capacity);
  CHECK(bounded.replace(1, 1, 4, 5, 101) == Status::Capacity);
  CHECK(bounded.digest() == before && bounded.check_invariants());
  CHECK(bounded.remove(1, 2) == Status::Applied);
  CHECK(bounded.replace(1, 1, 4, 5, 101) == Status::Applied);
  CHECK(bounded.level(1, B, 101).shares == 5 && bounded.check_invariants());
}

}  // namespace

int main(int argc, char** argv) {
  const bool bounded = argc == 2 && std::string(argv[1]) == "--bounded";
  if (argc != 1 && !bounded) return 2;
  for (unsigned kind = 0; kind < 5; ++kind) {
    FeedBook corrupt({4, 4});
    corrupt.add(1, 1, B, 100, 5000);
    msim::FeedBookTestAccess::corrupt(corrupt, kind);
    std::string why;
    CHECK(!corrupt.check_invariants(&why) && !why.empty());
  }
  test_random_feeds(5, 1000, 1, true);
  test_global_identity();
  test_reference_catch_up_and_level_capacity();
  test_priority_and_position();
  test_books_are_separate();
  test_faults_are_counted_not_applied();
  test_random_feeds(bounded ? 2 : 30, bounded ? 1000 : 20000, 1);
  // References far apart, so orders keep leaving RefMap's window for its
  // hash while they rest.
  test_random_feeds(bounded ? 2 : 10, bounded ? 1000 : 20000, msim::RefMap::kWindow / 2000);
  std::printf("feed profile: %s; exact bounded transitions=5000\n", bounded ? "targeted sanitizer" : "full stress");
  return check::finish("feed_book");
}
