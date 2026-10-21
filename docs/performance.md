# Performance: what we measured

## What the measurements show

- A contiguous price ladder won **five of six** tested comparisons; a tree won for 4,096 uniformly changing levels.
- In these bounded replay inputs, adding workers made total time **slower**, not faster.
- On the tested Apple host, CommonCrypto reduced the measured replay median by about **2.6–3.2×** versus the portable SHA-256 path.

These are observations from an active development host. Background CPU and pageouts were recorded. The retained set has `strict_release_admissible=false`: it is not dedicated-host latency, exchange performance, portable speed or full-day feed throughput.

The receipts come from several source revisions. The original engine/structure/replay set uses implementation `e7c251cfe55afd3196bc69ad084828ae564186da33683c34103fb7caaf2c8907`; the provider comparison uses `8b18928c9fc98316c1bedb27e8800c61c2b01fd7c0bd978aa466c33f70584f8d`. The [revision record](results/revision.json) preserves those identities. Matching and structure benchmark executables remain byte-identical in later revisions. Every reported repetition completed its semantic checks.

## How to read a number here

The [maintained observer](../scripts/measure.py) requires a verified Release build, final `-O3`, no sanitizers, AC power, three quiet one-second start samples and no increase in macOS pageouts. It samples processes every 250 ms. Outside named compute at 5% CPU, or any outside process at 100%, rejects the activity. All repetitions and rejected attempts are retained.

This is a host check, not a lab guarantee. It cannot certify core placement, frequency, cache residency, physical isolation or work between samples.

For the engine runs, each deterministic workload has seed 17 and one million commands. Independent checks compare every outcome and execution plus periodic exact state. After timing, the result is consumed by a final-state and last-output comparison.

Five throughput groups plus one warmup provide at least 1,000 ms of fresh-subject kernels per group. Parsing, construction, reference preparation, validation and reporting are outside the timed kernel. Setup/reset includes destroying the old subject and constructing the next; final destruction is excluded.

| Recorded quantity | What it means |
| --- | --- |
| Throughput | Sum of command-loop kernels; not process runtime, arrivals or network latency |
| Per-command timestamps | Instrumented intervals affected by clock/measurement overhead; quantization can produce zero, and no overhead is guessed away |
| 16/64-command batches | Distributions of batch means, not individual-command tail latency |
| C++ allocation counter | `new`, `new[]` and aligned calls on the calling thread in the measured loop; a three-call positive control is required |
| PMR and output checks | Separate identical-stream replay; no internal requests or output growth on that tested path |
| Peak RSS | Includes input, reference, retained samples, gates and runtime; it is not the book's PMR footprint |

## Matching commands

Five one-million-command streams passed an independent semantic gate before measurement. Each table cell retains five kernel repetitions of at least one second; generation, parsing, reference verification, construction and reporting are outside the interval.

| Workload | M commands/s, min / median / max | Executions | Executed shares |
| --- | ---: | ---: | ---: |
| stable_depth | 61.12 / 62.01 / 62.16 | 226,582 | 4,355,991 |
| cancel_churn | 41.78 / 45.03 / 45.14 | 49,235 | 474,214 |
| many_prices | 21.13 / 21.16 / 21.17 | 160,562 | 8,110,117 |
| multi_level_sweep | 17.74 / 18.10 / 18.15 | 913,243 | 23,304,036 |
| bursty | 59.98 / 60.00 / 60.11 | 132,273 | 5,575,229 |

Every mode produced the same semantic result. The measured loops reported zero calling-thread C++ allocations, zero internal PMR requests and zero execution-output growth. Those counters do not cover other threads, the C allocator, diagnostics or operating-system paging.

## Data structure trade-offs

Each ladder repetition performs two million operations; each reference-index repetition performs six million. Five repetitions are retained per arm. Nanoseconds per operation divide the median kernel time by the fixed operation count. They are comparative kernel results, not end-to-end latency.

| Active levels / change locality | Vector ns/op | Tree ns/op | Faster in this case |
| --- | ---: | ---: | --- |
| 32 / near best | 18.64 | 70.73 | Vector |
| 32 / uniform | 46.44 | 86.00 | Vector |
| 512 / near best | 18.93 | 110.98 | Vector |
| 512 / uniform | 99.80 | 130.69 | Vector |
| 4,096 / near best | 18.85 | 140.39 | Vector |
| 4,096 / uniform | 301.49 | 174.31 | Tree |

The result argues against replacing the contiguous ladder unconditionally. It does not prove that vectors win for every real order flow.

| Reference pattern | Direct window ns/op | Hash-only ns/op |
| --- | ---: | ---: |
| Recent | 1.67 | 8.78 |
| Aged live references | 1.98 | 9.06 |
| Far jumps | 9.05 | 8.75 |

The direct window is about 5.26× faster for the recent trace and 4.57× faster when repeatedly querying retained old references. Each kernel includes insertion, lookup and deletion; these are not find-only costs. Far jumps use the fallback path and are comparable to hash-only indexing. These traces do not support removing the direct window.

## Replay scaling: more workers lost on these inputs

