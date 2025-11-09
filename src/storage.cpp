#include "msim/storage.hpp"

#include "msim/lmdb_storage.hpp"

namespace msim {
std::unique_ptr<IStorage> make_storage(const std::string& path) {
  if (path.empty()) return std::make_unique<NullStorage>();

  auto has_mdb_ext =
      path.size() >= 4 && path.compare(path.size() - 4, 4, ".mdb") == 0;
  if (has_mdb_ext || path.find(".mdb/") != std::string::npos)
    return make_lmdb_storage(path);

  return std::make_unique<BinaryLogStorage>(path);
}

}  // namespace msim
