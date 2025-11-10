#pragma once
#include <lmdb.h>

#include <string>
#include <unordered_map>

#include "event.hpp"
#include "storage.hpp"

namespace msim {

class LMDBStorage final : public IStorage {
  MDB_env* env_ = nullptr;
  MDB_txn* txn_ = nullptr;
  std::unordered_map<std::string, MDB_dbi> dbis_;
  std::string path_;
  size_t batch_count_ = 0;
  const size_t batch_limit_ = 10000;  // commit every 10k writes

 public:
  explicit LMDBStorage(const std::string& path,
                       size_t map_size_bytes = (1ull << 30));
  ~LMDBStorage() override;

  void write(const Event& e) override;
  void flush() override;

 private:
  MDB_dbi dbi_for_symbol(const std::string& sym);
  void commit_txn();
};

std::unique_ptr<IStorage> make_lmdb_storage(const std::string& path);

}  // namespace msim
