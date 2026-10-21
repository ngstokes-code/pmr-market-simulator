# What is checked—and what each check cannot tell you

This register summarizes checks run at several revisions. Read each result with its source identity and scope; the table is **not** one release-wide PASS. The [current source manifest](results/source-manifest.json), [platform receipts](results/verification.json), [study](results/study.json), [measurements](results/performance.json) and [revision record](results/revision.json) preserve those identities.

Current implementation: `6ca67b557627daec6f8272e39679e1e6f2f24b6bb19b268b9bbce0b035dfb017`.

## Matching and storage

### Price-time outcomes, quantity and capacity

**Checked:** Independent map/deque differential traces, exhaustive histories through depth four, metamorphic cases and retained regressions.

**Boundary:** Finite inputs only. Output capacity is reserved before engine mutation; none of this establishes venue-equivalent matching.

### Ownership and reference reachability

**Checked:** Free/live corruption cases, backward-shift deletion mutant, reference-window catch-up, full-hash and `UINT64` boundary cases.

**Boundary:** One writer and valid owned allocations. Diagnostics are not recovery from arbitrary memory corruption.

## Feed and replay

### ITCH framing, supported messages and global identity

**Checked:** Framing/field cases, independent feed state, retired-reference failures and duplicate live IDs across owners.

**Boundary:** Ordered complete-session contract only. Unused payload rules and capture continuity remain unverified.

### Deterministic replay and terminal failure

**Checked:** Exact bounded terminal records across workers, chunks and index modes; startup, worker, buffer, input and output failure campaigns.

**Boundary:** No independent all-market full-day FIFO export. No crash durability or resumable allocation failure.

## Historical results

### Corrected reported-volume accounting

**Checked:** Separate C++ partition ledger and Python raw-byte/dictionary checker agree on all **310,120** symbol/five-minute cells and aggregates.

**Boundary:** One examined input. E/CY/P shares are eligible; Q/CN remain separate. This is not an official market-wide volume statistic.

### Passive-entry service scenarios

**Checked:** All **638,980** emitted event projections agree with raw reconstruction; table hashes, hand ledgers and a bounded token model agree.

**Boundary:** The high-level research formulas are shared. The 2/3 model outcomes are not bounds on actual venue fills.

## Demonstrations

### Synthetic feed example

**Checked:** Every C++ event projection agrees with raw reconstruction; the optional scenario trace leaves the research tables unchanged. [Browser checks](../tests/replay_view_test.html) cover playback, rewind and corrections.

**Boundary:** Exact 710-byte demo only. The browser does not run a market model, infer participants or measure latency.

### Matching visualizer

**Checked:** Four synthetic scenes; every command, execution and full FIFO state agrees with the reference book. Invariants and rejection behavior are checked. [Browser controls](../tests/book_view_test.html) exercise the viewer.

**Boundary:** Bounded synthetic scenes, not market realism. The browser applies checked state patches. Its sorted-index and capacity graphics are logical illustrations, not memory/probe traces or performance evidence.

### Historical-tape viewer and AMD preview

**Checked:** Full-input strict C++ replay plus an independent original-byte prefix reconstruction of initial FIFO state, event fields and every ordered-state digest. Hand/wire and [browser cases](../tests/tape_view_test.html) exercise report handling and controls.

**Boundary:** Export is limited to 60 seconds or 200,000 symbol events. The 64-bit state digest is noncryptographic; this is not an independent all-market full-day export. The included MP4 is a selected, as-reported excerpt. Raw tape is excluded; capture continuity and public display rights are unverified. No corrected full-day accounting or performance claim follows from the clip.

## Measurements and platform runs

### Matching-path allocation checks

**Checked:** Calling-thread `new`/`new[]`/aligned-allocation counter with a three-call positive control, plus PMR and output-growth checks.

**Boundary:** Tested command loops only. Other threads, C allocation, diagnostics and operating-system paging are outside the counter.

### Engine and structure timings

**Checked:** Five engine workloads, six ladder comparisons, three reference patterns and matched 1/2/4-worker replay. Every repetition is retained.

**Boundary:** Active development host, not dedicated-host latency, portable speed or general superiority. See [measurement limits](performance.md).

### SHA-256 provider comparison

**Checked:** Binary/snapshot/copy cases, canonical agreement across baseline/native/portable builds and [seven matched repetitions per arm](results/sha256-provider.json).

**Boundary:** Observed improvement on one Apple host. Portable fallback remains. No replay-allocation, portable-speed, cold-input or full-day throughput claim.

### Executed platform profiles

**Checked:** Full Debug/Release, selected ASan/UBSan and macOS TSan with race control. See the [exact matrix](reproduce.md#executed-profiles).

**Boundary:** Linux shared TSan is **15/16 FAIL**; static TSan also fails. The separate race control detects its planted race. Native x86 workflow is unexecuted.

## Unsupported claims

The project does not establish **complete source capture, Nasdaq allocation behavior, causal effects or strategy performance**. A byte-identical file, valid session markers and successful replay cannot prove that a source never omitted complete messages.

The [eight test obligations](../tests/inventory.json) organize the suite. Passing checks show that named failure mechanisms were detected; the count is not a correctness certificate.
