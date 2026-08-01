# LCRS Phase 0 Standalone Results

Date: 2026-08-02  
Source base: `a0674b7` plus the accepted four-track research specification  
Scope: standalone topology and selector model only; production `src/` unchanged

## Result

The standalone correctness model passes. The macOS arm64 paired model campaign also passes its model-level classifier, but it is not native scheduler throughput evidence.

```text
model classifier: PASS
integration decision: NARROW
```

## Correctness coverage

- Local coded palettes were exhaustively constructed for every shard count from 1 through 512.
- Representative uneven two-node topologies covered sizes through 512 shards.
- Every row rejects self-candidates and duplicate candidates.
- Local candidates remain in the thief locality; remote candidates leave it.
- Every eligible peer is covered before the generated palette cycle completes.
- Repeated construction with the same topology and runtime identity is byte-deterministic.
- Malformed, duplicate, sparse, empty, and width-overflow configurations fail closed.
- Selection tests cover global deepest, deterministic random-k, coded-only, coded plus fallback, next-epoch probing, race loss, offline/paused candidates, pressure, overflow, miss streak, small-runtime cutoff, deterministic no-steal mode, and local-before-remote ordering.

One implementation defect was found by the uneven-topology test: the constructor initially published the final `remote_count` before all remote slots were filled. Zero-filled future slots then falsely excluded shard 0 as a duplicate. The implementation now exposes only the number of populated remote slots, and the regression remains covered by the 2+3 topology case.

## Paired model campaign

Command:

```sh
python3 scripts/bench_lcrs_model.py \
  --binary object/lcrs-cmake/bench_lcrs_model \
  --out-dir object/lcrs-phase0 \
  --samples 5 \
  --shards 32 \
  --nodes 2 \
  --iterations 5000 \
  --seed 20260802 \
  --timeout 30
```

Platform: macOS 26.5.1, arm64, AppleClang 21, Python 3.12.7.

Each median row represents one fresh process. Policy order alternated across samples. Every policy within a workload produced the same input checksum.

| Workload | Global probes | Coded+fallback probes | Global p99 fan-in | Coded+fallback p99 | Random-k p99 | Coded-only p99 |
|---|---:|---:|---:|---:|---:|---:|
| balanced | 2,480,000 | 320,000 | 16 | 3 | 4 | 3 |
| one_hot | 2,480,000 | 1,042,496 | 16 | 10 | 8 | 5 |
| rotating_hotspot | 2,480,000 | 723,720 | 16 | 8 | 7 | 5 |
| drain_tail | 2,480,000 | 1,055,032 | 15 | 9 | 8 | 5 |
| uneven_numa | 2,480,000 | 320,000 | 16 | 4 | 6 | 4 |
| offline_churn | 2,480,000 | 329,992 | 16 | 3 | 4 | 3 |

Aggregate depth probes fell from 14,880,000 to 3,791,240, a 74.52% reduction. Steady-state p99 fan-in reductions were 81.25% for balanced, 75% for uneven NUMA, and 81.25% for offline churn. The hotspot-specific reductions were 37.5% for one-hot and 50% for rotating hotspot, both above the separate 10% hotspot gate.

Coded-only beat deterministic random-k on p99 fan-in in all six workloads. Coded+fallback performed no exact fallback in balanced or steady uneven-NUMA samples; sparse workloads used the second palette row, which increased probe count and recovered the full productive-work proxy.

## Verification

- `make test-lcrs-model`: passed.
- Focused CMake/CTest: 2/2 passed.
- Full release CTest: 26/26 passed.
- Direct AppleClang ASan/UBSan topology and selector run: passed.
- macOS LeakSanitizer option: unavailable on this platform; no leak claim is made from that run.
- Windows Make delegation dry run: generated the two focused CMake targets and tests.
- New C, header, and Python files carry `LicenseRef-LLAM-Commercial-Reciprocity-1.0`.

## Limitations

- The benchmark is a deterministic selector/collision model, not a live Chase-Lev queue benchmark.
- `productive_units` is a bounded work-distribution proxy, not measured runtime throughput.
- Candidate decisions share immutable snapshots and do not reproduce real CAS timing or cache coherence.
- The model's random-k constructor scans topology identities to obtain deterministic lowest hashes. Wall/CPU timings therefore are not used to claim a coded advantage over a production-quality O(k) random sampler.
- The supplied Linux host was unreachable before authentication (`Network is unreachable`), so Linux native evidence remains absent.
- No Phase 1 scheduler source, public ABI, release version, or package was changed.
