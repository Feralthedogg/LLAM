# LCWE Phase 1 Cost Model Results

## Verdict

`INCONCLUSIVE`

The classifier emitted `INCONCLUSIVE` in both full runs because one or more
process cells exceeded the pre-registered `1.15x` maximum-to-minimum wall-time
spread. This is a standalone cost-model result, not production validation.

The engineering decision is narrower than the classifier verdict:

- do not implement the Phase 1 pointer-wave or capsule-wave design in the
  production runtime;
- retain the compiler-facing Executor-plane goal;
- replace sort/pack/SIMD batching with a direct completion-to-continuation
  experiment.

That decision does not relabel the experiment as `REJECT`. It follows from the
large, repeatable gap between every realistic layout and the performance gate,
including a deliberately candidate-favorable bound over both noisy runs.

## Reproduction

- Branch: `codex/lcwe-phase1`
- Harness commit: `e6d5eb72f7672f27990742364d82be87d60fe8eb`
- Host: Apple M4, `macOS-26.5.1-arm64-arm-64bit`
- Compiler: `Apple clang version 21.0.0 (clang-2100.1.1.101)`
- Python: `3.12.7`
- Instances: `65536`
- Process samples per cell: `7`
- Seed: `7810762890074515045`
- Matrix: 84 cells and 588 process samples per full run

The pre-registered full command was:

```sh
python3 scripts/bench_lcwe_model.py \
  --binary ./bench_lcwe_model \
  --samples 7 \
  --instances 65536 \
  --rounds 31 \
  --warmup 5 \
  --seed 7810762890074515045 \
  --widths 1,2,4,8,16,32 \
  --cc clang \
  --out-dir object/lcwe-phase1
```

It produced 26 unstable cells out of 84, with a maximum spread of `5.5978x`.
Raw samples showed short 17–18 ms cells interrupted for as long as 94.6 ms,
while their CPU time remained much lower than wall time.

The plan explicitly allowed one rerun for `INCONCLUSIVE` spread. The rerun kept
the binary, cells, instances, widths, seed, and seven-process sampling fixed,
but extended each measured cell from 26 to 768 rounds:

```sh
python3 scripts/bench_lcwe_model.py \
  --binary ./bench_lcwe_model \
  --samples 7 \
  --instances 65536 \
  --rounds 831 \
  --warmup 63 \
  --seed 7810762890074515045 \
  --widths 1,2,4,8,16,32 \
  --cc clang \
  --out-dir object/lcwe-phase1-rerun
```

The rerun still produced 37 unstable cells out of 84, with a maximum spread of
`1.5318x`. During the run, unrelated FileProvider, Music, Discord, metadata,
cloud-drive, and WindowServer processes consumed substantial CPU. macOS
reported no thermal or CPU power warning. A five-cell `utility`-QoS calibration
also remained unstable (`1.0938x` to `1.3834x`), so a third full run was not
used to search for a preferred verdict.

Generated evidence is preserved locally in:

- `object/lcwe-phase1/lcwe_phase1_samples.csv`
- `object/lcwe-phase1/lcwe_phase1_summary.csv`
- `object/lcwe-phase1/lcwe_phase1_report.md`
- `object/lcwe-phase1-rerun/lcwe_phase1_samples.csv`
- `object/lcwe-phase1-rerun/lcwe_phase1_summary.csv`
- `object/lcwe-phase1-rerun/lcwe_phase1_report.md`

## Correctness

The differential model compared every candidate with the scalar reference
after every round over:

- all three fixed workloads: `exec_io_pipeline`, `exec_rpc_state`, and
  `exec_event_fanout`;
- three deterministic seeds;
- lane widths `1`, `2`, `4`, `8`, `16`, and `32`;
- instance counts `37` and `257`;
- cohort, pointer-wave, capsule-wave, and persistent AoSoA modes;
- 19 state-transition rounds per case.

Every field and checksum matched. The following focused verification passed:

```sh
make test-lcwe-model CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -g -fno-omit-frame-pointer'
```

The same differential test passed under AddressSanitizer and
UndefinedBehaviorSanitizer with:

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iexperiments/lcwe \
  experiments/lcwe/test_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/test_lcwe_model_asan
