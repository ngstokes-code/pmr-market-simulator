#pragma once
#include "event.hpp"
#include "market.pb.h"

namespace msim {

// Converts internal Event <-> protobuf Event
struct EventConvert {
  static void to_proto(const Event& src, msim::rpc::Event* dst) {
    dst->set_ts_ns(src.ts_ns);
    dst->set_type(static_cast<msim::rpc::EventType>(src.type));
    dst->set_symbol(src.symbol);
    dst->set_price(src.price);
    dst->set_qty(src.qty);
    dst->set_side(static_cast<msim::rpc::Side>(src.side));
  }

  static Event from_proto(const msim::rpc::Event& p) {
    return Event{p.ts_ns(),  static_cast<EventType>(p.type()),
                 p.symbol(), p.price(),
                 p.qty(),    static_cast<Side>(p.side())};
  }
};

}  // namespace msim
