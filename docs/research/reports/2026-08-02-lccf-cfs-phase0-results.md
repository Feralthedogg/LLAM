<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF-CFS Phase 0 Common-Fact Results

## Verdict

`PASS`

The standalone model supports building one immutable completion fact per
winning generation and sharing it across direct and queued consumers. All 72
predeclared cells passed correctness, lifetime, allocation, throughput, CPU,
and p99 gates.

This is a model result. It authorizes an internal Executor prototype; it does
not demonstrate a production runtime win and does not authorize a version or
release change.

## What was tested

The candidate stores stable completion data once:

- generation and semantic fact identity;
- canonical result, error, event, and payload;
- captured source and home-shard history;
- resolved site identity under retained module and payload references.

Every consumer separately rechecks mutable policy:

- direct enablement and module policy;
- backend capability;
- budget, fairness, tracing, and queue pressure;
- stop, migration, callback-active, and shard pause/offline state;
- current home and consuming shard.

The paired modes differ only in materialization work:

| Baseline | Candidate | Routing |
|---|---|---|
| `recompute_queue` | `shared_fact_queue` | queue only |
| `recompute_fused` | `shared_fact_fused` | direct only |
| `mixed_recompute` | `mixed_shared_fact` | deterministic direct/queue mix |

Tickets, frames, callback formulas, commands, generations, route selection,
and canonical final state are identical within every pair.

## Correctness and lifetime result

| Check | Result |
|---|---:|
| Differential cells | 72 / 72 |
| Cell statuses | 72 `PASS`, 0 failed |
| Fresh measured processes | 1,440 |
| Process-local candidate/baseline ratios | 720 |
| Raw rows with checksum mismatch inside a pair | 0 |
| Raw rows with unbalanced references | 0 |
| Raw rows with hot-path allocation | 0 |
| Fact build failures | 0 |
| Generation mismatches | 0 |
| Reuse delays after callback retirement | 0 |
| Shared rows with more than one normalization or lookup per generation | 0 |
| Module pin count mismatches | 0 |
| Payload pin count mismatches | 0 |

The C differential suite also compares event-sequence and command-sequence
hashes after every round, then checks literal final frames, events, commands,
and checksums. Three-way I/O/timeout/cancel workloads retire two losing tickets
per winning generation without allowing a stale ticket to decrement the new
generation's backend reference.

Race and transition coverage includes:

- concurrent three-source publication with exactly one winner;
- double-consume admission with exactly one callback reference;
- stale generation publication and consumption;
- direct disable, fairness, budget, trace, stop, migration, module disable,
  backend disable, callback-active, queue-pressure, and shard-state changes;
- queue forwarding to a new home and defer-before-forward for an offline home;
- direct-to-queue yield ownership transfer;
- module/payload pin failure, malformed completion, and invalid site handling;
- delayed reuse while any callback, queue, backend, external, payload, or
  module reference remains.

The concurrent fact test completed 1,000 repeated runs under the normal build,
then passed ThreadSanitizer. ASan/UBSan and static analysis also completed
without a reported defect.

## Benchmark method

The measured matrix is:

```text
4 workloads
x 3 paired mode families
x 3 layouts (64+64, 96+64, unified 128)
x 2 site counts (1, 8)
= 72 cells
```

Each cell used five repetitions. Even repetitions ran `ABBA`; odd repetitions
ran `BAAB`. Every letter is a fresh process, producing 20 raw processes and 10
process-local ratios per cell. Each process used:

```text
instances       257
chain length    8
warmup rounds   32
measured rounds 1024
completions     263,168
callbacks       2,105,344
```

Across the matrix, 378,961,920 generations and 3,031,695,360 callbacks were
measured. Ratios are defined as:

```text
wall speedup = baseline wall / candidate wall       (higher is better)
CPU ratio    = candidate CPU / baseline CPU          (lower is better)
p99 ratio    = candidate round p99 / baseline p99    (lower is better)
```

The host was an Apple M4 with 24 GiB RAM, macOS 26.5.1, and Apple Clang 21.0.0.
Hardware instruction counters were unavailable, so mixed-mode classification
used the predeclared CPU alternative.

