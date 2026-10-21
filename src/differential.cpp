#include "msim/differential.hpp"

#include <algorithm>
#include <memory_resource>
#include <sstream>

#include "msim/order_book.hpp"
#include "msim/reference_book.hpp"

namespace msim {

namespace detail {

std::string describe(const std::vector<Execution>& a,
                     const std::vector<Execution>& b) {
  std::ostringstream os;
  os << "executions differ (" << a.size() << " vs " << b.size() << ")";
  const std::size_t n = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const bool same = i < a.size() && i < b.size() && a[i] == b[i];
    if (same) continue;
    os << "; first difference at #" << i << ": "
       << (i < a.size() ? to_string(a[i]) : "none") << " vs "
       << (i < b.size() ? to_string(b[i]) : "none");
    break;
  }
  return os.str();
}

namespace {

void describe_side(std::ostringstream& os, const char* name,
                   const std::vector<LevelSnapshot>& a,
                   const std::vector<LevelSnapshot>& b) {
  const std::size_t n = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (i < a.size() && i < b.size() && a[i] == b[i]) continue;
    os << name << " level #" << i << ": ";
    if (i < a.size())
      os << "tick " << a[i].tick << " with " << a[i].orders.size() << " orders";
    else
      os << "none";
    os << " vs ";
    if (i < b.size())
      os << "tick " << b[i].tick << " with " << b[i].orders.size() << " orders";
    else
      os << "none";
    return;
  }
}

}  // namespace

std::string describe(const BookSnapshot& a, const BookSnapshot& b) {
  std::ostringstream os;
  os << "book state differs: ";
  if (!(a.bids == b.bids))
    describe_side(os, "bid", a.bids, b.bids);
  else
    describe_side(os, "ask", a.asks, b.asks);
  return os.str();
}

}  // namespace detail

std::optional<Divergence> engine_vs_reference(const std::vector<Command>& cmds,
                                              const BookLimits& limits,
                                              const CompareOptions& opt) {
  std::pmr::unsynchronized_pool_resource mr;
  OrderBook engine("diff", &mr, limits);
  ReferenceBook reference(limits);
  return first_divergence(engine, reference, cmds, opt);
}

std::vector<Command> shrink(
    std::vector<Command> cmds,
    const std::function<bool(const std::vector<Command>&)>& fails) {
  for (std::size_t chunk = std::max<std::size_t>(cmds.size() / 2, 1);;) {
    bool removed = false;
    for (std::size_t start = 0; start < cmds.size() && cmds.size() > 1;) {
      std::vector<Command> trial;
      trial.reserve(cmds.size());
      const std::size_t end = std::min(start + chunk, cmds.size());
      trial.insert(trial.end(), cmds.begin(),
                   cmds.begin() + static_cast<std::ptrdiff_t>(start));
      trial.insert(trial.end(), cmds.begin() + static_cast<std::ptrdiff_t>(end),
                   cmds.end());
      if (!trial.empty() && fails(trial)) {
        cmds = std::move(trial);
        removed = true;
      } else {
        start += chunk;
      }
    }
    if (removed) continue;
    if (chunk == 1) break;
    chunk /= 2;
  }
  renumber(cmds);
  return cmds;
}

}  // namespace msim
