<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF-CFS Phase 0 Common-Fact Results

## Verdict

`INCONCLUSIVE`

The correctness and lifetime contract passes, including heterogeneous resume
sites, failure cleanup, overflow fallback, generation reuse, and weak-memory
lifecycle admission. The performance advancement gate does not pass because
the process-local wall, CPU, and p99 ratios exceed the predeclared 1.10x spread
limit. A controlled repeat produced the same outcome.

No Executor prototype, production integration, version change, or release is
authorized by this result.

## What changed after hostile review

The earlier provisional `PASS` is withdrawn. It depended on assumptions that
were too favorable to the candidate. The final model now enforces:

- generation-tagged ownership for every completion ticket;
- `RUNNING(g) -> TERMINAL(g) -> ARMED(g+1)`, with cleanup complete before the
  next generation becomes observable;
- one atomic lifecycle gate containing both the closed bit and in-flight
  operation count, avoiding a two-atomic missed-entry window on weak memory;
- physically distinct split 64+64, split 96+64, and unified 128 fact storage;
- a retained initial site descriptor that is actually invoked;
- eight distinct resume function addresses, with pointer/site correspondence
  checked before every callback;
- explicit continuation-site resolution when `next_site` changes, without
  mutating the published fact;
- one common consume/callback transaction with full abort cleanup;
- an allocation-free intrusive overflow fallback instead of task loss;
- literal per-callback traces, including nonzero error cases;
- strict baseline-work validation and a nonzero runner exit for every result
  other than `PASS`.

This removed an important overclaim. A fact may retain the initial resume-site
descriptor, but a general language runtime cannot reuse that function pointer
after a callback selects a different site. The final measurements include the
required continuation dispatch work.

## Correctness and lifetime evidence

| Check | Result |
|---|---:|
| Differential matrix | 72 / 72 equivalent |
| Literal trace mismatches | 0 |
| Rows with unbalanced references | 0 / 1,440 |
| Rows with hot-path allocation | 0 / 1,440 |
| Fact build failure rows | 0 / 1,440 |
| Generation mismatch rows | 0 / 1,440 |
| Reuse-delay rows | 0 / 1,440 |
| Overflow push/pop imbalance rows | 0 / 1,440 |
| Shared rows with normalization count other than one per generation | 0 |
| Shared rows with invalid site-lookup bounds | 0 |

The final controlled run measured 378,961,920 generations and 3,031,695,360
callbacks. Every paired process had the same canonical checksum, routing,
ticket, pin, queue, and completion counts.

Race and failure coverage includes:

- three-source publication with one generation winner;
- duplicate ticket delivery and double-consume admission;
- acquire versus finish versus explicit rearm across 2,000 generations;
- stale tickets arriving after a newer generation is armed;
- module and payload pin failure, malformed completion, and invalid site;
- callback failure after admission with complete reference teardown;
- migration, stop, fairness, tracing, pressure, callback-active, and offline
  shard guard changes;
- queue forwarding, deferral, direct-to-queue yield, and forced ring overflow;
- platform normalization equivalence for Linux CQE, kqueue, and IOCP shapes.

## Physical layout scope

The model allocates the advertised fact storage at the real address used by
each candidate:

| Layout | Hot bytes | Fact sidecar bytes |
|---|---:|---:|
| split 64 + 64 | 64 | 64 |
| split 96 + 64 | 96 | 64 |
| unified 128 | 128 | 0 |

The standalone harness also keeps a constant control object per instance for
state, ticket owners, references, and a fallback test record. That constant
allocation is present in every layout and is excluded from the advertised
layout footprint. Therefore Phase 0 proves placement and relative behavior,
not the production Executor's absolute bytes per instance.

## Benchmark method

The strict matrix is:

```text
4 workloads
x 3 paired mode families
x 3 fact layouts
x 2 site counts
= 72 cells
```

Each cell uses five repetitions with alternating `ABBA` and `BAAB` process
order. Each letter is a fresh process, yielding 20 raw rows and 10 local
candidate/baseline ratios per cell.

```text
instances       257
chain length    8
warmup rounds   32
measured rounds 1024
completions     263,168 per process
callbacks       2,105,344 per process
```

