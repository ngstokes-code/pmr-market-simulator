#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "msim/book_types.hpp"

namespace msim {

enum class CommandType : std::uint8_t { Add = 1, Cancel = 2 };

// One input to a book. side, tick, and qty matter only for Add. Fields are
// ordered largest first, so a command takes 40 bytes rather than 48: a
// million-command stream is 8 MB smaller to hold and to stream through cache.
struct Command {
  std::uint64_t seq{};
  OrderId id{};
  Tick tick{};
  Qty qty{};
  CommandType type{CommandType::Add};
  Side side{Side::BUY};

  static Command add(std::uint64_t seq, OrderId id, Side side, Tick tick,
                     Qty qty) noexcept {
    return Command{seq, id, tick, qty, CommandType::Add, side};
  }
  static Command cancel(std::uint64_t seq, OrderId id) noexcept {
    return Command{seq, id, 0, 0, CommandType::Cancel, Side::BUY};
  }

  friend bool operator==(const Command& a, const Command& b) noexcept {
    return a.seq == b.seq && a.type == b.type && a.id == b.id &&
           a.side == b.side && a.tick == b.tick && a.qty == b.qty;
  }
};

static_assert(sizeof(Command) <= 40, "keep Command at 40 bytes or less");

// What one command did, apart from its executions.
struct Outcome {
  CommandType type{CommandType::Add};
  AddResult add;          // Add only
  bool canceled = false;  // Cancel only

  friend bool operator==(const Outcome& a, const Outcome& b) noexcept {
    return a.type == b.type && a.add == b.add && a.canceled == b.canceled;
  }
  friend bool operator!=(const Outcome& a, const Outcome& b) noexcept {
    return !(a == b);
  }
};

// Applies c to any book with the add/cancel interface. `out` is cleared and
// then receives the command's executions.
template <typename Book>
Outcome apply(Book& book, const Command& c, std::vector<Execution>& out) {
  if (c.type != CommandType::Add && c.type != CommandType::Cancel)
    throw std::invalid_argument("invalid command type");
  out.clear();
  Outcome r;
  r.type = c.type;
  if (c.type == CommandType::Add)
    r.add = book.add(Order{c.id, c.side, c.tick, c.qty}, out);
  else
    r.canceled = book.cancel(c.id);
  return r;
}

std::string to_string(const Command& c);
std::string to_string(const Outcome& o);
std::string to_string(const Execution& x);

// Text trace format, one command per line, so a failing case can be read,
// edited, and replayed by hand:
//
//   # msim-trace 1
//   # limits <max_resting_orders>,<max_levels_per_side>,<max_tick>
//   <seq> add <id> B|S <tick> <qty>
//   <seq> cancel <id>
//
// The limits line is optional. Blank lines and other lines starting with '#'
// are comments.
void write_trace(std::ostream& os, const std::vector<Command>& cmds,
                 const BookLimits* limits = nullptr);
// If `limits` is non-null and the trace has a limits line, *limits is set.
std::optional<std::vector<Command>> read_trace(std::istream& is,
                                               std::string* error = nullptr,
                                               BookLimits* limits = nullptr);
void write_trace_to_stderr(const std::vector<Command>& cmds);

// Renumbers seq to 1..n in order, e.g. after commands were removed.
void renumber(std::vector<Command>& cmds) noexcept;

}  // namespace msim
