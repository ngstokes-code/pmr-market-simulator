#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/../build"
mkdir -p "$BUILD"
cd "$BUILD"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
./market_sim --symbols AAPL,MSFT,GOOG --events 200000 --sigma 0.001 --arena-bytes 1048576 --print-arena --log events.bin
