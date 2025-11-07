
# PMR Market Simulator (C++17)

A **low-latency event-driven market simulator** demonstrating
modern **C++17 polymorphic memory resources (`std::pmr`)**, allocator design,
and high-throughput event loops.

The simulator models multiple independent order books with Gaussian-sampled
order prices and an optional sinusoidal price drift, sustaining over half a million
events per second in Debug mode on commodity hardware.

---

## ⚡ Overview

|Component|Purpose|
|------------|----------|
|**`OrderBook`**|Per-symbol book with PMR-based bids / asks / index maps and strict price–time priority.|
|**`Simulator`**|Generates and processes add / cancel / trade events with Gaussian sampling around a drifting midprice.|
|**`Storage`**|Append-only binary event logs (LMDB adapter planned).|
|**`pmr_utils`**|Counting upstream allocator for memory-usage telemetry.|

---

## 🔩 Key Features

- Per-symbol **monotonic arenas** via `std::pmr::monotonic_buffer_resource`
- Zero heap churn in the event loop
- Gaussian price generation around a dynamic midprice (`std::normal_distribution`)
- Optional **sinusoidal drift** to simulate cyclical volatility:
    - price = mid * (1.0 + drift_ampl * sin(2π * event / drift_period))
- Append-only binary log (swappable for LMDB)
- Lightweight allocator stats from a counting upstream resource
- Fully parameterized via CLI flags

---

## 🧪 Example Run

```text
MarketSim (PMR) Report
---------------------------
Symbols:           3
Total events:      100000
Adds:              50930
Cancels:           2024
Trades:            38969
Elapsed:           187.1 ms
Throughput:        534,550 ev/s  (Debug build, Ryzen 7 5800X)
---------------------------
```
⚙️ *On a Ryzen 7 5800X, a Release + LTO build typically **sustains 2–3 million events/sec (single thread)**. Performance **scales linearly with symbols and event count**; each std::pmr arena isolates per-symbol allocations to avoid cross-thread contention*

---

## 📊 Benchmarks (single thread)

| Build   | Symbols | Events | Time (ms) | Throughput (ev/s) | Notes        |
| ------- | ------- | ------ | --------- | ----------------- | ------------ |
| Debug   | 3       | 100 k  | 187       | 0.53 M            | Baseline     |
| Release | 3       | 100 k  | ~35       | 2.8 M             | `-O3`, LTO   |
| Release | 6       | 200 k  | ~70       | 2.9 M             | Linear scale |

*(Ryzen 7 5800X, Windows 11, MSVC 19.44)*

---

## 🔧 Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
./market_sim \
  --symbols AAPL,MSFT,GOOG \
  --events 200000 \
  --sigma 0.001 \
  --drift-ampl 0.002 \
  --drift-period 10000 \
  --arena-bytes 1048576 \
  --log events.bin \
  --print-arena
```

## ⚙️ CLI flags
| Flag                  | Description                       | Default          |
| --------------------- | --------------------------------- | ---------------- |
| `--events N`          | Total events to simulate          | 100 000          |
| `--symbols CSV`       | Comma-separated symbol list       | AAPL, MSFT, GOOG |
| `--sigma X`           | Gaussian sigma as fraction of mid | 0.001            |
| `--drift-ampl A`      | Sinusoidal drift amplitude        | 0.0 (off)        |
| `--drift-period P`    | Drift period in events            | 10 000           |
| `--arena-bytes BYTES` | Arena size per symbol             | 1 MiB            |
| `--log PATH`          | Binary log output path            | none             |
| `--print-arena`       | Print allocator usage summary     | off              |


---

## 🧭 Design Notes

- **Linear scalability**: cost ∝ (symbols * events)
- **Allocator efficiency**: PMR arenas recycle memory; no per-event new/delete
- **Cache locality**: deques + maps remain inside per-symbol arenas
- **Extensibility**: abstract `IStorage` enables LMDB or mmap back-ends
- **Portability**: C++17, no external dependencies

---

## 🚀 Next steps
- Implement `LMDBStorage : IStorage` and swap via `make_storage()`
- Add `market.proto` + gRPC tick stream
- Replace `std::pmr::map` SoA flat price levels
- Add real-time CSV / ImGui midprice visualization
- Add latency & allocation profiling

---

## 📚 References

- ISO C++17 §20.7 — Polymorphic Memory Resources
- Andrei Alexandrescu — Allocator-Aware Programming in C++
- LMDB (Howard Chu et al.) — Lightning Memory-Mapped Database


**PMR Market Simulator** — a compact allocator-aware C++ microstructure engine for exploring real-time order-book dynamics.
