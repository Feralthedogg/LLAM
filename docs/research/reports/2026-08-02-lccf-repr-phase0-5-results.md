<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF Phase 0.5 Representation Results

## Verdict

Correctness: `PASS`

Representation decision: `SELECT_B`

The experiment selects the 48-byte shared-event sidecar:

```text
A  canonical helper, normalize and resolve on every materialization       0 B
B  normalize once at publication, resolve on every materialization      48 B
C  normalize and resolve once, re-resolve only after a site change       64 B
```

Both B and C satisfy every predeclared gate against A. C does not satisfy
the separate B/C advancement rule. Its overall process-CPU ratio has a 95%
confidence interval of `1.0187..1.0421`, where a value at or below `0.98`
was required. C also fails the mixed-route wall gate. The retained initial
site descriptor is therefore not justified by this evidence.

This result authorizes B only as the representation for the next test-only
Executor experiment. It does not authorize production integration, a stable
ABI change, a version change, or a release. Version 3.0.0 remains held.

## Evidence boundary

The primary run used commit `d6247ed` on an Apple M4 (`Mac16,12`) with macOS
Darwin 25.5, Apple Clang 21, and Python 3.12.7. Evidence is intentionally
build-local under:

```text
object/lccf-repr-phase0-5/
```

The retained projections are:

```text
raw/*.jsonl
run-config.json
summary.json
summary.csv
```

An independent `--audit-only` invocation reparsed every raw line, repeated
all work and lifetime checks, recomputed every confidence interval, and
reproduced `summary.json` and `summary.csv` byte for byte.

| Evidence item | Count |
|---|---:|
| Workload/route/frame cells | 24 |
| Independent processes | 120 |
| Contrasts per cell | 3 |
| Raw paired-ratio rows | 2,880 |
| Process/cell/contrast medians | 360 |
| Paired ratios per cell and contrast | 40 |
| Bootstrap resamples per interval | 20,000 |

Each process emitted four blocks and two adjacent pairs per block. Every
process/contrast contained four `ABBA` and four `BAAB` rows, used one fixed
calibrated round count, and kept both sides alive for the process lifetime.
The shortest measured side was `1.26712x` the declared 25 ms minimum.

## Correctness and lifetime evidence

The model retains the Phase 0 lifecycle and routing protocol and changes only
the fact representation. The following checks pass:

| Check | Result |
|---|---:|
| Existing A/C differential cases | 72 / 72 equivalent |
| New A/B/C triple differential cases | 24 / 24 equivalent |
| Raw pair checksum mismatches | 0 / 2,880 |
| Raw pair logical-metric mismatches | 0 / 2,880 |
| Rows with an exact-work-equation error | 0 / 2,880 |
| Rows with a bad module or payload lifetime | 0 / 2,880 |
| Rows with queue, overflow, backend, or ticket imbalance | 0 / 2,880 |
| Rows with a hot-path allocation | 0 / 2,880 |
| Rows below the minimum window | 0 / 2,880 |

Coverage includes timer/cancel losers, callback failure teardown, stale
generation isolation, continuation-site changes, direct and queued guards,
forced mixed-route escape, intrusive overflow fallback, and physical
representation addresses. An invalid site is materialized as an immutable
`FAIL/EPROTO` fact as required by the LCCF-CFS semantic contract.

Two benchmark defects were found before the final run:

- a wall-clock-derived fairness p99 was incorrectly compared as a logical
  identity metric; it is now validated independently while deterministic
  fairness sample counts remain equal;
- one transiently slow calibration window could approve too few rounds; the
  same round count must now satisfy the minimum duration twice consecutively.

Linux UBSan also exposed a representation test that read event storage after
an intentionally failed publication. The test now publishes a valid event
before checking invalid-site failure-fact semantics and separately rejects a
site index that cannot fit the fact representation.

## Benchmark method

The fixed matrix is:

```text
4 workloads
x 3 route families (queue, fused, mixed)
x 2 frame sizes (64 B, 256 B)
= 24 cells
```

Every cell runs A/B, A/C, and B/C as separate long-lived in-process paired
contrasts. Each process has 257 instances, eight heterogeneous resume sites,
and a callback chain of eight. Calibration, warmup, latency storage, batch
allocation, and sidecar allocation happen outside measured windows.

Ratios are oriented as follows:

```text
wall speedup = left wall / right wall       higher is better for right
CPU ratio    = right CPU / left CPU         lower is better for right
p99 ratio    = right round p99 / left p99   lower is better for right
```

Route-level intervals are deterministic bootstrap 95% confidence intervals
over process medians. Stable byte-wise FNV-1a derivation supplies a separate
bootstrap seed for every contrast, route, and metric.

## B eligibility against A

All A/B gates pass.

| Route | Metric | 95% CI | Gate | Result |
|---|---|---:|---:|---:|
| queue | wall speedup | 1.330902..1.336520 | lower >= 0.98 | PASS |
| queue | CPU ratio | 0.748315..0.751531 | observation | — |
| queue | p99 ratio | 0.750020..0.758154 | upper <= 1.05 | PASS |
| fused | wall speedup | 1.398719..1.476712 | lower >= 0.980392 | PASS |
| fused | CPU ratio | 0.677132..0.714966 | observation | — |
| fused | p99 ratio | 0.694629..0.726569 | upper <= 1.05 | PASS |
| mixed | wall speedup | 1.390675..1.450436 | observation | — |
| mixed | CPU ratio | 0.688784..0.719067 | upper <= 0.95 | PASS |
| mixed | p99 ratio | 0.694542..0.723167 | upper <= 1.05 | PASS |

