#include "msim/volume.hpp"

#include "check.hpp"
int main() {
  using namespace msim::volume;
  // Breaks are received in a later interval, but reverse the original interval.
  std::vector<Record> records{{1, 1, 0, 10, 100, 1, 'E'},
                              {2, 2, interval_ns, 0, 40, 1, 'P'},
                              {3, 3, interval_ns, 11, 60, 1, 'Y'},
                              {4, 4, 0, 12, 50, 1, 'N'},
                              {6, 5, 0, 13, 50, 1, 'N'},
                              {7, 6, 0, 0, 100, 1, 'Q'},
                              {1, 7, 2 * interval_ns, 0, 0, 1, 'B'},
                              {2, 8, 3 * interval_ns, 0, 0, 1, 'B'},
                              {5, 9, interval_ns, 0, 20, 2, 'P'}};
  Result r;
  r.reconcile(records);
  const auto total = r.total();
  CHECK(total[E] == 0 && total[CY] == 60 && total[P] == 20 && total[Q] == 100 &&
        total[CN] == 100);
  CHECK(r.breaks == 2 && r.corrected_shares == 140);
  CHECK(r.cells[Key(1, 0)][E] == 0 && r.cells[Key(1, 1)][P] == 0);
  double f = 0;
  CHECK(!fraction(r.cells[Key(1, 0)], f));
  CHECK(fraction(total, f) && f == 0.25);
  for (auto bad : std::vector<std::vector<Record>>{
           {{1, 1, 0, 1, 10, 1, 'E'}, {1, 2, 0, 1, 10, 1, 'E'}},
           {{2, 1, 0, 0, 0, 0, 'B'}},
           {{1, 1, 0, 0, 0, 1, 'B'}, {1, 2, 0, 1, 10, 1, 'E'}},
           {{1, 1, 0, 1, 10, 1, 'E'},
            {1, 2, 0, 0, 0, 1, 'B'},
            {1, 3, 0, 0, 0, 1, 'B'}},
           {{3, 1, 0, 1, 10, 1, 'E'},
            {3, 2, 0, 0, 0, 2, 'B'}}}) {
    bool failed = false;
    try {
      Result x;
      x.reconcile(bad);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    CHECK(failed);
  }
  // Partition order cannot change the cell sums or correction attribution.
  Result partitioned;
  for (unsigned p = 0; p < 4; ++p) {
    std::vector<Record> part;
    for (const auto& row : records)
      if (row.match % 4 == p) part.push_back(row);
    partitioned.reconcile(part);
  }
  CHECK(partitioned.cells == r.cells && partitioned.total() == total &&
        partitioned.breaks == r.breaks);
  // One match can report both displayed sides. Count the printable entry once.
  std::vector<Record> companion{{8, 1, 0, 20, 100, 1, 'N'},
                                {8, 2, 0, 21, 100, 1, 'E'},
                                {8, 3, interval_ns, 0, 0, 1, 'B'}};
  Result c;
  c.reconcile(companion);
  CHECK(c.companion_groups == 1 && c.breaks == 1 && c.corrected_shares == 100 &&
        c.corrected_nonprintable_shares == 100 && c.total()[E] == 0 &&
        c.total()[CN] == 0);
  bool duplicate = false;
  companion = {{8, 1, 0, 20, 100, 1, 'N'}, {8, 2, 0, 20, 100, 1, 'N'}};
  try {
    Result x;
    x.reconcile(companion);
  } catch (const std::runtime_error&) {
    duplicate = true;
  }
  CHECK(duplicate);
  return check::finish("corrected executed-volume accounting");
}
