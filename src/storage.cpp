#include "storage.hpp"

namespace msim {
std::unique_ptr<IStorage> make_storage(const std::string& path) {
  if (path.empty()) return std::make_unique<NullStorage>();
  return std::make_unique<BinaryLogStorage>(path);
}
}  // namespace msim
