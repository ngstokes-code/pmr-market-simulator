#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "msim/feed_handler.hpp"
#include "msim/day_references.hpp"

namespace msim {

// Metadata follows file order, not a timestamp sort. This is a supported
// ITCH-5.0 state contract, not proof of source transport continuity or an
// oracle for every venue rule. validate() performs no mutations.
class ProtocolState {
 public:
  enum class Scope { CompleteSession, BoundedFragment };
  struct Symbol {
    std::array<char, 8> stock{};
    bool named = false, directory = false;
    char category = ' ', authenticity = ' ', trading = '?';
    char nasdaq_operation = 'T';  // no Nasdaq operational halt observed
    std::uint32_t round_lot = 0;
    std::uint64_t high_timestamp = 0, clock_regressions = 0;
    std::uint64_t trading_update = 0;
  };
  explicit ProtocolState(Scope scope = Scope::CompleteSession,
                         std::size_t day_blocks = 65536,
                         bool experimental_without_day_identity = false)
      : symbols_(65536), scope_(scope), day_(day_blocks),
        check_day_(scope==Scope::CompleteSession && !experimental_without_day_identity) {}

  const Symbol& symbol(std::uint16_t locate) const noexcept {
    return symbols_[locate];
  }
  char session() const noexcept { return session_; }
  std::uint64_t messages() const noexcept { return messages_; }
  void set_ordinal(std::uint64_t n) noexcept { ordinal_ = n; }
  std::uint64_t clock_regressions() const noexcept { return regressions_; }
  std::size_t day_reference_blocks() const noexcept { return day_.blocks(); }
  std::uint64_t reference_births() const noexcept { return day_.births(); }
  template<class F> void for_each_reference(F&& visit) const { day_.for_each(visit); }
  bool complete() const noexcept { return event_mask_ == 63 && first_was_open_; }
  bool trading(std::uint16_t locate) const noexcept {
    const auto& s = symbols_[locate];
    return session_ == 'Q' && s.trading == 'T' && s.nasdaq_operation == 'T' &&
           s.trading_update > last_breach_;
  }

