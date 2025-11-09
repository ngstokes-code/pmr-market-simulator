#pragma once
#include <cstdio>
#include <memory>
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
  explicit BinaryLogStorage(const std::string& path) {
    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) throw std::runtime_error("open log failed");
  }
  ~BinaryLogStorage() {
    if (fp_) std::fclose(fp_);
  }
  void write(const Event& e) override {
    auto b = e.serialize();
    uint32_t n = static_cast<uint32_t>(b.size());
    std::fwrite(&n, sizeof(n), 1, fp_);
    std::fwrite(b.data(), 1, b.size(), fp_);
  }
  void flush() override { std::fflush(fp_); }

 private:
  std::FILE* fp_{nullptr};
};

std::unique_ptr<IStorage> make_storage(const std::string& path);

}  // namespace msim
