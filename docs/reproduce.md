# Reproduce the checks and results

Start with the demo if you want to see the project run. Use the later sections when you need a particular platform receipt, historical table or measurement.

| Goal | Start here |
| --- | --- |
| Build and run the synthetic example | [Public demonstration](#public-demonstration) |
| Inspect the checked AMD excerpt | [Historical-tape viewer](#historical-tape-viewer) |
| See exactly which profiles passed or failed | [Executed profiles](#executed-profiles) |
| Regenerate the historical study tables | [Historical study](#historical-study) |
| Recreate the timing experiments | [Measurements](#measurements) |

Requirements: Python 3.12 or later, CMake 3.20 or later, a C++17 compiler and POSIX file/thread APIs. Executed toolchains include Apple Clang 21, Ubuntu GCC 13.3 and Clang 18.1.3. Windows and untested compiler/platform combinations are not certified. The [result metadata](results/verification.json) records the runner's Python and the separate Python discovered by CMake/CTest.

Run commands from the repository root. Each retained output directory must be new. Keep raw input, logs and builds outside the publication set; `results/` and `build-*` are ignored. Run heavy commands serially.

macOS uses CommonCrypto for SHA-256; other hosts use the retained portable implementation. No dependency is downloaded. To compare the portable path, pass `--portable-sha256` to `scripts/run.py verify` (CMake option `-DMSIM_PORTABLE_SHA256=ON`) and use a separate build directory. Verification records and sets the provider explicitly; it does not inherit an old cache selection.

## Public demonstration

```sh
export E=results/local
python3 scripts/run.py verify --build build-release --out "$E/release" --config Release --jobs 2
python3 scripts/run.py demo --build build-release --out "$E/demo"
```

The original 710-byte, 23-message session needs no download. The run produces canonical displayed state, metadata and day references, correction-aware accounting, independent raw checks and research tables.

Watch one price level change:

- 90 shares ahead are canceled; 30 execute.
- The optimistic queue advances. The pessimistic queue keeps the canceled shares ahead.
- The level empties after 300 ms and returns after 100 ms.
- A later seven-share `P` report is broken. Corrected `P` volume is zero; reported displayed orders are not restored.

The separate matching example is fixed in [demo.txt](demo.txt).

The [matching visualizer](replay/index.html) covers FIFO priority, multi-price sweeps, queue changes and capacity rejection. Pause, step, scrub or select a scene. Four synthetic scenes contain 1,055 commands; the burst scene has 1,003 commands and up to 160 waiting orders.

Every command, execution and complete FIFO state is checked against the independent C++ book. The sorted-index and capacity views are logical illustrations, not a raw-memory dump. After verification, reproduce the retained data with:

```sh
python3 scripts/render_book.py --build build-release --out docs/replay/book-data.js --check
```

The diagnostic `diff_replay --json` exports snapshots with a final verification record. Failure or incomplete output cannot produce viewer data. Export bounds are 2,000 commands, 512 orders and 32 prices per side. Normal matching and replay are unchanged. Animation is slowed for viewing; its pace is not a throughput or latency result.

The matching asset includes its producing executable hash. `--check` compares that identity too; a different compiler/build can require regeneration even when scene semantics agree. Keep that custody difference separate from a matching regression.

The [feed reconstruction](replay/feed.html) renders the original 23-message demo and its service scenarios. The browser displays snapshots; it runs no matching or service model. After the demo, reproduce those snapshots with:

```sh
python3 scripts/render_replay.py --demo "$E/demo" --out docs/replay/demo-data.js --check
```

The exporter admits only the original demo bytes. It requires a successful same-source receipt, compares every C++ observation with the independent raw reconstruction, and checks that scenario tracing leaves all research tables unchanged. Omit `--check` only when you intend to regenerate the asset. Arbitrary historical input and participant inference are unsupported.

To run the browser harnesses, serve the repository locally:

```sh
python3 -m http.server 8000 --bind 127.0.0.1
```

Then open [`book_view_test.html`](../tests/book_view_test.html) or [`replay_view_test.html`](../tests/replay_view_test.html). They exercise the controls, snapshots, rewind and narrow layouts. The viewers themselves open without a server or install.

The verifier forces `MSIM_BUILD_TESTS=ON` and requires the exact, nonempty CTest inventory in [`tests/inventory.json`](../tests/inventory.json). The full suite has 23 entries across eight obligations:

- matching outcomes and complete state;
- exhaustive histories and ownership corruption;
- malformed ITCH fields and global references;
- worker and output failure paths;
- accounting, research and evidence gates.

The count is not the argument. Each check has an expected transition or a planted failure to detect.

## Historical-tape viewer

The [tape viewer](replay/tape.html) loads a checked local `tape.json`; the page does not ship with a historical tape preloaded. The repository includes one ten-second preview of a selected 50-second AMD opening excerpt at 5×: [watch the MP4](replay/assets/amd-open-5x.mp4).

The date label comes from the input filename. Capture continuity and public display rights are unverified. The raw input is not included. The export command creates a self-contained `index.html`; keep other historical exports outside the repository and confirm rights before distributing them.

```sh
python3 scripts/render_tape.py --build build-release --input /path/to/input.itch --out /path/outside/repository/tape-excerpt --symbol AMD --date-label "2019-01-30 (filename)" --speed 5 --start-ns 34200000000000 --end-ns 34250000000000
```

The existing `itch_observe --tape INPUT OUTDIR SYMBOL START_NS END_NS` diagnostic consumes the whole file through the strict handler and checks final book invariants.

It captures the state immediately before the first symbol event at or after the inclusive start. The first event at or after the exclusive end closes the excerpt. Both boundaries must be bracketed. The tool stops at 60 seconds or 200,000 selected events; it fails rather than downsampling. A full session and the existing storage bounds are still required.

The independent Python check scans original framing from byte zero through the excerpt end, then rebuilds the selected symbol with ordered dictionaries. It compares:

- the exact initial FIFO snapshot;
- every exported event field;
- every post-event ordered 64-bit state digest.

The digest checks equality; it is not cryptographic. Consumed-input SHA-256 binds the source separately. This prefix check is not a second all-market, full-day replay. The local export retains source/executable identities and failed attempts; a failure cannot produce a successful viewer.

The browser applies checked updates; it does not match orders. Seven nearest prices per side show detail, while a separate census counts orders across the displayed book. Overflow is reported. The drawing caps each queue at 40 orders per price; census capacity is explicit. Every state patch is applied, even though the flow trail summarizes a recent interval.

| Message | Viewer treatment |
| --- | --- |
| `E`, `C` | Reduce displayed orders; `C` uses its reported execution price |
| Printable `E`, `C`, `P` | Count in the reported-print total; `P` has no known side or displayed lifecycle |
| `Q` | Keep as an auction report |
| `B` | Mark its report; never restore the displayed book |

Corrections received after the excerpt are not applied. Playback follows file order, not timestamp sorting. A high-water display clock preserves regressions and reports their count. These are feed timestamps, not network latency or measured throughput. Date labels are external; capture continuity remains unverified.

The [tape browser harness](../tests/tape_view_test.html) uses a small original fixture to check controls, updates, rewind, report classes, corrections, wide IDs, reduced motion and narrow layouts. It needs the localhost server described above. The exported viewer and public file-picker page do not.

To exercise the empty-suite failure path, configure a build with `-DMSIM_BUILD_TESTS=OFF`, then run `verify`. Verification must turn the option back on and pass the exact inventory. CTest runs serially; `--jobs 2` limits compilation parallelism.

```sh
python3 scripts/run.py verify --build build-debug --out "$E/debug" --config Debug --jobs 2
python3 scripts/run.py verify --build build-asan --out "$E/asan" --config Release --sanitize address,undefined --bounded --jobs 2
python3 scripts/run.py verify --build build-tsan --out "$E/tsan" --config Release --sanitize thread --bounded --jobs 2
python3 scripts/audit.py --build build-release --out "$E/documentation-audit.json"
```

The bounded sanitizer selection has 16 tests. It keeps directed/exhaustive cases and smaller feed stress profiles, but omits the large differential seed sweep. TSan also requires its intentional race control to report a data race.

The deterministic fuzz driver is not coverage-guided fuzzing. The optional libFuzzer target has no executed release result. The publication audit requires a Git checkout and checks local links, the deterministic example, source inventory, license, Docker exclusions and credential signatures.

## Executed profiles

**Current candidate:** source `6ca67b5` has a macOS Release result of 23/23, the seven-stage demo, and the current tape/browser checks. It does not turn earlier platform results into current-source results. The [revision record](results/revision.json) links each activity to its own source and executable identity.

The matrix below is the earlier full platform run, source `e7c251cfe55afd3196bc69ad084828ae564186da33683c34103fb7caaf2c8907`:

| Profile | Result |
| --- | --- |
| macOS ARM64 Debug / Release | 23/23 each |
| macOS ASan/UBSan | 16/16 |
| macOS TSan | 16/16; planted race detected |
| Linux ARM64 GCC Debug / Release | 23/23 each |
| Linux ARM64 Clang ASan/UBSan | 16/16 |
| Linux ARM64 Clang static TSan | Build FAIL: runtime/allocation-probe new/delete definitions collide |
| Linux ARM64 Clang shared TSan | **FAIL: 15/16**; file-size-limit output campaign produces TSan BUS |
| Separate Linux shared TSan control | Planted race detected; does not repair the failed selection |
| Native x86-64 workflow | Unexecuted |

### Later receipts

These runs add evidence for specific changes. They do not rewrite the matrix above or inherit its source identity.

- **SHA provider (`8b18928`):** macOS Release 23/23 with CommonCrypto and forced-portable builds; macOS ASan/UBSan and TSan selections 16/16; Linux ARM64 GCC Release 23/23 with the portable provider; seven-stage demo passed. Earlier Debug and Linux sanitizer results were not rerun at this revision. The archive was rebuilt and checked separately.
- **Feed replay presentation:** Release 23/23; seven-stage demo; Chromium playback, rewind, queue selection, correction, reduced-motion and 320 px layout checks. C++ executables stayed byte-identical to the provider revision. Other browser engines were not tested.
- **Matching visualizer:** Release 23/23; four scenes passed their reference and invariant gates under focused ASan/UBSan. This did not rerun the full sanitizer profile. Matching, feed/research formulas and benchmark kernels stayed unchanged.
- **Historical viewer:** Release 23/23; seven-stage demo; focused ASan/UBSan coverage for observer, volume and tape boundary/failure paths. This was not a full sanitizer-profile rerun. Core reconstruction, accounting/research formulas and benchmark executables stayed unchanged.
- **Current tape preview (`6ca67b5`):** Release 23/23 and the current browser checks. The clip is selected and as reported; capture continuity and public display rights remain unverified.

Linux TSan remains unresolved. A standalone writer with no project code reproduces a related TSan BUS under an inherited 1,600-byte file-size limit; matched plain and ASan/UBSan writers return normal write errors. The failure occurs without a user thread, stdio or project allocation interposition. The cause and its relation to the older 2 KiB failure are unknown. There is no complete Linux TSan pass or compiler-bug attribution. Static-link and shared-runtime failures are separate activities.

## Source-only Linux ARM64

The Docker context is an explicit source/license/reproduction-guide allowlist. It excludes Git, historical data, private evidence, results and authenticated state. Apple Silicon runs ARM64 natively inside a Linux VM; this is not bare-metal or emulated performance evidence.

```sh
mkdir -p results
docker build --platform linux/arm64 -f Dockerfile.verify -t pmr-verify .
docker run --rm --platform linux/arm64 -v "$PWD/results:/results" pmr-verify \
  python3 scripts/run.py verify --build /results/build-debug \
  --out /results/linux-debug --config Debug --jobs 2
docker run --rm --platform linux/arm64 -v "$PWD/results:/results" pmr-verify \
  python3 scripts/run.py verify --build /results/build-release \
  --out /results/linux-release --config Release --jobs 2
docker run --rm --platform linux/arm64 -v "$PWD/results:/results" pmr-verify \
  python3 scripts/run.py verify --build /results/build-asan \
  --out /results/linux-asan --config Release --sanitize address,undefined \
  --compiler clang++ --bounded --jobs 2
docker run --rm --platform linux/arm64 -v "$PWD/results:/results" \
  -e CXXFLAGS=-shared-libsan \
  -e LD_LIBRARY_PATH=/usr/lib/llvm-18/lib/clang/18/lib/linux pmr-verify \
  python3 scripts/run.py verify --build /results/build-tsan-shared \
  --out /results/linux-tsan-shared --config Release --sanitize thread \
  --compiler clang++ --bounded --jobs 2
```

The last command reproduces a failed profile, not a promised pass. Its deliberate control can be run separately in the same image against `/results/build-tsan-shared/tests/replay_race_probe`, with the same runtime loader path. It must exit nonzero **and contain `ThreadSanitizer: data race`**; an arbitrary signal is insufficient. The default static-link failure is reproduced by omitting those two environment variables in a fresh TSan build directory. The [x86 workflow](../.github/workflows/verify.yml) explicitly requests the shared runtime; creating/editing it does not execute it.

## Historical study

Supply the read-only uncompressed regular file yourself; access/redistribution rights and obtainability are not established by this repository. The [study](market-data.md) identifies its exact bytes and protocol specification.

```sh
export INPUT=/path/to/01302019.NASDAQ_ITCH50
python3 scripts/run.py study \
  --build build-release --input "$INPUT" --out "$E/study" \
  --partitions 16 --budget-mib 64
python3 scripts/render_results.py \
  --out "$E/public" --study "$E/study" --verify release="$E/release"
```

The first command gates all-market C++ accounting, independent raw accounting, C++ event observations, research and an independent raw event/state route. All research table hashes must agree; input/source identities must remain unchanged. High-level analytical formulas are shared, as the [study limitations](market-data.md#auctions-checks-and-limits) explain. Individual raw tables stay outside Git.

The renderer generates panel, queue, depletion, service, depth/time, context, auction and complete symbol/block sensitivity tables. Public `symbols.csv` and `intervals.csv` are exact copies from the accepted report. Supply one `--verify label=directory` per retained profile, including failures. Shared metadata records Python executable basenames and counts from each identified CTest log; raw receipts retain their original paths and hashes.

## Measurements

```sh
python3 scripts/measure.py engine --build build-release --out "$E/engine"
python3 scripts/measure.py mechanisms --build build-release --out "$E/mechanisms" --real-input "$INPUT"
python3 scripts/measure.py profile --build build-release --out "$E/profile"
```

These commands apply the optional strict observer, which rejects an entire activity when its CPU or pageout limits fail. The retained current-host set uses the same kernels, inputs and repetitions while recording background activity as a limitation. The strict renderer does not accept it as a quiet-host set. Preserve every repetition and host observation; never select results by speed. Full-day feed timing is not required or reported.

To reproduce a current-host comparison directly, keep both workers and validation settings fixed except for the declared arm. For example, run the following from the verified Release build; each replay invocation checks exact state and input agreement and emits all three repetitions. Use both `balanced` and `hot` feeds. Retain stdout/stderr and host observations alongside the generated input hashes.

```sh
python3 scripts/make_replay_workload.py "$E/balanced.itch" --commands 1000000 --skew balanced
build-release/replay_bench "$E/balanced.itch" 1 64000 window identity plain warm 3
build-release/replay_bench "$E/balanced.itch" 2 64000 window identity plain warm 3
build-release/replay_bench "$E/balanced.itch" 4 64000 window identity plain warm 3
```

The retained [measurement record](results/performance.json) identifies every arm's parameters, input digest, repetitions and dispersion. `structure_bench ladder vector 4096 2000000 uniform` and the matched `tree` arm reproduce the deep-level comparison; `structure_bench index window 18 2000000 recent` and its `hash` arm reproduce the recent-reference comparison. Use the corresponding executables in `build-release/`. The engine generator and `book_bench run` arguments are listed in [measure.py](../scripts/measure.py); current-host interpretation does not imply that the strict observer passed.

### Matched SHA-provider builds

The [provider comparison](results/sha256-provider.json) uses separate verified Release builds of the same source. On macOS, make `build-commoncrypto` with the default `verify` command and `build-portable` with `--portable-sha256`, using fresh verification output directories. Generate the same balanced and hot inputs above. The loop reproduces the balanced one-worker pair; repeat it for hot input and four workers with distinct output filenames. Keep host observations and every stdout/stderr; do not select repetitions by speed.

```sh
for rep in 0 1 2 3 4 5 6; do
  if [ $((rep % 2)) -eq 0 ]; then
    first=portable; second=commoncrypto
  else
    first=commoncrypto; second=portable
  fi
  for provider in "$first" "$second"; do
    "build-$provider/replay_bench" "$E/balanced.itch" 1 64000 window identity plain warm 1 \
      > "$E/provider-balanced-t1-r$rep-$provider.csv" \
      2> "$E/provider-balanced-t1-r$rep-$provider.stderr"
  done
done
```

Each invocation includes two untimed admission passes and one reported repetition; resident-hash diagnostics have a separate timed warmup. The compact JSON retains raw rows, min/median/max, dispersion, configurations, executable and input identities, canonical output hashes, and private raw-record hashes. The force-portable option is a build configuration, not a runtime identity bypass. Both arms retain global validation and consumed-byte SHA-256. Linux uses the portable path; these Apple-host ratios do not predict Linux performance.

The startup profile sampled reference preparation. A separate diagnostic launched the identical `many_prices` stream with `book_bench run --mode throughput --reps 1 --warmup 0 --minimum-ms 20000`, waited a fixed 20 seconds, then ran `sample PID 10 1 -file stacks.txt` and waited for successful child completion. Its matching/reset/validation stacks and all rates remain diagnostic. Hardware counters were unavailable in the tested VM; cache pressure does not certify cold caches.

The [source manifest](results/source-manifest.json) hashes CMake and code/tests/tools/bench/fuzz/scripts, including untracked implementation files. Prose, workflows and public summaries have separate delivery custody; changing them cannot relabel another implementation's evidence. A retained run requires source agreement, successful required commands and unchanged supplied input. Byte/session success is not source-capture continuity, venue equivalence or production assurance. See the compact [claim register](claims.md) for each limit.
