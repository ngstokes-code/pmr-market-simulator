#pragma once

// Decoder for Nasdaq TotalView-ITCH 5.0, as described in Nasdaq's public
// specification. Input is the file layout Nasdaq uses for its published
// sample days: each message is a 2-byte big-endian length, then the message.
//
// Integers are big-endian. Prices are Price(4): an integer count of
// 1/10000 dollar. Timestamps are nanoseconds since midnight in 6 bytes.

#include <cstddef>
#include <cstdint>

namespace msim::itch {

using Price = std::uint32_t;
using Ref = std::uint64_t;

namespace detail {

inline std::uint16_t be16(const unsigned char* p) noexcept {
  return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}
inline std::uint32_t be32(const unsigned char* p) noexcept {
  return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 |
         std::uint32_t{p[2]} << 8 | std::uint32_t{p[3]};
}
inline std::uint64_t be48(const unsigned char* p) noexcept {
  return std::uint64_t{be16(p)} << 32 | be32(p + 2);
}
inline std::uint64_t be64(const unsigned char* p) noexcept {
  return std::uint64_t{be32(p)} << 32 | be32(p + 4);
}

}  // namespace detail

// Fields every message carries at the same offsets.
struct Header {
  char type;
  std::uint16_t locate;  // stock locate code, 0 for market-wide messages
  std::uint64_t timestamp;
};

struct SystemEvent : Header {
  char code;  // O S Q M E C
};

struct StockDirectory : Header {
  char stock[8];
  char market_category;  // Q G S are Nasdaq-listed
  std::uint32_t round_lot;
  char authenticity{' '};
};

struct TradingAction : Header {
  char stock[8];
  char state;  // H halted, P paused, Q quotation only, T trading
};

struct OperationalHalt : Header {
  char stock[8];
  char market;
  char action;
};
struct CircuitBreaker : Header { char level; };

struct Imbalance : Header {
  std::uint64_t paired, imbalance;
  char direction;
  char stock[8];
  Price far_price, near_price, reference_price;
  char cross_type, variation;
};

struct AddOrder : Header {
  Ref ref;
  char side;  // 'B' or 'S'
  std::uint32_t shares;
  char stock[8];
  Price price;
  bool attributed;  // type F; the MPID itself is not kept
};

struct OrderExecuted : Header {
  Ref ref;
  std::uint32_t shares;
  std::uint64_t match;
};

struct OrderExecutedWithPrice : Header {
  Ref ref;
  std::uint32_t shares;
  std::uint64_t match;
  bool printable;
  Price price;
};

struct OrderCancel : Header {
  Ref ref;
  std::uint32_t shares;
};

struct OrderDelete : Header {
  Ref ref;
};

struct OrderReplace : Header {
  Ref old_ref;
  Ref new_ref;
  std::uint32_t shares;
  Price price;
};

// Execution against a non-displayed order. The book never held it.
struct Trade : Header {
  std::uint32_t shares;
  char stock[8];
  Price price;
  std::uint64_t match;
};

struct CrossTrade : Header {
  std::uint64_t shares;
  char stock[8];
  Price price;
  std::uint64_t match;
  char cross_type;  // O opening, C closing, H halt or IPO
};

struct BrokenTrade : Header {
  std::uint64_t match;
};

// Message length for each type the decoder understands, 0 for the rest.
// Types the decoder does not model still frame correctly and are counted.
constexpr std::uint16_t expected_length(char type) noexcept {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'h': return 21;
    case 'I': return 50;
    case 'W': return 12;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    default: return 0;
  }
}

// These messages have no displayed-order lifecycle transition. Their
// lengths are checked; their payloads are not interpreted by this decoder.
constexpr std::uint16_t ignored_length(char type) noexcept {
  switch (type) {
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'K': return 28;
    case 'J': return 35;
    case 'N': return 20;
    default: return 0;
  }
}

enum class DecodeStatus { Decoded, BadLength, BadField, Unsupported };

inline const char* decode_status_name(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Decoded: return "decoded";
    case DecodeStatus::BadLength: return "bad_length";
    case DecodeStatus::BadField: return "bad_field";
    case DecodeStatus::Unsupported: return "unsupported_type";
  }
  return "invalid_status";
}

