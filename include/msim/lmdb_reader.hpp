#pragma once
#include <lmdb.h>

#include <string>
#include <vector>

#include "msim/event.hpp"

namespace msim {

class LMDBReader {
 public:
  explicit LMDBReader(const std::string& path);
  ~LMDBReader();

  std::vector<Event> read_all(const std::string& symbol);
  std::vector<std::string> list_symbols();

 private:
  MDB_env* env_ = nullptr;
  MDB_txn* txn_ = nullptr;
};

}  // namespace msim