## C eligibility against A

All A/C gates pass.

| Route | Metric | 95% CI | Gate | Result |
|---|---|---:|---:|---:|
| queue | wall speedup | 1.341096..1.348276 | lower >= 0.98 | PASS |
| queue | CPU ratio | 0.741563..0.745636 | observation | — |
| queue | p99 ratio | 0.746487..0.754299 | upper <= 1.05 | PASS |
| fused | wall speedup | 1.342141..1.382781 | lower >= 0.980392 | PASS |
| fused | CPU ratio | 0.722177..0.745575 | observation | — |
| fused | p99 ratio | 0.736524..0.754808 | upper <= 1.05 | PASS |
| mixed | wall speedup | 1.338122..1.352273 | observation | — |
| mixed | CPU ratio | 0.739692..0.747598 | upper <= 0.95 | PASS |
| mixed | p99 ratio | 0.742772..0.755336 | upper <= 1.05 | PASS |

## C advancement against B

C must clear a separate 2% overall CPU advantage and route-local wall and
p99 non-regression gates before its additional descriptor retention can be
selected. It does not.

| Scope | Metric | 95% CI | Gate | Result |
|---|---|---:|---:|---:|
| overall | CPU ratio | 1.018698..1.042092 | upper <= 0.98 | FAIL |
| queue | wall speedup | 0.998454..1.008756 | lower >= 0.98 | PASS |
| queue | CPU ratio | 0.991416..1.001543 | observation | — |
| queue | p99 ratio | 0.992201..1.010500 | upper <= 1.05 | PASS |
| fused | wall speedup | 0.926697..0.984248 | lower >= 0.98 | INCONCLUSIVE |
| fused | CPU ratio | 1.014675..1.079029 | observation | — |
| fused | p99 ratio | 1.033066..1.075800 | upper <= 1.05 | INCONCLUSIVE |
| mixed | wall speedup | 0.933959..0.955701 | lower >= 0.98 | FAIL |
| mixed | CPU ratio | 1.046205..1.070674 | observation | — |
| mixed | p99 ratio | 1.042133..1.063971 | upper <= 1.05 | INCONCLUSIVE |

The hard failures are sufficient to select B. In particular, the overall CPU
interval is entirely above 1.0: C is slower than B in this experiment, not
merely short of the required 2% win.

## Work reduction and physical storage

| Route | B normalization reduction vs A | C normalization reduction vs A | C lookup reduction vs B |
|---|---:|---:|---:|
| queue | 88.89% | 88.89% | 18.49% median |
| fused | 87.50% | 87.50% | 8.30% median |
| mixed | 87.88% | 87.88% | 11.08% median |

B captures the dominant normalization reduction. C removes an additional
fraction of site lookups, but the saved lookup does not pay for its larger
fact copy and descriptor-bearing sidecar on this host.

| Representation | Causal hot bytes | Physical sidecar bytes |
|---|---:|---:|
| A | 64 | 0 |
| B | 64 | 48 |
| C | 64 | 64 |

The harness retains a constant control allocation per instance for lifecycle,
ticket, and deterministic model state. That control exists for all three
representations and is excluded from the advertised sidecar footprint. The
result proves relative representation placement, not the future Executor's
absolute bytes per instance.

## Cross-platform validation

| Environment | Validation |
|---|---|
| macOS arm64, Apple M4, Apple Clang 21 | fresh CMake build, 30/30 CTest, focused Make tests, ASan+UBSan representation/model tests, Clang static analyzer |
| Linux arm64 container, GCC 15.3 | representation/model normal, ASan+LeakSanitizer, UBSan, and TSan execution |
| Windows x86_64, MinGW GCC 15.2 + Wine 11 | representation/model PE compile and execution |

Apple's sanitizer runtime does not support LeakSanitizer, so macOS leak
detection is not counted as a pass; Linux ASan ran with leak detection. The
supplied external Linux host timed out at the network layer, so Linux evidence
comes from a local native-arm64 Docker environment. Wine execution validates
the Windows code path and binary contract but is not a substitute for a later
native Windows hardware run.

## Limits

- This remains a deterministic standalone model, not the production Executor.
- No real `io_uring`, kqueue, or IOCP completion traffic is measured here.
- Eight sites are deliberately fixed; monomorphic best cases are excluded.
- p50 and p99 are per-round harness latency, not application request latency.
- The 0/48/64-byte sidecars are real allocations, but the common control
  object prevents an absolute production footprint claim.
- Hardware counters, code-size effects, instruction-cache behavior, module
  unload, and generated native effect segments are not measured.
- No file under `src/`, installed header, stable ABI, version metadata, tag,
  package, or release artifact is changed or authorized.

## Reproduction

```sh
make -j4 test-lccf-repr

python3 scripts/bench_lccf_repr.py \
  --binary ./bench_lccf_repr \
  --output-dir object/lccf-repr-phase0-5 \
  --samples 5 \
  --blocks 4 \
  --minimum-window-ms 25 \
  --instances 257 \
  --chain 8 \
  --frame-bytes 64,256

python3 scripts/bench_lccf_repr.py \
  --audit-only \
  --output-dir object/lccf-repr-phase0-5
```