struct ParseStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;      // consumed, framing included
  std::uint64_t other = 0;      // well-framed messages of unmodelled types
  // Parsing stops on the first invalid or unsupported message.
  std::uint64_t bad_length = 0;
  std::uint64_t bad_field = 0;
  std::uint64_t unsupported = 0;
  std::size_t error_offset = 0;
  bool truncated = false;       // input ended inside a frame
};

// Decodes one message, without its length prefix, and passes it to h:
// h.on(message) for each modelled type, h.on_other(header) for the rest.
// Calls nothing on invalid fields, wrong lengths, or unsupported types.
template <typename Handler>
DecodeStatus decode_checked(const unsigned char* m, std::size_t len, Handler& h) {
  using namespace detail;
  if (len < 11) return DecodeStatus::BadLength;
  if (be48(m + 5) >= 86'400'000'000'000ULL)
    return DecodeStatus::BadField;
  const char type = static_cast<char>(m[0]);
  const std::uint16_t want = expected_length(type);
  if (want == 0) {
    const std::uint16_t ignored = ignored_length(type);
    if (ignored == 0) return DecodeStatus::Unsupported;
    if (len != ignored) return DecodeStatus::BadLength;
    h.on_other(Header{type, be16(m + 1), be48(m + 5)});
    return DecodeStatus::Decoded;
  }
  if (len != want) return DecodeStatus::BadLength;
  if (type=='R' && (m[29]!='P' && m[29]!='T')) return DecodeStatus::BadField;
  if (type=='R' && m[19]!='Q' && m[19]!='G' && m[19]!='S' && m[19]!='N' &&
      m[19]!='A' && m[19]!='P' && m[19]!='Z' && m[19]!='V' && m[19]!=' ')
    return DecodeStatus::BadField;
  if ((type == 'A' || type == 'F') && m[19] != 'B' && m[19] != 'S')
    return DecodeStatus::BadField;
  if (type == 'C' && m[31] != 'Y' && m[31] != 'N')
    return DecodeStatus::BadField;
  if (type == 'H' && m[19] != 'H' && m[19] != 'P' &&
      m[19] != 'Q' && m[19] != 'T') return DecodeStatus::BadField;
  if (type == 'S' && m[11] != 'O' && m[11] != 'S' && m[11] != 'Q' &&
      m[11] != 'M' && m[11] != 'E' && m[11] != 'C') return DecodeStatus::BadField;
  if (type == 'Q' && m[39] != 'O' && m[39] != 'C' && m[39] != 'H')
    return DecodeStatus::BadField;
  constexpr Price max_price = 2'000'000'000;
  if (((type == 'A' || type == 'F' || type == 'P' || type == 'C') &&
        be32(m + 32) > max_price) ||
      (type == 'U' && be32(m + 31) > max_price) ||
      (type == 'Q' && be32(m + 27) > max_price) ||
      (type == 'I' && (be32(m + 36) > max_price ||
                      be32(m + 40) > max_price || be32(m + 44) > max_price)))
    return DecodeStatus::BadField;
  if (type == 'h' && ((m[19] != 'Q' && m[19] != 'B' && m[19] != 'X') ||
                      (m[20] != 'H' && m[20] != 'T')))
    return DecodeStatus::BadField;
  if (type == 'W' && m[11] != '1' && m[11] != '2' && m[11] != '3')
    return DecodeStatus::BadField;
  if (type == 'I') {
    const char direction = static_cast<char>(m[27]);
    const char cross = static_cast<char>(m[48]);
    const char variation = static_cast<char>(m[49]);
    if (direction != 'B' && direction != 'S' && direction != 'N' &&
        direction != 'O' && direction != 'P') return DecodeStatus::BadField;
    if (cross != 'O' && cross != 'C' && cross != 'H' && cross != 'A')
      return DecodeStatus::BadField;
    if (variation != ' ' && variation != 'L' &&
        !(variation >= '1' && variation <= '9') &&
        !(variation >= 'A' && variation <= 'C')) return DecodeStatus::BadField;
  }

  const Header hd{type, be16(m + 1), be48(m + 5)};
  const auto copy8 = [m](char* dst, std::size_t at) {
    for (std::size_t i = 0; i < 8; ++i) dst[i] = static_cast<char>(m[at + i]);
  };
  switch (type) {
    case 'A':
    case 'F': {
      AddOrder a{hd, be64(m + 11), static_cast<char>(m[19]), be32(m + 20),
                 {}, be32(m + 32), type == 'F'};
      copy8(a.stock, 24);
      h.on(a);
      break;
    }
    case 'E':
      h.on(OrderExecuted{hd, be64(m + 11), be32(m + 19), be64(m + 23)});
      break;
    case 'C':
      h.on(OrderExecutedWithPrice{hd, be64(m + 11), be32(m + 19),
                                  be64(m + 23), m[31] == 'Y', be32(m + 32)});
      break;
    case 'X':
      h.on(OrderCancel{hd, be64(m + 11), be32(m + 19)});
      break;
    case 'D':
      h.on(OrderDelete{hd, be64(m + 11)});
      break;
    case 'U':
      h.on(OrderReplace{hd, be64(m + 11), be64(m + 19), be32(m + 27),
                        be32(m + 31)});
      break;
    case 'P': {
      Trade t{hd, be32(m + 20), {}, be32(m + 32), be64(m + 36)};
      copy8(t.stock, 24);
      h.on(t);
      break;
    }
    case 'Q': {
      CrossTrade q{hd, be64(m + 11), {}, be32(m + 27), be64(m + 31),
                   static_cast<char>(m[39])};
      copy8(q.stock, 19);
      h.on(q);
      break;
    }
    case 'B':
      h.on(BrokenTrade{hd, be64(m + 11)});
      break;
    case 'W':
      h.on(CircuitBreaker{hd, static_cast<char>(m[11])});
      break;
    case 'S':
      h.on(SystemEvent{hd, static_cast<char>(m[11])});
      break;
    case 'R': {
      StockDirectory r{hd, {}, static_cast<char>(m[19]), be32(m + 21),
                       static_cast<char>(m[29])};
      copy8(r.stock, 11);
      h.on(r);
      break;
    }
    case 'H': {
      TradingAction t{hd, {}, static_cast<char>(m[19])};
      copy8(t.stock, 11);
      h.on(t);
      break;
    }
    case 'h': {
      OperationalHalt halt{hd, {}, static_cast<char>(m[19]),
                            static_cast<char>(m[20])};
      copy8(halt.stock, 11);
      h.on(halt);
      break;
    }
    case 'I': {
      Imbalance indication{hd, be64(m + 11), be64(m + 19),
          static_cast<char>(m[27]), {}, be32(m + 36), be32(m + 40),
          be32(m + 44), static_cast<char>(m[48]), static_cast<char>(m[49])};
      copy8(indication.stock, 28);
      h.on(indication);
      break;
    }
  }
  return DecodeStatus::Decoded;
}

