#include "msim/lmdb_storage.hpp"

#include <cstring>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
namespace msim {

LMDBStorage::LMDBStorage(const std::string& path, size_t map_size_bytes)
    : path_(path) {
  fs::create_directories(path_);

  int rc = mdb_env_create(&env_);
  if (rc) throw std::runtime_error("mdb_env_create failed");

  mdb_env_set_mapsize(env_, map_size_bytes);
  mdb_env_set_maxdbs(env_, 64);
  rc = mdb_env_open(env_, path_.c_str(), 0, 0);
  if (rc) {
    std::cerr << "[LMDBStorage] mdb_env_open failed (" << rc
              << "): " << mdb_strerror(rc) << " path=" << path_ << std::endl;
    throw std::runtime_error("mdb_env_open failed");
  }

  // Begin first transaction
  rc = mdb_txn_begin(env_, nullptr, 0, &txn_);
  if (rc) throw std::runtime_error("mdb_txn_begin failed");
}

LMDBStorage::~LMDBStorage() {
  try {
    flush();
  } catch (...) {
  }
  if (txn_) mdb_txn_abort(txn_);
  if (env_) mdb_env_close(env_);
}

MDB_dbi LMDBStorage::dbi_for_symbol(const std::string& sym) {
  auto it = dbis_.find(sym);
  if (it != dbis_.end()) return it->second;

  MDB_dbi dbi;
  int rc = mdb_dbi_open(txn_, sym.c_str(), MDB_CREATE, &dbi);
  if (rc)
    throw std::runtime_error("mdb_dbi_open failed: " +
                             std::string(mdb_strerror(rc)));
  dbis_[sym] = dbi;
  return dbi;
}

void LMDBStorage::commit_txn() {
  if (!txn_) return;
  int rc = mdb_txn_commit(txn_);
  if (rc != MDB_SUCCESS) {
    std::cerr << "LMDB commit failed: " << mdb_strerror(rc) << "\n";
    mdb_txn_abort(txn_);  // ensure clean state
  }

  txn_ = nullptr;
  batch_count_ = 0;

  // Immediately start a fresh write txn for continuity
  rc = mdb_txn_begin(env_, nullptr, 0, &txn_);  // restart txn
  if (rc != MDB_SUCCESS) {
    std::cerr << "[LMDBStorage] txn restart failed: " << mdb_strerror(rc)
              << "\n";
    txn_ = nullptr;  // mark unusable
  }
}

void LMDBStorage::write(const Event& e) {
  // Serialize event into a linear byte buffer
  std::vector<uint8_t> buf;
  buf.reserve(e.serialized_size());
  buf = e.serialize();

  // Prepare key = timestamp
  MDB_val key, val;
  key.mv_size = sizeof(e.ts_ns);
  key.mv_data = const_cast<uint64_t*>(&e.ts_ns);

  // Value = serialized bytes
  val.mv_size = buf.size();
  val.mv_data = buf.data();

  // Open or reuse DBI for symbol
  MDB_dbi dbi = dbi_for_symbol(e.symbol);
  int rc = mdb_put(txn_, dbi, &key, &val, 0);
  if (rc) {
    std::cerr << "mdb_put failed: " << mdb_strerror(rc) << "\n";
  }

  if (++batch_count_ >= batch_limit_) {
    commit_txn();
  }
}

void LMDBStorage::flush() {
  if (txn_ && batch_count_ > 0) commit_txn();
}

std::unique_ptr<IStorage> make_lmdb_storage(const std::string& path) {
  return std::make_unique<LMDBStorage>(path);
}

}  // namespace msim
