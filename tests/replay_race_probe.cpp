// TSan positive control. Never run this intentionally racy program as a
// benchmark.
#include <atomic>
#include <thread>
int main() {
  volatile unsigned buffer = 0;
  std::atomic<unsigned> ready{0};
  auto work = [&] {
    ready.fetch_add(1);
    while (ready.load() != 2) {
    };
    for (unsigned i = 0; i < 100000; ++i) buffer = i;
  };
  std::thread a(work), b(work);
  a.join();
  b.join();
  return buffer == 99999 ? 0 : 1;
}
