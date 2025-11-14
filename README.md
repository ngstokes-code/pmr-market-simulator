# PMR Market Simulator (C++17)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)
![Performance](https://img.shields.io/badge/Throughput-25.7M%20ev%2Fs%20%E2%80%94%20NUMA%20Optimized-brightgreen.svg)
![Persistence](https://img.shields.io/badge/Storage-LMDB-critical.svg)
![gRPC](https://img.shields.io/badge/Streaming-gRPC%20Realtime-blueviolet.svg)
![Build](https://img.shields.io/badge/Build-CMake-darkblue.svg)
![Memory](https://img.shields.io/badge/Arena-std%3A%3Apmr%20Monotonic-success.svg)
![Allocator](https://img.shields.io/badge/Allocator-aware-9cf.svg)

A **high-performance, allocator-aware market simulator** with NUMA-optimized
order-book engines, zero-allocation event loops, LMDB persistence, and
**real-time gRPC streaming** for downstream analytics or distributed pipelines.

It models multiple independent limit order books with Gaussian-sampled prices
and optional sinusoidal drift—sustaining **7.6 million events/sec single-threaded**
and **25.7 million events/sec** multi-threaded on a modern 6-core CPU.

> ⚙️ 25.7 million events/sec (6 threads, NUMA-pinned) · 7.67 million events/sec (single core) · ~130 ns per event

## Table of Contents

- [Overview](#overview)
- [Core Features](#core-features)
- [Real-Time gRPC Streaming](#real-time-grpc-streaming)
  - [Components](#components)
  - [Features](#features)
- [Simulator vs gRPC Throughput Explained](#simulator-vs-grpc-throughput-explained)
- [Benchmarks](#benchmarks)
  - [Example Simulation](#example-simulation)
- [Replay Mode (LMDB Round-Trip)](#replay-mode-lmdb-round-trip)
- [Detailed Single-Thread Benchmarks](#detailed-single-thread-benchmarks)
- [Build & Run](#build--run)
  - [Example Run](#example-run)
  - [Streaming to a Collector](#streaming-to-a-collector)
- [CLI Flags](#cli-flags)
- [Design Principles](#design-principles)
- [References](#references)


---

## 🚀 Quick Start

```bash
git clone https://github.com/ngstokes-code/pmr-market-simulator
cd pmr-market-simulator
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
```

---
<a id="overview"></a>
## ⚡ Overview

|Component|Purpose|
|------------|----------|
|**`OrderBook`**|Per-symbol price-time order book with PMR-based bids / asks / index maps and strict price–time priority.|
|**`Simulator`**|Multi-threaded event engine generating add / cancel / trade flows with Gaussian price sampling with optional drifting midprice.|
|**`LMDBStorage`**|Durable, per-symbol append-only event log using Lightning Memory-Mapped Database (LMDB).|
|**`LMDBReader`**|Offline reader for verifying and replaying persisted LMDB event streams.|
| **`GrpcStorage`**     | High-throughput client-side event streamer with protobuf batching and gRPC backpressure handling. |
| **`collector_server`**| Reference real-time receiver measuring delivered events/sec and writing ACK counts. |
|**`pmr_utils`**|Custom counting upstream allocator for precise arena telemetry.|

---
<a id="core-features"></a>
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
<a id="real-time-grpc-streaming"></a>
## 📡 Real-Time gRPC Streaming

The simulator can export live order-book events over a **high-throughput gRPC streaming interface** suitable for real-time analytics, research, and distributed pipelines.

```text
┌──────────┐      ┌────────────┐     ┌──────────────┐
│ Simulator ├───▶│ GrpcStorage ├───▶│ gRPC Stream  │──▶ CollectorServer
└──────────┘      └────────────┘     └──────────────┘
```
<a id="components"></a>
### Components
| Component              | Description                                          |
| ---------------------- | ---------------------------------------------------- |
| `GrpcStorage`          | Client-side batching serializer + streaming writer   |
| `MarketStream.Publish` | Bidirectional protobuf-defined RPC                   |
| `collector_server`     | High-throughput receiver measuring delivered msg/sec |

<a id="features"></a>
### Features

- **Batching** of events (default: 512) for network efficiency
- **Protobuf arena allocation** for zero-copy serialization
- **Cross-process real-time delivery**
- **~2.0–3.5M protobuf messages/sec** delivered on Windows/MSVC (baseline)

---
<a id="simulator-vs-grpc-throughput-explained"></a>
## 📊 Simulator vs gRPC Throughput Explained

The simulator internally executes simulation iterations, each of which may or may not produce an order-book event.
Only actual generated events (ADD / CANCEL / TRADE) are sent to the gRPC stream.

Example for 500,000 iterations:
| Type                       | Count       |
| -------------------------- | ----------- |
| Simulation iterations      | 500,000     |
| ORDER_ADD                  | 146,038     |
| ORDER_CANCEL               | 40,106      |
| TRADE                      | 104,000     |
| **Exported events (gRPC)** | **290,144** |

The collector returns an ACK containing the number of received protobuf messages, which equals the exported-event count.

---
<a id="benchmarks"></a>
## 📊 Benchmarks

### Multi-Threaded (NUMA-Pinned Arenas)
Benchmarked on **Ryzen 7 5800X (8C/16T, NUMA 0)**
Compiled with Clang 17 / MSVC 19.44 using `-O3` + LTO (Release)

| Config | Threads | Logging | Events/sec | Notes |
|---------|----------|----------|-------------|-------|
| 6 symbols (NUMA-pinned) | 6 | No | **25.7 M** | xoshiro128+ RNG + Box-Muller, NUMA-pinned threads |
| 6 symbols (single-thread baseline) | 1 | No | **7.6 M** | Single-core baseline |
| 6 symbols (NUMA-pinned) | 6 | Yes | **2.56 M** | Shared binary logging (I/O-bound) |
| 6 symbols (single-thread baseline) | 1 | Yes | **3.59 M** | Single writer, I/O-bound |

> ⚙️ *Each core processes ~7–7.6 million events/sec (≈130 ns per event), with linear scaling up to 6 threads and full NUMA locality.*

<a id="example-simulation"></a>
## 🧪 Example Simulation

```yaml
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
<a id="replay-mode-lmdb-round-trip"></a>
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
```text
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
<a id="detailed-single-thread-benchmarks"></a>
## 📊 Detailed Single-Thread Benchmarks

| Build   | Symbols | Events | Time (ms) | Throughput (ev/s) | Notes        |
| ------- | ------- | ------ | --------- | ----------------- | ------------ |
| Debug   | 3       | 100 k  | 117       | 0.94 M            | Baseline     |
| Release | 3       | 100 k  | ~13       | 7.6 M             | `-O3`, LTO   |
| Release | 6       | 200 k  | ~28       | 7.7 M             | Linear scale |

*(Ryzen 7 5800X, Windows 11, MSVC 19.44)*

---
<a id="build--run"></a>
## 🔧 Build & Run

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
```
<a id="example-run"></a>
### Example Run
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
<a id="streaming-to-a-collector"></a>
### ▶️ Streaming to a Collector

Start collector:
```bash
./collector_server
```
Run simulator with streaming enabled:
```bash
./market_sim --events 500000 --grpc localhost:50051
```
Output:
```yaml
MarketSim Report
---------------------------
Symbols:           3
Total events:      500000
Adds:              146038
Cancels:           40106
Trades:            104000
Elapsed:           176.9 ms
Throughput:        2.82M ev/s
---------------------------
Collector ACK count: 290144
```

<a id="cli-flags"></a>
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
| `--grpc HOST:PORT`    | Stream generated events to a remote gRPC collector using the `MarketStream.Publish` RPC. Enables batched protobuf serialization and real-time delivery. | off |


---
<a id="design-principles"></a>
## 🧭 Design Principles

- **Allocator locality** -> per-symbol arenas eliminate cross-thread contention
- **NUMA locality** -> pinned threads prevent remote-node memory access
- **Linear scalability** -> cost ≈ O(symbols × events)
- **Deterministic performance** -> no runtime allocations during event loop (zero heap churn)
- **Extensible I/O** -> abstract `IStorage` enables LMDB, mmap, or gRPC back-ends
- **Portable** -> header-only STL + LMDB C library (no external dependencies)

---
<a id="references"></a>
## 📚 References

- ISO C++17 §20.7 — Polymorphic Memory Resources
- Andrei Alexandrescu — Allocator-Aware Programming in C++
- LMDB (Howard Chu et al.) — Lightning Memory-Mapped Database


**PMR Market Simulator** — a compact allocator-aware C++ microstructure engine for exploring real-time order-book dynamics.
