# SREM Phase 0 Engineering Decision

Status: **STOP — formal result inconclusive, with dominant negative evidence**

Date: 2026-07-26

## Decision

Do not advance the Site-Resident Effect Machine (SREM) to a production
prototype, public ABI, or release.

The formal screening classifier returned `INCONCLUSIVE` because six of the
120 cells exceeded the predeclared `1.10x` process-ratio spread ceiling. This
is not relabeled as a formal `REJECT`.

The engineering decision is nevertheless to stop. No tile width had a valid
adaptive threshold, every sparse configuration missed its `0.95x` control,
and even a candidate-favorable calculation using the best observed wall and
CPU sample from every required core cell remained dramatically below the
`1.50x` wall and `0.70x` CPU targets. A full 64-cell, nine-sample gate cannot
reverse that screening result without selecting for a preferred outcome.

## Evidence Provenance

- Source commit:
  `df78d073ee09e28263c6fc7bac456897e61b2473`
- GitHub Actions run:
  [SREM Research #30221082292](https://github.com/Feralthedogg/LLAM/actions/runs/30221082292)
- Artifact: `srem-phase0-screen-linux-x86_64`
- Runner: Ubuntu 24.04, Linux `6.17.0-1020-azure`, AMD EPYC 7763,
  four vCPUs
- Owner affinity: CPU 0
- Compiler: GCC 13.3.0 at `-O3`
- Result schema: `SREM_PAIR version=2`, `blocks_per_mode=16`
- Matrix: 120 cells, five fresh processes per cell, 600 raw rows
- Pair order: 360 ABBA rows and 240 BAAB rows, as required by the odd/even
  five-sample schedule
- Minimum accumulated time: 100 ms per mode
- Generated report:
  `docs/research/reports/2026-07-26-srem-phase0-results.md`

The raw CSV, metadata, environment record, compiler optimization report, and
byte-identical report remain in the workflow artifact. The tracked report's
SHA-256 is
`884083420441b972f0daf573696af92882bb5d7fc93e06087f22e6b5ad0c18ae`.

## Formal Screening Result

The retained classifier output is:

```text
phase=screen
verdict=INCONCLUSIVE
reason=screen summary failed integrity controls
```

All 600 rows passed the non-statistical controls:

- baseline and candidate checksums matched;
- `hot_allocations=0`;
- every row met the declared duration;
- all operation, completion, queue, and vector accounting matched;
- native model, driver, Python contract, Linux, macOS, MinGW/Wine,
  sanitizer, and thread-sanitizer checks passed during harness development;
- Clang widened all four workload superblocks at the required source loops.

Six wall-ratio summaries and six CPU-ratio summaries exceeded `1.10x`.
The maximum spread was the same sparse RPC cell
(`adaptive_srem`, width 8, active 1, threshold 8):

```text
wall spread = 1.373689x
CPU spread  = 1.372615x
```

The first macOS version 1 screen was more unstable. Version 2 repaired the
known long-block scheduling bias and the authoritative Linux run reduced the
maximum spread from `5.012050x` to `1.373689x`; it did not make every cell
formally stable.

## Candidate-Favorable Stop Bound

Because stability failed, these numbers are diagnostic engineering bounds,
not a formal verdict. They deliberately favor SREM: for each required core
cell they use the best wall speedup and best CPU ratio among all five
processes, then take the required worst cell. The listed threshold is the
most favorable core threshold for that width.

| width | threshold | sparse median floor | favorable core wall floor | favorable core CPU ceiling |
|---:|---:|---:|---:|---:|
| 8 | 8 | 0.777842x | 0.645101x | 1.550143x |
| 16 | 16 | 0.581223x | 0.621635x | 1.608586x |
| 32 | 32 | 0.423131x | 0.582885x | 1.715755x |
| required | — | >= 0.950000x | >= 1.500000x | <= 0.700000x |

There was no valid threshold to freeze:

- the best sparse floor over any threshold was `0.786537x` at width 8;
- the best favorable core wall floor was only `0.645101x`;
- the best favorable core CPU ceiling was still `1.550143x`.

The observed direction is therefore not marginal. SREM was at least about
`1.55x` too CPU-expensive in the most favorable required configuration while
delivering at most about `0.65x` of baseline wall throughput.

## Mechanism Decomposition

The intended mechanisms did execute:

- dense ready-mask publication reduced runnable queue traffic by exactly
  `8x`, `16x`, and `32x` at the corresponding tile widths;
- all useful lanes ran through the forced vector path in dense vector cells;
- vector execution improved the dense tile candidate by roughly
  `1.23x` to `1.30x` over its scalar-tile form.

Those gains did not repay the candidate representation and execution costs.
Against the conventional waker/frame baseline, dense forced-vector cells
still achieved only `0.686x` to `0.744x` wall throughput and consumed
`1.344x` to `1.458x` CPU. Sparse planar state was worse because it retained
tile admission, mask, plane, and descriptor costs without enough useful
lanes to amortize them.

The failed component is therefore not queue coalescing or compiler
vectorization in isolation. It is the requirement that a general async
runtime keep suspended state in permanent planes and drive every transition
through a tile machine. The baseline's contiguous AoS frame and direct
per-invocation scalar transition are substantially cheaper on this x86
cost model.

## Consequences

- Do not run the full Phase 0 gate.
- Do not modify `src/`, public headers, ABI, or runtime defaults for SREM.
- Do not bump the project version or publish a release for this research.
- Keep the standalone model and its negative evidence; it prevents repeating
  the same permanent-plane/SIMD hypothesis under a different name.

The reusable result is narrower: completion publication can be reduced by a
dense bitmap without losing correctness. A future experiment should isolate
that mechanism while retaining AoS language frames. A more ambitious
compiler/runtime direction should eliminate whole suspension/resumption
boundaries by submitting declarative effect chains to the platform backend,
rather than trying to SIMD-vectorize arbitrary continuations after each
completion.
