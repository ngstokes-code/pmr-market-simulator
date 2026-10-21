#pragma once

// Test assertions that stay active in every build type. `assert` compiles
// away under NDEBUG, which made Release test runs check nothing.

#include <cstdio>

namespace check {

inline int& failures() {
  static int n = 0;
  return n;
}

inline bool report(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    ++failures();
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }
  return ok;
}

// Call at the end of main.
inline int finish(const char* suite) {
  if (failures() == 0) {
    std::printf("OK: %s\n", suite);
    return 0;
  }
  std::fprintf(stderr, "FAIL: %s (%d failed checks)\n", suite, failures());
  return 1;
}

}  // namespace check

#define CHECK(cond) \
  ::check::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

// For checks whose failure makes the rest of the test meaningless.
#define REQUIRE(cond)         \
  do {                        \
    if (!CHECK(cond)) return; \
  } while (0)