/tmp/test_lcwe_model_asan
```

Project verification also passed:

- `make test-quick`, including LCWE, runtime, stress, security, and composite
  server tests;
- CMake Release configure and complete build;
- a clean CTest rerun: 24/24 passed.

The first CTest invocation had one transient
`test_runtime_shutdown_internal` close/watch timing failure. LCWE changes do
not touch its source or production I/O implementation. The identical test
binary then passed 300 consecutive direct repetitions and the complete CTest
rerun. No speculative runtime change was made without a reproducible root
cause.

## Vectorization

The Clang `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize` audit showed:

| Workload loop | Pointer wave | Capsule compute | AoSoA upper bound |
|---|---|---|---|
| `exec_io_pipeline` | not vectorized, line 98 | vectorized width 4, line 192 | vectorized width 4, line 292 |
| `exec_rpc_state` | not vectorized, line 128 | vectorized width 4, line 220 | vectorized width 4, line 331 |
| `exec_event_fanout` | not vectorized, line 161 | vectorized width 4, line 253 | vectorized width 4, line 375 |

This confirms the layout premise but not the performance premise. Pointer
chasing prevented vectorization. Capsule packing enabled vectorization, but its
pack/unpack cost outweighed the vector compute. Persistent AoSoA removed that
copy cost and was fastest, yet still missed the Phase 1 gates.

## Performance

The following values are the selected medians from the generated extended-run
report. Pointer and capsule rows are realistic layouts. AoSoA is an upper bound
and cannot by itself produce `PROMISING`.

| Workload | Mode | Lanes | Layout | Wall ns/op | CPU ns/op | Wall speedup | CPU reduction | p99 ns/op | Spread |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| `exec_event_fanout` | scalar | 1 | baseline | 10.11 | 10.06 | 1.00x | 0.0% | 12.80 | 1.138x |
| `exec_event_fanout` | cohort | 1 | baseline | 13.46 | 13.42 | 0.75x | -33.4% | 17.03 | 1.247x |
| `exec_event_fanout` | wave_pointers | 16 | realistic | 11.45 | 11.44 | 0.88x | -13.7% | 12.47 | 1.088x |
| `exec_event_fanout` | wave_capsule | 32 | realistic | 13.75 | 13.74 | 0.74x | -36.6% | 15.23 | 1.220x |
| `exec_event_fanout` | wave_aosoa | 32 | upper-bound only | 8.57 | 8.48 | 1.18x | 15.7% | 14.47 | 1.033x |
| `exec_io_pipeline` | scalar | 1 | baseline | 9.60 | 9.58 | 1.00x | 0.0% | 11.50 | 1.298x |
| `exec_io_pipeline` | cohort | 1 | baseline | 12.72 | 12.70 | 0.75x | -32.6% | 14.15 | 1.035x |
| `exec_io_pipeline` | wave_pointers | 16 | realistic | 11.20 | 11.09 | 0.86x | -15.8% | 17.42 | 1.299x |
| `exec_io_pipeline` | wave_capsule | 32 | realistic | 13.79 | 13.78 | 0.70x | -43.8% | 15.69 | 1.273x |
| `exec_io_pipeline` | wave_aosoa | 32 | upper-bound only | 8.27 | 8.25 | 1.16x | 13.9% | 9.44 | 1.042x |
| `exec_rpc_state` | scalar | 1 | baseline | 9.39 | 9.36 | 1.00x | 0.0% | 17.25 | 1.075x |
| `exec_rpc_state` | cohort | 1 | baseline | 12.83 | 12.78 | 0.73x | -36.5% | 17.20 | 1.077x |
| `exec_rpc_state` | wave_pointers | 16 | realistic | 11.28 | 11.26 | 0.83x | -20.3% | 12.92 | 1.054x |
| `exec_rpc_state` | wave_capsule | 32 | realistic | 13.94 | 13.92 | 0.67x | -48.7% | 15.36 | 1.237x |
| `exec_rpc_state` | wave_aosoa | 32 | upper-bound only | 8.18 | 8.17 | 1.15x | 12.7% | 9.06 | 1.199x |

The pre-registered workload gate required both:

- throughput at least `1.50x` scalar;
- CPU ns/op at most `0.70x` scalar.

No selected realistic row was faster than scalar. The best realistic medians
were only `0.88x`, `0.86x`, and `0.83x`; CPU consumption increased by
13.7%–20.3%. Even the persistent AoSoA upper bound reached only
`1.15x`–`1.18x`, with 12.7%–15.7% CPU reduction.

## Candidate-Favorable Bound

To ensure the stop decision does not depend on a noisy median, a deliberately
unfair bound was computed over all 1,176 raw process samples from both full
runs. For every workload, it compares the slowest observed scalar with the
fastest observed candidate and independently gives the CPU gate the largest
scalar CPU time and smallest candidate CPU time.

| Workload | Realistic maximum possible wall speedup | Realistic minimum possible CPU ratio | AoSoA maximum possible wall speedup | AoSoA minimum possible CPU ratio |
|---|---:|---:|---:|---:|
| `exec_event_fanout` | 1.0009x | 1.0028x | 1.3329x | 0.7528x |
| `exec_io_pipeline` | 1.1223x | 1.0066x | 1.5295x | 0.7384x |
| `exec_rpc_state` | 1.2849x | 0.9049x | 1.7332x | 0.6706x |

Every realistic layout still fails both gates under that bound. Only one AoSoA
workload can satisfy both gates, while `LAYOUT_BLOCKED` requires at least two.
Consequently, measurement stability must still be fixed before assigning a
formal non-`INCONCLUSIVE` classifier label, but there is no evidence-based case
for spending production-runtime risk on this batching mechanism.

## Interpretation

The failed assumption was not “SIMD can help.” SIMD did help once the data was
contiguous. The failed assumption was that generic continuation work is dense
enough to repay cohort discovery, grouping, and layout conversion:

1. stable cohort formation alone reduced throughput to `0.73x`–`0.75x`;
2. pointer waves could not vectorize and remained slower than scalar;
3. capsule waves vectorized, but pack/unpack made them the slowest realistic
   layout;
4. removing pack/unpack entirely still left AoSoA far below the `1.50x` and
   `0.70x` gates.

LCWE wave batching should therefore remain a documented negative result, not a
production feature or public ABI.

## Next Action

Keep the language-neutral Executor-plane objective, but move the optimization
boundary from “batch many resumptions” to “remove a resumption round trip.”
The next experiment will model a completion-threaded continuation cell:

- the compiler supplies a stable resume-site function and compact frame;
- the I/O/timer backend writes result, generation, cancellation state, and
  resume site into one runtime-owned completion cell;
- a same-shard completion spends a bounded continuation budget and directly
  invokes the resume site, avoiding waker allocation, generic runnable-queue
  insertion, cohort sorting, and capsule copying;
- cross-shard, fairness, blocking, and foreign callbacks fall back to the
  existing scheduler through an explicit ring path;
- the ABI stays C-compatible so C, other language runtimes, and compiler
  backends can all target it.

This successor must first beat a standalone scalar cost model and a
queue-based continuation baseline before any production header or runtime
source changes.
