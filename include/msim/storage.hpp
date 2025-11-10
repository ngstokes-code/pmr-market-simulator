#pragma once
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "event.hpp"

namespace msim {

struct IStorage {
  virtual ~IStorage() = default;
  virtual void write(const Event& e) = 0;
  virtual void flush() = 0;
};

struct NullStorage : IStorage {
  void write(const Event&) override {}
  void flush() override {}
};

class BinaryLogStorage : public IStorage {
 public:
  explicit BinaryLogStorage(const std::string& path);
  ~BinaryLogStorage();

  void write(const Event& e) override;
  void flush() override;

 private:
  std::FILE* fp_{nullptr};
  std::mutex mtx_;
  std::vector<char> buf_;
  size_t batch_bytes_;
};

std::unique_ptr<IStorage> make_storage(const std::string& path);

}  // namespace msim
