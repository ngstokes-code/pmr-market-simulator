#include "msim/book_types.hpp"

namespace msim {

const char* to_string(AddStatus s) noexcept {
  switch (s) {
    case AddStatus::Accepted:
      return "accepted";
    case AddStatus::InvalidQty:
      return "invalid_qty";
    case AddStatus::InvalidTick:
      return "invalid_tick";
    case AddStatus::DuplicateId:
      return "duplicate_id";
    case AddStatus::CapacityFull:
      return "capacity_full";
    case AddStatus::InvalidSide:
      return "invalid_side";
  }
  return "unknown";
}

}  // namespace msim
