#include "msim/storage.hpp"

#include "msim/lmdb_storage.hpp"

namespace msim {

BinaryLogStorage::BinaryLogStorage(const std::string& path) {
  fp_ = std::fopen(path.c_str(), "wb");
  if (!fp_) throw std::runtime_error("open log failed");
}

BinaryLogStorage::~BinaryLogStorage() {
  flush();
  if (fp_) std::fclose(fp_);
}

void BinaryLogStorage::write(const Event& e) {
  std::lock_guard<std::mutex> lock(mtx_);
  auto b = e.serialize();
  uint32_t n = static_cast<uint32_t>(b.size());
  std::fwrite(&n, sizeof(n), 1, fp_);
  std::fwrite(b.data(), 1, b.size(), fp_);
}

void BinaryLogStorage::flush() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (fp_) std::fflush(fp_);
}

std::unique_ptr<IStorage> make_storage(const std::string& path) {
  if (path.empty()) return std::make_unique<NullStorage>();

  auto has_mdb_ext =
      path.size() >= 4 && path.compare(path.size() - 4, 4, ".mdb") == 0;
  if (has_mdb_ext || path.find(".mdb/") != std::string::npos)
    return make_lmdb_storage(path);

  return std::make_unique<BinaryLogStorage>(path);
}

}  // namespace msim
