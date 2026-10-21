#include "msim/command.hpp"

#include <iostream>
#include <istream>
#include <ostream>
#include <sstream>

namespace msim {

std::string to_string(const Command& c) {
  std::ostringstream os;
  os << c.seq;
  if (c.type == CommandType::Add)
    os << " add " << c.id << ' ' << (c.side == Side::BUY ? 'B' : 'S') << ' '
       << c.tick << ' ' << c.qty;
  else
    os << " cancel " << c.id;
  return os.str();
}

std::string to_string(const Outcome& o) {
  if (o.type == CommandType::Cancel)
    return o.canceled ? "canceled" : "not_found";
  std::ostringstream os;
  os << to_string(o.add.status) << " filled=" << o.add.filled
     << " rested=" << o.add.rested;
  return os.str();
}

std::string to_string(const Execution& x) {
  std::ostringstream os;
  os << "exec resting=" << x.resting_id << " incoming=" << x.incoming_id
     << " tick=" << x.tick << " qty=" << x.qty;
  return os.str();
}

void write_trace(std::ostream& os, const std::vector<Command>& cmds,
                 const BookLimits* limits) {
  os << "# msim-trace 1\n";
  if (limits)
    os << "# limits " << limits->max_resting_orders << ','
       << limits->max_levels_per_side << ',' << limits->max_tick << '\n';
  for (const Command& c : cmds) os << to_string(c) << '\n';
}

std::optional<std::vector<Command>> read_trace(std::istream& is,
                                               std::string* error,
                                               BookLimits* limits) {
  std::vector<Command> cmds;
  std::string line;
  for (std::size_t lineno = 1; std::getline(is, line); ++lineno) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    if (line[first] == '#') {
      std::istringstream in(line.substr(first + 1));
      std::string word;
      char c1 = 0, c2 = 0;
      BookLimits l;
      if (limits && in >> word && word == "limits" &&
          in >> l.max_resting_orders >> c1 >> l.max_levels_per_side >> c2 >>
              l.max_tick &&
          c1 == ',' && c2 == ',')
        *limits = l;
      continue;
    }

    std::istringstream in(line);
    Command c;
    std::string verb;
    bool ok = static_cast<bool>(in >> c.seq >> verb >> c.id);
    if (ok && verb == "add") {
      char side = 0;
      ok = static_cast<bool>(in >> side >> c.tick >> c.qty) &&
           (side == 'B' || side == 'S');
      c.type = CommandType::Add;
      c.side = side == 'B' ? Side::BUY : Side::SELL;
    } else if (ok && verb == "cancel") {
      c.type = CommandType::Cancel;
    } else {
      ok = false;
    }
    std::string extra;
    if (!ok || in >> extra) {
      if (error) *error = "line " + std::to_string(lineno) + ": " + line;
      return std::nullopt;
    }
    cmds.push_back(c);
  }
  return cmds;
}

void write_trace_to_stderr(const std::vector<Command>& cmds) {
  write_trace(std::cerr, cmds);
}

void renumber(std::vector<Command>& cmds) noexcept {
  for (std::size_t i = 0; i < cmds.size(); ++i) cmds[i].seq = i + 1;
}

}  // namespace msim
