#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace msim {

enum class EventType : uint8_t { ORDER_ADD = 1, ORDER_CANCEL = 2, TRADE = 3 };

struct Event {
  uint64_t ts_ns{};
  EventType type{};
  std::string symbol;
  double price{};
  int32_t qty{};
  char side{};  // 'B' or 'S'

  std::string to_string() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "[%s] %s %.2f x %d (%c) t=%llu",
             (type == EventType::ORDER_ADD      ? "ADD"
              : type == EventType::ORDER_CANCEL ? "CXL"
                                                : "TRD"),
             symbol.c_str(), price, qty, side, (unsigned long long)ts_ns);
    return buf;
  }

  size_t serialized_size() const noexcept {
    // symbol length (2) + symbol bytes + ts + eventType + price + qty + side
    return 2 + symbol.size() + sizeof(ts_ns) + sizeof(EventType) +
           sizeof(price) + sizeof(qty) + sizeof(side);
  }

  std::vector<uint8_t> serialize() const {
    /** serialize() data between its C++ in-memory representation and a compact,
     * linear array of bytes. This process is called serialization. */
    uint16_t sl = static_cast<uint16_t>(symbol.size());
    std::vector<uint8_t> out;
    out.reserve(serialized_size());

    // 1. symbol length (2 bytes)
    out.push_back(static_cast<uint8_t>(sl & 0xFF));
    out.push_back(static_cast<uint8_t>((sl >> 8) & 0xFF));

    // 2. symbol bytes
    out.insert(out.end(), symbol.begin(), symbol.end());

    // 3. remaining fields
    auto put = [&](auto v) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
      out.insert(out.end(), p, p + sizeof(v));
    };
    put(ts_ns);
    out.push_back(static_cast<uint8_t>(type));  // 1 byte
    put(price);
    put(qty);
    out.push_back(static_cast<uint8_t>(side));  // 1 byte
    return out;
  }

  static std::optional<Event> deserialize(const uint8_t* data, size_t len,
                                          size_t& consumed) {
    /** deserialize() a compact linear array of bytes, into its C++ in-memory
     * representation. This process is called deserialization. */
    if (len < 2) return std::nullopt;
    uint16_t sl = data[0] | (uint16_t(data[1]) << 8);
    if (len <
        2 + sl + sizeof(uint64_t) + 1 + sizeof(double) + sizeof(int32_t) + 1)
      return std::nullopt;
    size_t off = 2;
    std::string sym(reinterpret_cast<const char*>(data + off), sl);
    off += sl;
    uint64_t ts;
    std::memcpy(&ts, data + off, sizeof(ts));
    off += sizeof(ts);
    uint8_t t = data[off++];
    double price;
    std::memcpy(&price, data + off, sizeof(price));
    off += sizeof(price);
    int32_t qty;
    std::memcpy(&qty, data + off, sizeof(qty));
    off += sizeof(qty);
    char side = static_cast<char>(data[off++]);
    consumed = off;
    return Event{ts,  static_cast<EventType>(t), std::move(sym), price, qty,
                 side};
  }
};

}  // namespace msim
