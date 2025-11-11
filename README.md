# PMR Market Simulator (C++17)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)
![Performance](https://img.shields.io/badge/Throughput-25.7M%20ev%2Fs%20%E2%80%94%20NUMA%20Optimized-brightgreen.svg)
![Persistence](https://img.shields.io/badge/Storage-LMDB-critical.svg)
![Build](https://img.shields.io/badge/Build-CMake-darkblue.svg)
![Memory](https://img.shields.io/badge/Arena-std%3A%3Apmr%20Monotonic-success.svg)
![Allocator](https://img.shields.io/badge/Allocator-aware-9cf.svg)

A **low-latency, allocator-aware, event-driven market simulator** demonstrating
modern **C++17 polymorphic memory resources** (`std::pmr`), custom allocator design,
NUMA-aware memory locality, and high-throughput event processing.

It models multiple independent order books with Gaussian-sampled order prices and
optional sinusoidal drift – sustaining **7.6 million events/sec single-threaded**
and **25.7 million events/sec multi-threaded (6 cores)** using
NUMA-pinned arenas and the `xoshiro128+` RNG with Box-Muller transform.

> ⚙️ 25.7 million events/sec (6 threads, NUMA-pinned) · 7.67 million events/sec (single core) · ~130 ns per event

---

## ⚡ Overview

|Component|Purpose|
|------------|----------|
|**`OrderBook`**|Per-symbol price-time order book with PMR-based bids / asks / index maps and strict price–time priority.|
|**`Simulator`**|Multi-threaded event engine generating add / cancel / trade flows with Gaussian price sampling with optional drifting midprice.|
|**`LMDBStorage`**|Durable, per-symbol append-only event log using Lightning Memory-Mapped Database (LMDB).|
|**`LMDBReader`**|Offline reader for verifying and replaying persisted LMDB event streams.|
|**`pmr_utils`**|Custom counting upstream allocator for precise arena telemetry.|

---

## 🔩 Core Features

- Per-symbol **monotonic arenas** (`std::pmr::monotonic_buffer_resource`)
- **NUMA-pinned threads and arenas** for optimal cache locality
- **Zero heap churn** in hot paths (event loop fully preallocated)
- **Gaussian-sampled prices** around a drifting midprice via fast `xoshiro128+` RNG + Box-Muller transform
- Optional **sinusoidal drift** to simulate cyclical volatility:
    - price = mid * (1.0 + drift_ampl * sin(2π * event / drift_period))
- LMDB persistence for **durable replayable event logs**
- Precise allocator telemetry (bytes requested per symbol)
- Fully parameterized via CLI flags

---

## 📊 Benchmarks

### Multi-Threaded (NUMA-Pinned Arenas)
Benchmarked on **Ryzen 7 5800X (8C/16T, NUMA 0)**
Compiled with Clang 17 / MSVC 19.44 using `-O3` + LTO (Release)

| Config | Threads | Logging | Events/sec | Notes |
|---------|----------|----------|-------------|-------|
| 6 symbols (NUMApinned) | 6 | No | **25.7 M** | xoshiro128+ RNG + Box-Muller, NUMA-pinned threads |
| 6 symbols (single-thread baseline) | 1 | No | **7.6 M** | Single-core baseline |
| 6 symbols (NUMApinned) | 6 | Yes | **2.56 M** | Shared binary logging (I/O-bound) |
| 6 symbols (single-thread baseline) | 1 | Yes | **3.59 M** | Single writer, I/O-bound |

> ⚙️ *Each core processes ~4.5–4.7 million events/sec (≈165 ns per event), with linear scaling up to 6 threads and full NUMA locality.*

---

### Single-Threaded (Legacy Baseline)

| Build   | Symbols | Events | Time (ms) | Throughput (ev/s) | Notes        |
| ------- | ------- | ------ | --------- | ----------------- | ------------ |
| Debug   | 3       | 100 k  | 187       | 0.53 M            | Baseline     |
| Release | 3       | 100 k  | ~35       | 2.8 M             | `-O3`, LTO   |
| Release | 6       | 200 k  | ~70       | 2.9 M             | Linear scale |

*(Ryzen 7 5800X, Windows 11, MSVC 19.44)*

---


## 🧪 Example Simulation

```text
MarketSim (PMR) Report
---------------------------
Symbols:           3
Total events:      100000
Adds:              29232
Cancels:           7987
Trades:            20732
Elapsed:           13.039 ms
Throughput:        7669299 ev/s
---------------------------
```
⚙️ *On a Ryzen 7 5800X, Release + LTO builds typically sustain **5–7 million events/sec** single-threaded.<br>Performance scales linearly with symbol count and event volume.*

---

## 🗃️ Replay Mode (LMDB Round-Trip)

Generate, persist, and replay market events from LMDB.
```bash
# 1. Generate a persistent LMDB log
./market_sim --symbols AAPL,MSFT,GOOG --events 10000 --log store.mdb

# 2. Read and summarize stored events
./market_sim --read store.mdb

# 3. Inspect the first 5 events per symbol
./market_sim --read store.mdb --dump 5
```

### Sample Output
```
Found 3 symbol(s): AAPL GOOG MSFT
AAPL: 3043 events
First 5 events:
 [ADD] AAPL 99.79 x 39 (S) t=502533332403200
 [TRD] AAPL 99.60 x 73 (S) t=502533293606400
 [ADD] AAPL 99.64 x 90 (B) t=502533333190400
 [TRD] AAPL 99.53 x 16 (S) t=502533342694400
 [ADD] AAPL 99.52 x 18 (B) t=502533292428800
```

---

## 📊 Detailed Single-Thread Benchmarks

| Build   | Symbols | Events | Time (ms) | Throughput (ev/s) | Notes        |
| ------- | ------- | ------ | --------- | ----------------- | ------------ |
| Debug   | 3       | 100 k  | 117       | 0.94 M            | Baseline     |
| Release | 3       | 100 k  | ~13       | 7.6 M             | `-O3`, LTO   |
| Release | 6       | 200 k  | ~28       | 7.7 M             | Linear scale |

*(Ryzen 7 5800X, Windows 11, MSVC 19.44)*

---

## 🔧 Build & Run

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
```

## Example Run
```bash
./market_sim \
  --symbols AAPL,MSFT,GOOG \
  --events 200000 \
  --sigma 0.001 \
  --drift-ampl 0.002 \
  --drift-period 10000 \
  --arena-bytes 1048576 \
  --log store.mdb \
  --print-arena
```

## ⚙️ CLI flags
| Flag                  | Description                                       | Default          |
| --------------------- | ------------------------------------------------- | ---------------- |
| `--events N`          | Total events to simulate                          | 100 000          |
| `--symbols CSV`       | Comma-separated symbol list                       | AAPL, MSFT, GOOG |
| `--sigma X`           | Gaussian sigma as fraction of midprice            | 0.001       |
| `--drift-ampl A`      | Sinusoidal drift amplitude                        | 0.0 (off)        |
| `--drift-period P`    | Drift period in events                            | 10 000           |
| `--arena-bytes BYTES` | Arena size per symbol                             | 1 MiB            |
| `--no-log`            | Disable persistence entirely (greater throughput) | none             |
| `--log PATH`          | LMDB output path                                  | none             |
| `--read PATH`         | Replay events from LMDB                           | none             |
| `--dump N`            | When reading, print first N events per symbol     | none             |
| `--print-arena`       | Print allocator usage summary                     | off              |


---

## 🧭 Design Principles

- **Allocator locality** -> per-symbol arenas eliminate cross-thread contention
- **NUMA locality** -> pinned threads prevent remote-node memory access
- **Linear scalability** -> cost ≈ O(symbols × events)
- **Deterministic performance** -> no runtime allocations during event loop (zero heap churn)
- **Extensible I/O** -> abstract `IStorage` enables LMDB, mmap, or gRPC back-ends
- **Portable** -> header-only STL + LMDB C library (no external dependencies)

---

## 🚀 Roadmap
- [x] `LMDBStorage` persistence
- [x] `LMDBReader` replay + `--dump` mode
- [x] **NUMA-pinned multi-threading**
- [ ] Add `market.proto` + gRPC tick stream
- [ ] Flat SoA price levels (replace `std::pmr::map`)
- [ ] Real-time visualization (ImGui / CSV)
- [ ] Latency & allocator profiling tools

---

## 📚 References

- ISO C++17 §20.7 — Polymorphic Memory Resources
- Andrei Alexandrescu — Allocator-Aware Programming in C++
- LMDB (Howard Chu et al.) — Lightning Memory-Mapped Database


**PMR Market Simulator** — a compact allocator-aware C++ microstructure engine for exploring real-time order-book dynamics.
