
# PMR Market Simulator (C++17)

A baseline low-latency **market simulator** showcasing **`std::pmr` arenas** and
**Gaussian price sampling around the mid**.

## Features (baseline)
- Per‑symbol **PMR arenas** using `std::pmr::monotonic_buffer_resource`
- Order book with **price–time priority** (PMR containers)
- Gaussian order prices around the evolving **midprice**
- Append‑only **binary log** (swap with LMDB later)
- Simple allocator stats via a counting upstream resource

## Build
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
./market_sim --symbols AAPL,MSFT,GOOG --events 200000 --sigma 0.001 --arena-bytes 1048576 --log events.bin --print-arena
```

## CLI flags
- `--events N`                : total events to simulate (default 100000)
- `--symbols CSV`             : symbol list (default 3 auto-picked)
- `--sigma X`                 : base Gaussian sigma (fraction of mid, default 0.001)
- `--drift-ampl A`            : volatility drift amplitude (default 0.0 = off)
- `--drift-period P`          : drift period in events (default 10000)
- `--arena-bytes BYTES`       : per‑symbol arena size (default 1<<20)
- `--log PATH`                : append-only binary event log (optional)
- `--print-arena`             : print upstream arena usage

## Next steps
- Implement `LMDBStorage : IStorage` and swap via `make_storage()`
- Add `market.proto` + gRPC service for streaming ticks / snapshots
- Replace `std::pmr::map` with a flat container or custom price levels (SoA)