## Quantitative result

| Candidate | Cells | Wall min / median / max | Median CPU ratio | Worst p99 ratio |
|---|---:|---:|---:|---:|
| `shared_fact_queue` | 24 | 1.881 / 2.066 / 2.119x | 0.484 | 0.554 |
| `shared_fact_fused` | 24 | 1.779 / 1.928 / 2.015x | 0.519 | 0.583 |
| `mixed_shared_fact` | 24 | 1.822 / 1.984 / 2.051x | 0.504 | 0.561 |
| **All cells** | **72** | **1.779 / 1.984 / 2.119x** | **0.504** | **0.583** |

Mixed-mode median CPU ratios ranged from 0.488 to 0.549, comfortably beyond
the required 5% improvement. The minimum queue and fused wall speedups were
1.881x and 1.779x respectively, so neither approached the 0.98x regression
boundary. The worst candidate/baseline p99 ratio was 0.583, below the 1.05
limit.

Timer/cancel competition was the least favorable workload because it performs
three claims and retires two losers per generation:

| Workload | Cells | Wall min / median / max |
|---|---:|---:|
| I/O pipeline | 18 | 1.984 / 2.040 / 2.119x |
| RPC state | 18 | 1.952 / 2.010 / 2.079x |
| Timer/cancel | 18 | 1.779 / 1.843 / 1.942x |
| Mixed fairness | 18 | 1.871 / 1.969 / 2.108x |

No measured layout changed the conclusion:

| Layout | Hot / sidecar bytes | Wall median | CPU median | Worst p99 ratio |
|---|---:|---:|---:|---:|
| split 64 + 64 | 64 / 64 | 1.988x | 0.503 | 0.583 |
| split 96 + 64 | 96 / 64 | 1.986x | 0.503 | 0.566 |
| unified 128 | 128 / 0 | 1.984x | 0.504 | 0.581 |

The performance result therefore does not justify enlarging the hot cell. The
split 64 + 64 layout remains the prototype choice because it preserves the
smallest hot footprint while using an already-owned, allocation-free sidecar.

## Cross-platform validation

| Environment | Validation |
|---|---|
| macOS arm64, Apple Clang 21 | Make, CMake, CTest, full evidence matrix |
| macOS arm64, GCC 15 | fact and differential model tests |
| Linux arm64, GCC 15 | fact/model tests and timer-cancel benchmark smoke |
| Linux arm64, GCC 15 ASan/UBSan | fact and model tests |
| Linux arm64, GCC 15 TSan | concurrent fact tests |
| Windows x86_64, MinGW GCC 15 + Wine | fact/model tests and benchmark smoke |

The Windows pass found and fixed one portability defect: MinGW does not define
POSIX `ESTALE`. The fact contract now exposes `LCCF_FACT_ESTALE`, which maps to
the native constant where available and to a stable experiment-local value on
Windows. The supplied SSH Linux host was unreachable at the network layer, so
Linux validation used the local Linux/aarch64 container instead.

## Limits

- The model intentionally makes recompute paths repeat normalization and site
  resolution. The roughly 2x result is the isolated cost ceiling, not an
  expected production speedup.
- No real `io_uring`, kqueue, or IOCP kernel traffic was measured. Their input
  conventions are represented by deterministic adapters.
- p50/p99 values are per-round harness latency, not end-to-end application
  latency.
- No hardware instruction counters were available.
- Module lookup, callback code, and payload retention are modeled costs; real
  cache misses, allocator behavior, and scheduler contention may change their
  share of total runtime cost.
- The experiment does not modify `src/`, installed headers, stable ABI,
  version metadata, or release artifacts.

## Reproduction

```sh
make -j4 test-lccf-fact

python3 scripts/bench_lccf_fact.py \
  --binary ./bench_lccf_fact \
  --output-dir object/lccf-cfs-phase0 \
  --samples 5 \
  --rounds 1024 \
  --warmup-rounds 32 \
  --instances 257 \
  --chain 8
```

Raw rows are stored below `object/lccf-cfs-phase0/raw/`; the strict aggregate
is `object/lccf-cfs-phase0/summary.json`.
