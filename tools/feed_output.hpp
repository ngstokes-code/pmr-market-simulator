#pragma once

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include "msim/feed_handler.hpp"

inline std::uint64_t feed_number(const char* text) {
  std::uint64_t value = 0;
  const char* end = text + std::strlen(text);
  const auto parsed = std::from_chars(text, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("expected an unsigned decimal integer");
  return value;
}

inline void feed_ascii_text(const std::string& text) {
  for (char value : text) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte < 32 || byte > 126)
      throw std::invalid_argument("report field is not printable ASCII");
  }
}

inline std::string feed_stock(const char* bytes) {
  std::string stock(bytes, 8);
  feed_ascii_text(stock);
  while (!stock.empty() && stock.back() == ' ') stock.pop_back();
  if (stock.empty()) throw std::invalid_argument("empty stock label");
  return stock;
}

inline std::string feed_csv_field(const std::string& text) {
  feed_ascii_text(text);
  if (text.find_first_of(",\"") == std::string::npos) return text;
  std::string quoted = "\"";
  for (char value : text) {
    if (value == '"') quoted.push_back('"');
    quoted.push_back(value);
  }
  quoted.push_back('"');
  return quoted;
}

template <typename Book>
void report_feed_fault(const msim::FeedHandler<Book>& h) {
  if (!h.failed()) return;
  std::fprintf(stderr,
      "feed fault: message=%llu offset=%zu type=%c locate=%u ref=%llu reason=%s\n",
      static_cast<unsigned long long>(h.ordinal), h.offset, h.fault_header.type,
      h.fault_header.locate, static_cast<unsigned long long>(h.fault_ref),
      msim::FeedBook::status_name(h.status));
}


// Publication is provisional until the caller has flushed its final success
// record. Exceptions attempt to restore preceding file bytes or remove a
// newly published directory; failed rollback explicitly reports uncertainty.
// Exit status, final console record and requested output must all agree.
// Concurrent writers and crash durability are outside
// this single-owner contract.
class FeedOutput {
 public:
  explicit FeedOutput(const char* path) : destination_(path) {
    existed_ = ::lstat(path, &initial_) == 0;
    temporary_ = destination_ + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temporary_.data());
    if (fd < 0) throw std::runtime_error("cannot create output temporary file");
    file_ = ::fdopen(fd, "w");
    if (!file_) {
      ::close(fd); ::unlink(temporary_.c_str());
      throw std::runtime_error("cannot open output stream");
    }
  }
  FeedOutput(const FeedOutput&) = delete;
  FeedOutput& operator=(const FeedOutput&) = delete;
  ~FeedOutput() {
    if (file_) std::fclose(file_);
    if (published_ && !committed_) {
      if (existed_) {
        if (std::rename(backup_.c_str(),destination_.c_str()) != 0)
          std::fputs("output rollback failed; destination state uncertain\n",stderr);
        else backup_.clear();
      } else if (::unlink(destination_.c_str()) != 0) {
        std::fputs("output rollback failed; destination state uncertain\n",stderr);
      }
    }
    if (!temporary_.empty()) ::unlink(temporary_.c_str());
    if (!backup_.empty() && (!published_ || committed_)) ::unlink(backup_.c_str());
  }
  std::FILE* file() const noexcept { return file_; }
  void publish() {
    const bool failed = std::ferror(file_) != 0;
    const int closed = std::fclose(file_); file_ = nullptr;
    if (failed || closed != 0) throw std::runtime_error("output write failed");
    struct stat now{};
    const bool exists_now = ::lstat(destination_.c_str(),&now) == 0;
    if (exists_now != existed_ || (existed_ &&
        (now.st_dev!=initial_.st_dev || now.st_ino!=initial_.st_ino ||
         now.st_size!=initial_.st_size || now.st_mtime!=initial_.st_mtime)))
      throw std::runtime_error("output destination changed");
    if (existed_) {
      if (S_ISDIR(now.st_mode)) throw std::runtime_error("cannot publish output over a directory");
      backup_ = destination_ + ".rollback.XXXXXX";
      const int fd=::mkstemp(backup_.data());
      if (fd<0) throw std::runtime_error("cannot prepare output rollback");
      ::close(fd); ::unlink(backup_.c_str());
      if (::linkat(AT_FDCWD,destination_.c_str(),AT_FDCWD,backup_.c_str(),0))
        throw std::runtime_error("cannot preserve previous output");
    }
    if (std::rename(temporary_.c_str(),destination_.c_str()))
      throw std::runtime_error("cannot publish output");
    temporary_.clear(); published_=true;
  }
  void commit() noexcept { committed_=true; }
 private:
  std::string destination_,temporary_,backup_;
  struct stat initial_{};
  std::FILE* file_=nullptr;
  bool existed_=false,published_=false,committed_=false;
};

class FeedDirectory {
 public:
  explicit FeedDirectory(const std::string& path) : destination_(path) {
    if (std::filesystem::exists(destination_))
      throw std::runtime_error("report directory already exists");
    auto parent=destination_.parent_path();
    if (parent.empty()) parent=".";
    std::filesystem::create_directories(parent);
    std::string pattern=(parent/(destination_.filename().string()+".tmp.XXXXXX")).string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create report directory");
    temporary_=pattern;
  }
  FeedDirectory(const FeedDirectory&) = delete;
  FeedDirectory& operator=(const FeedDirectory&) = delete;
  ~FeedDirectory() noexcept {
    try {
      std::error_code error;
      if (!temporary_.empty()) std::filesystem::remove_all(temporary_,error);
      if (published_ && !committed_) {
        std::filesystem::remove_all(destination_,error);
        if (error) std::fputs("report rollback failed; destination state uncertain\n",stderr);
      }
    } catch (...) { std::fputs("report cleanup failed; inspect owned output paths\n",stderr); }
  }
  std::filesystem::path path() const { return temporary_; }
  void publish() {
#if defined(__APPLE__)
    const int result=::renamex_np(temporary_.c_str(),destination_.c_str(),RENAME_EXCL);
#elif defined(__linux__)
    const int result=static_cast<int>(::syscall(SYS_renameat2,AT_FDCWD,
        temporary_.c_str(),AT_FDCWD,destination_.c_str(),1U));
#else
#error "Atomic no-replace report publication requires macOS or Linux"
#endif
    if (result) throw std::runtime_error("cannot publish report directory");
    temporary_.clear(); published_=true;
  }
  void commit() noexcept { committed_=true; }
 private:
  std::filesystem::path destination_,temporary_;
  bool published_=false,committed_=false;
};

inline std::string feed_json(const std::string& value) {
  std::string result="\"";
  static constexpr char digits[]="0123456789abcdef";
  for (char c : value) {
    const auto b=static_cast<unsigned char>(c);
    if (c=='"' || c=='\\') { result.push_back('\\'); result.push_back(c); }
    else if (b<32) { result+="\\u00"; result.push_back(digits[b>>4]); result.push_back(digits[b&15]); }
    else result.push_back(c);
  }
  return result+'"';
}

inline void feed_json_write(std::FILE* file,const char* value) noexcept {
  static constexpr char digits[]="0123456789abcdef";
  std::fputc('"',file);
  for (const char* at=value;*at;++at) {
    const auto c=static_cast<unsigned char>(*at);
    if (c=='"' || c=='\\') { std::fputc('\\',file); std::fputc(c,file); }
    else if (c<32) { std::fputs("\\u00",file); std::fputc(digits[c>>4],file); std::fputc(digits[c&15],file); }
    else std::fputc(c,file);
  }
  std::fputc('"',file);
}
