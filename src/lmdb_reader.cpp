#include "msim/lmdb_reader.hpp"

#include <iostream>
#include <stdexcept>

namespace msim {

LMDBReader::LMDBReader(const std::string& path) {
  int rc = mdb_env_create(&env_);
  if (rc)
    throw std::runtime_error("mdb_env_create failed: " + std::to_string(rc));

  // Allow many DBIs (important when you have one per symbol)
  mdb_env_set_maxdbs(env_, 64);

  rc = mdb_env_open(env_, path.c_str(), MDB_RDONLY, 0664);
  if (rc)
    throw std::runtime_error("mdb_env_open failed: " + std::to_string(rc));

  // Start a read txn and open unnamed meta DB to ensure sub-DB visibility
  rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn_);
  if (rc)
    throw std::runtime_error("mdb_txn_begin failed: " + std::to_string(rc));

  // Important: open unnamed meta-DB once to populate handles
  MDB_dbi main_dbi;
  rc = mdb_dbi_open(txn_, nullptr, 0, &main_dbi);
  if (rc)
    throw std::runtime_error("mdb_dbi_open meta failed: " + std::to_string(rc));
  mdb_dbi_close(env_, main_dbi);
}

LMDBReader::~LMDBReader() {
  if (txn_) mdb_txn_abort(txn_);
  if (env_) mdb_env_close(env_);
}

std::vector<Event> LMDBReader::read_all(const std::string& symbol) {
  MDB_dbi dbi;
  if (mdb_dbi_open(txn_, symbol.c_str(), 0, &dbi) != 0)
    throw std::runtime_error("dbi open failed: " + symbol);

  MDB_cursor* cursor = nullptr;
  mdb_cursor_open(txn_, dbi, &cursor);

  MDB_val key, val;
  std::vector<Event> out;

  while (mdb_cursor_get(cursor, &key, &val, MDB_NEXT) == 0) {
    size_t consumed = 0;
    auto evt = Event::deserialize(static_cast<const uint8_t*>(val.mv_data),
                                  val.mv_size, consumed);
    if (evt) out.push_back(std::move(*evt));
  }

  mdb_cursor_close(cursor);
  mdb_dbi_close(env_, dbi);
  return out;
}

std::vector<std::string> LMDBReader::list_symbols() {
  MDB_dbi dbi;
  if (mdb_dbi_open(txn_, nullptr, 0, &dbi) != 0)
    throw std::runtime_error("dbi_open failed for unnamed DB");

  MDB_cursor* cursor = nullptr;
  mdb_cursor_open(txn_, dbi, &cursor);

  MDB_val key, val;
  std::vector<std::string> names;
  while (mdb_cursor_get(cursor, &key, &val, MDB_NEXT) == 0) {
    names.emplace_back(static_cast<const char*>(key.mv_data), key.mv_size);
  }

  mdb_cursor_close(cursor);
  mdb_dbi_close(env_, dbi);
  return names;
}

}  // namespace msim