  static std::string label(const char* stock) {
    std::string value(stock, 8);
    for (char byte : value) {
      const auto c = static_cast<unsigned char>(byte);
      if (c < 32 || c > 126) throw std::runtime_error("non-ASCII stock identity");
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (value.empty()) throw std::runtime_error("empty stock identity");
    return value;
  }

  void validate(const itch::Header& h) const {
    if (h.timestamp >= 86'400'000'000'000ULL)
      throw std::runtime_error("timestamp outside a day");
    if (session_ == 'C') throw std::runtime_error("message after End of Messages");
    if (!messages_ && scope_ == Scope::CompleteSession && h.type != 'S')
      throw std::runtime_error("input must begin with Start of Messages");
    if (h.type == 'S' || h.type=='V' || h.type=='W' || h.type=='K') {
      if (h.locate != 0) throw std::runtime_error("market-wide message has nonzero locate");
    } else if (h.locate == 0) {
      throw std::runtime_error("symbol message has zero locate");
    }
    if (h.locate && h.type != 'R' && scope_ == Scope::CompleteSession &&
        !symbols_[h.locate].directory)
      throw std::runtime_error("symbol event precedes its directory");
  }
  void validate(const itch::SystemEvent& m) const {
    validate(static_cast<const itch::Header&>(m));
    static constexpr char order[] = "OSQMEC";
    const char* at = std::strchr(order, m.code);
    if (!at || m.code == '\0') throw std::runtime_error("invalid system event");
    const auto next = static_cast<unsigned>(at - order) + 1;
    if (next <= sequence_ ||
        (scope_ == Scope::CompleteSession && next != sequence_ + 1))
      throw std::runtime_error("system events out of order");
  }
  template <class M> void validate(const M& m) const {
    validate(static_cast<const itch::Header&>(m));
  }
  void validate(const itch::StockDirectory& m) const {
    validate(static_cast<const itch::Header&>(m));
    validate_stock(m.locate, m.stock);
    if (m.authenticity!='P' && m.authenticity!='T')
      throw std::runtime_error("invalid directory authenticity");
    const auto category=std::string("QGSNAPZV ");
    if (category.find(m.market_category)==std::string::npos)
      throw std::runtime_error("invalid market category");
    const auto key = std::string(m.stock, 8);
    const auto found = locates_.find(key);
    if (found != locates_.end() && found->second != m.locate)
      throw std::runtime_error("stock identity occurs at another locate");
    const auto& s = symbols_[m.locate];
    if (s.directory && (s.category != m.market_category ||
        s.round_lot != m.round_lot || s.authenticity != m.authenticity))
      throw std::runtime_error("directory metadata changed");
  }
  void validate(const itch::AddOrder& m) const {
    named(m); validate_birth(); if (check_day_) day_.validate_new(m.ref);
  }
  void validate(const itch::OrderReplace& m) const {
    validate(static_cast<const itch::Header&>(m));
    validate_birth();
    if (check_day_) day_.validate_new(m.new_ref);
  }
  void validate(const itch::Trade& m) const { named(m); }
  void validate(const itch::CrossTrade& m) const { named(m); }
  void validate(const itch::TradingAction& m) const { named(m); }
  void validate(const itch::OperationalHalt& m) const { named(m); }
  void validate(const itch::Imbalance& m) const { named(m); }
  void validate(const itch::CircuitBreaker& m) const {
    validate(static_cast<const itch::Header&>(m));
    if (m.locate) throw std::runtime_error("circuit breaker has nonzero locate");
  }

  void commit(const itch::Header& h) {
    ++messages_;
    if (h.locate) {
      auto& s = symbols_[h.locate];
      if (h.timestamp < s.high_timestamp) { ++s.clock_regressions; ++regressions_; }
      else s.high_timestamp = h.timestamp;
    }
  }
  template <class M> void commit(const M& m) {
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::SystemEvent& m) {
    if (!messages_ && m.code == 'O') first_was_open_ = true;
    static constexpr char order[] = "OSQMEC";
    sequence_ = static_cast<unsigned>(std::strchr(order, m.code) - order) + 1;
    event_mask_ |= 1U << (sequence_ - 1);
    session_ = m.code;
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::StockDirectory& m) {
    auto& s = symbols_[m.locate];
    remember(m.locate, m.stock); s.directory = true;
    s.category = m.market_category; s.round_lot = m.round_lot;
    s.authenticity = m.authenticity;
    locates_.emplace(std::string(m.stock, 8), m.locate);
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::TradingAction& m) {
    remember(m.locate, m.stock); symbols_[m.locate].trading = m.state;
    symbols_[m.locate].trading_update = ordinal_ ? ordinal_ : messages_ + 1;
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::OperationalHalt& m) {
    remember(m.locate, m.stock);
    if (m.market == 'Q') symbols_[m.locate].nasdaq_operation = m.action;
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::AddOrder& m) {
    if (check_day_) day_.insert(m.ref);
    named_commit(m);
  }
  void commit(const itch::OrderReplace& m) {
    if (check_day_) day_.insert(m.new_ref);
    commit(static_cast<const itch::Header&>(m));
  }
  void commit(const itch::Trade& m) { named_commit(m); }
  void commit(const itch::CrossTrade& m) { named_commit(m); }
  void commit(const itch::Imbalance& m) { named_commit(m); }
  void commit(const itch::CircuitBreaker& m) {
    last_breach_ = ordinal_ ? ordinal_ : messages_ + 1;
    commit(static_cast<const itch::Header&>(m));
  }
  void finish() const {
    if (!messages_) throw std::runtime_error("empty input");
    if (scope_ == Scope::CompleteSession && !complete())
      throw std::runtime_error("incomplete session: terminal End of Messages required");
  }

 private:
  void validate_birth() const {
    if (scope_==Scope::CompleteSession && session_!='S' && session_!='Q' && session_!='M')
      throw std::runtime_error("order birth outside system hours");
  }
  void validate_stock(std::uint16_t loc, const char* stock) const {
    (void)label(stock);
    if (symbols_[loc].named && std::memcmp(symbols_[loc].stock.data(), stock, 8))
      throw std::runtime_error("stock identity disagrees with locate");
  }
  template <class M> void named(const M& m) const {
    validate(static_cast<const itch::Header&>(m));
    validate_stock(m.locate, m.stock);
  }
  void remember(std::uint16_t loc, const char* stock) {
    std::memcpy(symbols_[loc].stock.data(), stock, 8);
    symbols_[loc].named = true;
  }
  template <class M> void named_commit(const M& m) {
    remember(m.locate, m.stock); commit(static_cast<const itch::Header&>(m));
  }
  std::vector<Symbol> symbols_;
  std::map<std::string, std::uint16_t> locates_;
  Scope scope_;
  char session_ = '?';
  unsigned sequence_ = 0;
  unsigned event_mask_ = 0;
  bool first_was_open_ = false;
  std::uint64_t messages_ = 0, regressions_ = 0;
  std::uint64_t last_breach_ = 0;
  std::uint64_t ordinal_ = 0;
  DayReferences day_;
  bool check_day_;
};

// Dispatch shared by coordinator/worker/research, with metadata validation
// before book mutation. Allocation exceptions invalidate the whole activity.
// Bounded independent references separately verify these transition rules.
template <class Book>
struct MarketHandler {
  Book& book;
  ProtocolState state;
  FeedHandler<Book> lifecycle;
  bool invalid = false;
  explicit MarketHandler(Book& b, ProtocolState::Scope scope =
                         ProtocolState::Scope::CompleteSession)
      : book(b), state(scope), lifecycle(b) {}
  template <class M> void on(const M& m) {
    if (invalid) throw std::runtime_error("handler already invalid");
    try {
      state.validate(m);
      lifecycle.on(m);
      if (lifecycle.failed())
        throw std::runtime_error(FeedBook::status_name(lifecycle.status));
      state.commit(m);
    } catch (...) { invalid = true; throw; }
  }
  void on_other(const itch::Header& h) {
    if (invalid) throw std::runtime_error("handler already invalid");
    try { state.validate(h); state.commit(h); }
    catch (...) { invalid = true; throw; }
  }
  void set_context(std::size_t offset, std::uint64_t ordinal) noexcept {
    state.set_ordinal(ordinal);
    lifecycle.set_context(offset, ordinal);
  }
  bool failed() const noexcept { return invalid || lifecycle.failed(); }
};

}  // namespace msim
