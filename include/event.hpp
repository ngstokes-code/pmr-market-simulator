
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <optional>

namespace msim {

enum class EventType : uint8_t { ORDER_ADD=1, ORDER_CANCEL=2, TRADE=3 };

struct Event {
    uint64_t ts_ns{};
    EventType type{};
    std::string symbol;
    double price{};
    int32_t qty{};
    char side{}; // 'B' or 'S'

    std::vector<uint8_t> serialize() const {
        uint16_t sl = static_cast<uint16_t>(symbol.size());
        std::vector<uint8_t> out;
        auto reserve = 2 + sl + sizeof(ts_ns) + 1 + sizeof(price) + sizeof(qty) + 1;
        out.reserve(reserve);
        auto put = [&](auto v){ auto* p = reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(), p, p+sizeof(v)); };
        out.push_back(static_cast<uint8_t>(sl & 0xFF));
        out.push_back(static_cast<uint8_t>((sl >> 8) & 0xFF));
        out.insert(out.end(), symbol.begin(), symbol.end());
        put(ts_ns);
        out.push_back(static_cast<uint8_t>(type));
        put(price);
        put(qty);
        out.push_back(static_cast<uint8_t>(side));
        return out;
    }

    static std::optional<Event> deserialize(const uint8_t* data, size_t len, size_t& consumed) {
        if (len < 2) return std::nullopt;
        uint16_t sl = data[0] | (uint16_t(data[1])<<8);
        if (len < 2 + sl + sizeof(uint64_t) + 1 + sizeof(double) + sizeof(int32_t) + 1) return std::nullopt;
        size_t off = 2;
        std::string sym(reinterpret_cast<const char*>(data+off), sl); off += sl;
        uint64_t ts; std::memcpy(&ts, data+off, sizeof(ts)); off += sizeof(ts);
        uint8_t t = data[off++];
        double price; std::memcpy(&price, data+off, sizeof(price)); off += sizeof(price);
        int32_t qty; std::memcpy(&qty, data+off, sizeof(qty)); off += sizeof(qty);
        char side = static_cast<char>(data[off++]);
        consumed = off;
        return Event{ts, static_cast<EventType>(t), std::move(sym), price, qty, side};
    }
};

} // namespace msim