Ratios are:

```text
wall speedup = baseline wall / candidate wall
CPU ratio    = candidate CPU / baseline CPU
p99 ratio    = candidate round p99 / baseline p99
```

Hardware instruction counters were unavailable, so mixed-mode classification
used the declared process-CPU alternative.

## Quantitative result

The initial final run classified all 72 cells `INCONCLUSIVE`. The controlled
repeat classified one cell `PASS` and 71 `INCONCLUSIVE`. There were no
correctness or median-performance failures; every inconclusive result was due
only to ratio spread above 1.10x.

Controlled-repeat aggregates:

| Candidate | Cells | Wall min / median / max | Median CPU ratio | Worst median p99 ratio |
|---|---:|---:|---:|---:|
| `shared_fact_queue` | 24 | 1.144 / 1.611 / 2.096x | 0.668 | 0.908 |
| `shared_fact_fused` | 24 | 1.242 / 1.713 / 2.337x | 0.619 | 0.850 |
| `mixed_shared_fact` | 24 | 1.212 / 1.687 / 2.335x | 0.627 | 0.829 |
| **All cells** | **72** | **1.144 / 1.633 / 2.337x** | **0.641** | **0.908** |

Site cardinality is the important split:

| Sites | Cells | Wall min / median / max | Median CPU ratio | Interpretation |
|---|---:|---:|---:|---|
| 1 | 36 | 1.956 / 2.145 / 2.337x | 0.467 | monomorphic upper bound |
| 8 | 36 | 1.144 / 1.232 / 1.309x | 0.813 | heterogeneous runtime case |

The eight-site result is the credible generic-runtime signal: median wall
speedup is about 1.23x and median process CPU falls about 18.7%. The one-site
result is retained as an upper bound, not presented as the expected production
gain.

Layouts remain indistinguishable at this evidence quality:

| Layout | Cells | Wall median | CPU median |
|---|---:|---:|---:|
| split 64 + 64 | 24 | 1.631x | 0.639 |
| split 96 + 64 | 24 | 1.627x | 0.643 |
| unified 128 | 24 | 1.643x | 0.637 |

The controlled repeat's worst wall, CPU, and p99 ratio spreads were 3.699x,
1.425x, and 23.581x. These tails make the strict aggregate inconclusive even
though its medians are favorable. The next measurement must use longer
in-process paired windows or another predeclared noise model; weakening the
gate after seeing these results is not acceptable.

## Cross-platform validation

| Environment | Validation |
|---|---|
| macOS arm64, Apple Clang 21 | Make, CMake, 28-test CTest suite, ASan/UBSan, static analyzer, full matrix |
| Linux arm64 container, GCC 15 | fact/model tests, ASan/UBSan, TSan |
| Windows x86_64, MinGW GCC 15 + Wine | fact/model compile and execution |

The Windows build uses experiment-local errno fallbacks for POSIX values not
provided by MSVC/MinGW. The supplied external Linux host was unavailable at the
network layer, so the Linux checks used a local Linux arm64 container.

## Limits

- This is a standalone deterministic model, not real `io_uring`, kqueue, or
  IOCP traffic.
- Recompute modes intentionally repeat canonical event normalization at each
  modeled consumption. The result is a cost ceiling; a smaller normalization
  cache may capture much of the gain without a persistent fact object.
- Continuation sites now use distinct functions, but real AOT code size,
  indirect-branch prediction, module unload, and instruction-cache effects are
  not modeled.
- p50/p99 are per-round harness latency, not application latency.
- No hardware instruction counters were available.
- The experiment does not modify `src/`, installed headers, stable ABI,
  version metadata, or release artifacts.

## Reproduction

```sh
make -j4 test-lccf-fact

python3 scripts/bench_lccf_fact.py \
  --binary ./bench_lccf_fact \
  --output-dir object/lccf-cfs-phase0-honest-final \
  --samples 5 \
  --rounds 1024 \
  --warmup-rounds 32 \
  --instances 257 \
  --chain 8
```

The controlled repeat is under
`object/lccf-cfs-phase0-honest-final-rerun/`. Raw rows and strict summaries are
intentionally build-local evidence and are not release artifacts.
