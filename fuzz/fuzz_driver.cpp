// Runs the fuzz target without libFuzzer: on each file named on the command
// line, or else on --runs N seeded random inputs. CTest uses the second form
// so the target is exercised in every build.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "msim/rng.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

int main(int argc, char** argv) {
  std::size_t runs = 0;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--runs" && i + 1 < argc)
      runs = std::stoul(argv[++i]);
    else
      files.push_back(a);
  }

  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(in),
                                          {});
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
  }

  Xoroshiro128Plus rng(2024);
  std::vector<std::uint8_t> buf;
  for (std::size_t r = 0; r < runs; ++r) {
    buf.resize(1 + 4 * (1 + rand_index(rng, 200)));
    for (auto& b : buf) b = static_cast<std::uint8_t>(rng.next_u64());
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
  }
  std::printf("OK: fuzz driver (%zu files, %zu random inputs)\n", files.size(),
              runs);
  return 0;
}