The original portable-provider comparison processes 1,000,070 synthetic messages. Every repetition checks exact final state against the same one-worker reference; three repetitions are retained per arm.

| Ownership workload | 1 worker | 2 workers | 4 workers |
| --- | ---: | ---: | ---: |
| Balanced | 151.8 ms | 157.6 ms | 168.4 ms |
| Concentrated | 151.3 ms | 158.1 ms | 163.6 ms |

**These runs do not benefit from more workers.** The result applies to these bounded inputs and this implementation, not to every feed or machine.

Coordinator CPU is about 145–147 ms while worker work is smaller. The coordinator frames messages and validates global identity before dispatch. Balanced ownership divides events evenly; concentrated ownership sends about 85% of events to one of four workers. The measurements give no reason to add workers automatically for this configuration.

Read/hash time is about 114–116 ms in the uninstrumented arms and is already included in coordinator time. The run does not isolate global validation or decoding as the cause of the serial cost. Worker busy/wait intervals overlap coordinator work; summed waits span several threads. Stage timestamps add CPU and cannot supply uninstrumented cost fractions.

With four workers, the apparent Amdahl serial fraction exceeds one because total time grows. It is not a physical serial-work proportion. Final validation/capture is included in replay time; report writing and printing are excluded.

## Input hashing: a bounded improvement on one host

Stack samples and cached-read/resident-hash controls pointed to synchronous portable SHA-256, rather than cached file reading, as the largest observed serial cost.

The provider comparison changes only the input hash provider and its build/test support. macOS uses CommonCrypto; other hosts and forced-portable builds retain the portable implementation. Both paths hash every consumed byte and run the same framing, global identity, worker and final-state checks.

The [provider receipt](results/sha256-provider.json) compares verified builds of the same source over identical 29,502,388-byte inputs. Each arm has seven repetitions; the portable/native order alternates each pair. Each command completes two semantic-gate passes before one measured replay. Total time includes setup, read/hash, pipeline and final canonical validation/capture. Report writing and printing are excluded.

| Input / workers | Portable median | CommonCrypto median | Ratio |
| --- | ---: | ---: | ---: |
| Balanced / 1 | 155.177 ms | 52.609 ms | 2.95× |
| Balanced / 4 | 164.547 ms | 62.864 ms | 2.62× |
| Concentrated / 1 | 152.804 ms | 48.145 ms | 3.17× |
| Concentrated / 4 | 163.190 ms | 61.081 ms | 2.67× |

Resident hashing alone took about 120 ms of thread CPU with the portable implementation and 13–14 ms with CommonCrypto. In the production read/hash path, medians fell from 112.7–116.1 ms to 15.7–16.3 ms. A separate native stack sample reached the system SHA compression routine.

This points to the selected mechanism; it does not create exclusive additive pipeline fractions or an instruction-count claim. Canonical state, metadata, day references, cross records and other nontiming outputs agree across the old baseline and both new providers, with one and four workers on both inputs.

Known binary vectors, block/padding boundaries, chunked and zero-length updates, repeat digest snapshots and value copies pass. An interposed call control verifies that requests above 4 GiB are split without 32-bit truncation; it does not hash an actual 4 GiB buffer.

One worker still has the lower total median on both inputs, even with native hashing. Balanced worker busy time falls from about 36.6 ms to 16.0 ms with four workers, while the pipeline remains about 47 ms and final validation/capture rises from about 4.5 ms to 15.1 ms.

Worker capacities stay fixed, so adding owners increases aggregate reserved storage and invariant-check work. This is not a constant-total-memory scaling experiment. Skipping identity or final checks would change the contract and was not tested.

All prototype and final repetitions remain in the receipt; none was selected by speed. Background CPU and pageouts were observed. These warm-input comparisons use unpinned heterogeneous cores, no PMU counters and no dedicated-host isolation. Replay allocation was not measured. The result is bounded library replay on one Apple host—not matching-command throughput, portable speed or full-day feed throughput.

## Profiles and remaining limits

On Apple M5 Pro / 24 GiB, macOS 26.3.2 and Apple Clang 21, a separate one-repetition, 20-second-kernel experiment was sampled with macOS `sample` at 1 ms for 10 seconds. The startup sample caught **reference preparation only**. A second sample, started after a fixed 20-second delay, reached matching and prominently showed `open_level`, `close_level` and ladder movement on `many_prices`. Reset and post-timer snapshot/validation also appear. These profiles identify code paths to investigate; they do not give exclusive cost fractions, cycles per operation or admitted rates. Raw profiles and hashes are retained.

Native ARM64 Linux-VM hardware-counter probes returned `EPERM` for cycles, instructions, cache misses and branch misses. No PMU values are replaced with wall time. A 256 MiB pressure arm exists in the diagnostics but cannot certify cold input/cache state: setup may warm the cache again.

See the [reproduction guide](reproduce.md#measurements) for commands. The [result record](results/performance.json) retains source, executable and input identities, every repetition, strict-gate failures and host observations. Fixed arm order, heterogeneous cores, background load, paging, warm inputs and no hard affinity or PMU counters limit inference. Full-day feed timing is outside this release scope.