template <typename Handler>
bool decode(const unsigned char* m, std::size_t len, Handler& h) {
  return decode_checked(m, len, h) == DecodeStatus::Decoded;
}

namespace detail {
template <typename H>
auto failed(const H& h, int) -> decltype(h.failed()) { return h.failed(); }
template <typename H>
bool failed(const H&, long) { return false; }
template <typename H>
auto context(H& h, std::size_t at, std::uint64_t ordinal, int)
    -> decltype(h.set_context(at, ordinal), void()) { h.set_context(at, ordinal); }
template <typename H>
void context(H&, std::size_t, std::uint64_t, long) {}
}  // namespace detail

// Walks length-prefixed messages in [data, data + size) and decodes each.
// Stops early, setting `truncated`, if the input ends inside a frame or a
// frame is too short to hold a stock locate.
template <typename Handler>
ParseStats parse(const unsigned char* data, std::size_t size, Handler& h) {
  ParseStats s;
  std::size_t at = 0;
  while (at + 2 <= size) {
    const std::size_t len = detail::be16(data + at);
    if (len < 3 || at + 2 + len > size) {
      s.truncated = true;
      s.error_offset = at;
      break;
    }
    const unsigned char* m = data + at + 2;
    ++s.messages;
    detail::context(h, at, s.messages, 0);
    const DecodeStatus status = decode_checked(m, len, h);
    if (status != DecodeStatus::Decoded) {
      s.bad_length += status == DecodeStatus::BadLength;
      s.bad_field += status == DecodeStatus::BadField;
      s.unsupported += status == DecodeStatus::Unsupported;
      s.error_offset = at;
      s.bytes = at;
      return s;
    }
    if (expected_length(static_cast<char>(m[0])) == 0)
      ++s.other;
    at += 2 + len;
    if (detail::failed(h, 0)) { s.bytes = at; return s; }
  }
  s.bytes = at;
  if (at < size && !s.truncated) { s.truncated = true; s.error_offset = at; }
  return s;
}

}  // namespace msim::itch
