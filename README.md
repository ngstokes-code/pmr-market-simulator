# PMR Market Simulator (C++17)

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)
![Build](https://img.shields.io/badge/Build-CMake-darkblue.svg)
![Arena](https://img.shields.io/badge/Memory-std%3A%3Apmr%20Monotonic-success.svg)
![Performance](https://img.shields.io/badge/Throughput-~42M%20ev%2Fs%20(6T%20pinned)-brightgreen.svg)
![Storage](https://img.shields.io/badge/Storage-LMDB-critical.svg)
![Streaming](https://img.shields.io/badge/Streaming-gRPC-blueviolet.svg)

A **high-performance, allocator-aware market microstructure simulator** with per-symbol arenas, NUMA pinning, and a fast per-symbol **limit order book**. Designed for **repeatable benchmarking**, correctness, and maintainable hot-path code.

**Current performance (Windows / MSVC / Ryzen 7 5800X, 6 pinned threads):**
- **Best:** 42.4M events/sec  
- **Avg:** 41.2M events/sec  
(6 symbols, 2,000,000 events, `sigma=0.001`, 1 MiB arena per symbol, `--no-log`)

> Results vary by CPU, compiler, and flags. Logging / I/O will reduce throughput drastically.

---

## What’s in here

- **Limit Order Book (LOB)**
  - Strict **price-time priority**
  - Internal **tick-based prices** (`int32_t tick`) for determinism and speed
  - **Flat hash** price levels + pooled level reuse (no `std::map<double>` pointer chasing)
  - Cancel index maintained for correctness (filled resting orders removed from index)

- **Simulation Engine**
  - Multi-threaded event generation and application (one symbol per thread by default)
  - Deterministic ID + timestamp generation in benchmark mode (no realtime clock in hot loop)

- **Performance / Memory**
  - **Per-symbol** `std::pmr::monotonic_buffer_resource` arenas
  - NUMA pinning support (best-effort on Windows/Linux)
  - Bounded **SPSC** ring buffer implementation + unit tests (building block for future pipelining)

- **Storage / Streaming (optional)**
  - LMDB-backed persistence + replay mode
  - gRPC streaming support for downstream collectors (kept out of hot loop unless enabled)

---

## Repo layout (high level)

- `include/msim/`
  - `order_book.hpp` — core order book API + structures
  - `flat_hash.hpp` — fixed-capacity flat hash with tombstone compaction
  - `spsc_ring.hpp` — bounded SPSC ring buffer
  - `simulator.hpp` — simulation engine interface
- `src/`
  - `order_book.cpp` — LOB implementation
  - `simulator.cpp` / `main.cpp` — harness + CLI
  - `storage/` — LMDB + optional gRPC plumbing (not required for benchmark mode)
- `tests/`
  - `spsc_ring_test.cpp`
  - `order_book_test.cpp`
- `scripts/`
  - `bench.sh` — repeatable benchmark runner (multi-config MSVC aware)

---

## Build

### Windows (Visual Studio 2022 / MSVC)
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DMSIM_WITH_GRPC=OFF -DMSIM_BUILD_TESTS=ON
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

### Linux / single-config generators (example)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMSIM_WITH_GRPC=OFF -DMSIM_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

---

## Benchmark (recommended)
Use the included script (handles MSVC multi-config correctly and runs multiple reps):
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMSIM_WITH_GRPC=OFF -DMSIM_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Default config (override via env):
- `SYMBOLS=AAPL,MSFT,GOOG,AMZN,NVDA,TSLA`
- `EVENTS=2000000`
- `THREADS=6`
- `SIGMA=0.001`
- `ARENA_BYTES=1048576`
- `REPS=5`

Example override:
```bat
THREADS=1 EVENTS=3000000 REPS=3 scripts/bench.sh
```

### Latest benchmark numbers

**Best**: 42,404,684 ev/s
**Avg**: 41,163,937 ev/s

(Windows 11, MSVC 19.44, Ryzen 7 5800X, 6 pinned threads, `--no-log`)

---

## CLI (common flags)

| Flag                  | Meaning                                | Default          |
| --------------------- | -------------------------------------- | ---------------- |
| `--events N`          | total simulated iterations/events      | `100000`         |
| `--symbols CSV`       | comma-separated symbols                | `AAPL,MSFT,GOOG` |
| `--threads N`         | worker threads (typically = symbols)   | auto             |
| `--sigma X`           | gaussian sigma (fraction of mid)       | `0.001`          |
| `--arena-bytes BYTES` | arena size per symbol                  | `1048576`        |
| `--no-log`            | disable persistence entirely           | off              |
| `--log PATH`          | persist to LMDB                        | off              |
| `--read PATH`         | replay from LMDB                       | off              |
| `--dump N`            | when reading, print first N per symbol | off              |
| `--print-arena`       | show allocator telemetry               | off              |
| `--grpc HOST:PORT`    | stream events to collector             | off              |

> Benchmarking tip: always use `--no-log` unless you're explicitly measuring storage/streaming.

## Why it's fast (short version)

- Tick-based prices avoid floating-key ordering issues and enable tight hashing.
- Flat hash maps + pooling reduce allocations and pointer chasing vs. trees.
- Tombstone compaction keeps open-addressing delete-heavy workloads stable.
- Benchmark harness avoids:
    - contended global atomics in hot loops
    - realtime clock calls per event
    - string hashing/lookup per event

## More docs
- `docs/architecture.md` — core architecture + invariants (LOB / harness / arenas)
- `docs/streaming.md` — gRPC collector, batching, expected throughput
- `docs/persistence.md` — LMDB log format + replay mode