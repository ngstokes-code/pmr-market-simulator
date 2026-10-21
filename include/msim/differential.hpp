#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "msim/book_types.hpp"
#include "msim/command.hpp"

namespace msim {

// The first point where two books disagree.
struct Divergence {
  std::size_t index = 0;  // position of the command in the sequence
  std::string what;
};

struct CompareOptions {
  std::size_t snapshot_every = 1;  // also compared after the last command
  bool check_invariants = true;     // for books that offer the hook
};

namespace detail {

template <typename T, typename = void>
struct has_invariants : std::false_type {};
template <typename T>
struct has_invariants<
    T,
    std::void_t<decltype(std::declval<const T&>().check_invariants(nullptr))>>
    : std::true_type {};

template <typename Book>
std::optional<std::string> broken(const Book& b) {
  if constexpr (has_invariants<Book>::value) {
    std::string why;
    if (!b.check_invariants(&why)) return why;
  }
  return std::nullopt;
}

std::string describe(const std::vector<Execution>& a,
                     const std::vector<Execution>& b);
std::string describe(const BookSnapshot& a, const BookSnapshot& b);

}  // namespace detail

// Feeds the same commands to `a` and `b` and compares, after every command,
// the outcome and the full list of executions. Book state is compared every
// `snapshot_every` commands and at the end.
template <typename A, typename B>
std::optional<Divergence> first_divergence(A& a, B& b,
                                           const std::vector<Command>& cmds,
                                           const CompareOptions& opt = {}) {
  std::vector<Execution> ea, eb;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const Outcome oa = apply(a, cmds[i], ea);
    const Outcome ob = apply(b, cmds[i], eb);
    if (oa != ob)
      return Divergence{i, "outcome " + to_string(oa) + " vs " + to_string(ob)};
    if (ea != eb) return Divergence{i, detail::describe(ea, eb)};

    if (opt.check_invariants) {
      if (auto why = detail::broken(a))
        return Divergence{i, "first book: " + *why};
      if (auto why = detail::broken(b))
        return Divergence{i, "second book: " + *why};
    }
    const bool last = i + 1 == cmds.size();
    if ((opt.snapshot_every && (i + 1) % opt.snapshot_every == 0) || last) {
      const BookSnapshot sa = a.snapshot();
      const BookSnapshot sb = b.snapshot();
      if (!(sa == sb)) return Divergence{i, detail::describe(sa, sb)};
    }
  }
  return std::nullopt;
}

// OrderBook against ReferenceBook, both fresh, both with `limits`.
std::optional<Divergence> engine_vs_reference(const std::vector<Command>& cmds,
                                              const BookLimits& limits,
                                              const CompareOptions& opt = {});

// Greedy delta debugging: removes ever smaller chunks of commands while
// `fails` stays true. The result still fails and no single chunk of the
// final size can be removed from it. Precondition: fails(cmds).
std::vector<Command> shrink(
    std::vector<Command> cmds,
    const std::function<bool(const std::vector<Command>&)>& fails);

}  // namespace msim
