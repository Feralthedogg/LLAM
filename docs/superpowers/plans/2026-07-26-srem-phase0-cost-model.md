# SREM Phase 0 Cost Model Implementation Plan

> **For agentic workers:** Execute this plan task by task with tests and review
> checkpoints. Do not modify production runtime code before the evidence gate.

**Goal:** Determine whether compiler-shaped effect tiles, ready-mask
coalescing, and predicated effect superblocks can jointly provide a
category-level improvement over a conventional waker/coroutine executor.

**Architecture:** A standalone C11 model executes the same deterministic async
segments through AoS frames plus wakers and through persistent split-state
effect tiles. The native driver measures baseline and candidate in the same
fresh process using balanced ABBA/BAAB order; a Python runner screens tile
widths, runs an independent selected-width gate, retains every sample, and
classifies only process-local ratios.

**Tech stack:** C11, C11 atomics, pthreads and Win32 threads, CMake 3.20+, GNU
Make compatibility, Python standard library, Clang/GCC/MSVC warning policy,
ASan/UBSan, TSan where supported, and compiler vectorization remarks.

## Global Constraints

- Follow
  `docs/superpowers/specs/2026-07-26-llam-site-resident-effect-machine-design.md`.
- Keep Phase 0 standalone. Do not modify `src/`, public headers under
  `include/llam/`, the current stackful task representation, or ABI version.
- Preserve the user's root-worktree changes in
  `docs/operations/benchmarks.md` and `scripts/bench_deep_compare.py`; never
  stage or overwrite either file.
- Do not add a third-party dependency.
- Allocate frames, tiles, planes, tickets, masks, wakers, queues, producer
  teams, histograms, descriptors, and scratch storage before measured blocks.
  Every row must report `hot_allocations=0`.
- Baseline and candidate receive the same completion identities, generation
  validity, active lanes, completion order, event words, scalar program,
  logical effect descriptors, and round count.
- The baseline must be credible, not intentionally pessimized:
  - one contiguous AoS frame slab;
  - one reusable waker per invocation;
  - a bounded array ring;
  - generation validation before publication;
  - one module callback lookup per resumed invocation;
  - one callback runs the complete segment between real suspension points;
  - no allocation, reference counting, stealing, kernel wake, or artificial
    queue bounce in the measured path.
- Candidate admission and dirty-tile publication are measured. Do not provide
  pre-grouped active masks to the timed superblock.
- Tiles are formed when invocations start and remain fixed. Do not repack,
  sort, migrate, or compact lanes in the measured path.
- A homogeneous workload keeps lanes in a tile on the same suspension site
  except for the configured divergence mask. With `site_count=8`, tiles are
  distributed across eight sites; the candidate is not allowed to pretend the
  module has one site.
- Completion occupancy is the number of active lanes per tile in the backend
  batch already returned to the owner. The model may not wait to increase it.
- Test tile widths `8`, `16`, and `32`; hot frame footprints `64`, `128`, and
  `256` bytes; site counts `1` and `8`; occupancy `1`, `25%`, `50%`, and
  `100%`; divergence `0%`, `12.5%`, and `50%`.
- Use four workloads:
  - `srem_http_pipeline`;
  - `srem_rpc_pipeline`;
  - `srem_divergent_cancel`;
  - `srem_mixed_fairness`.
- Compare exact modes:
  - `waker_frame`;
  - `tile_scalar`;
  - `tile_vector`;
  - `adaptive_srem`;
  - `remote_waker_frame`;
  - `remote_adaptive_srem`.
- The vector loop must update inactive lanes with identity semantics. Scalar
  and vector modes must be field-for-field identical after every measured
  block.
- The adaptive threshold is selected only from screening data by a
  predeclared deterministic rule. Full-gate data uses a different seed and
  may not change the width or threshold.
- Use one native process per matrix sample. Odd samples use ABBA and even
  samples BAAB. Each paired mode accumulates at least `250 ms` for the full
  gate and uses the same calibrated round count.
- Use nine full-gate samples per cell. Calculate wall speedup and CPU ratio
  within the native process, then aggregate. Retain every raw row.
- Paired spread is maximum/minimum across the nine process-local ratios. Never
  remove an outlier or silently rerun a failing cell.
- On Linux, pin the owner and two persistent remote producers to distinct
  available CPUs when possible. Record affinity and clock sources. On macOS,
  record pthread QoS and `hw.activecpu` without changing either.
- Record compiler, flags, architecture, OS, active CPU count, source commit,
  selected width/threshold, exact command, and raw evidence paths.

## Measurement Repair Amendment

The first complete screening at source commit
`494321f7a0f9f7c1dc38b9a46c99de7fe9207ae3` produced all 600 required
samples but was `INCONCLUSIVE`: process-local wall-ratio spread reached
`5.012050x`, well above the predeclared `1.10x` ceiling. The version 1 driver
accumulated only two long blocks per mode, so a core migration or
descheduling interval could dominate one side of a pair. Follow-up 500 ms
and pinned Linux-container diagnostics still exceeded the stability gate.

This is an integrity repair, not a relaxed gate:

- result schema version 2 records `blocks_per_mode=16`;
- each process repeats eight balanced ABBA or BAAB quartets while retaining
  the same minimum accumulated time per mode;
- operation counts, metrics, checksums, and equality checks cover all 16
  blocks;
- the formal `1.50x` wall, `0.70x` CPU, and `1.10x` spread thresholds remain
  unchanged;
- authoritative evidence is collected on a manually dispatched Linux x86-64
  workflow with the benchmark owner pinned to one allowed CPU.

Version 1 rows remain diagnostic evidence only and cannot be mixed with
version 2 summaries.

## Formal Verdicts

One screened width receives `CATEGORY` only if all of these pass:

- both homogeneous workloads have worst-case wall speedup `>= 1.50x` and
  candidate CPU ratio `<= 0.70x` at occupancy `50%` and `100%`, every frame
  footprint, both site counts, and divergence `0%` and `12.5%`;
- `adaptive_srem` is at least `0.95x` baseline wall throughput at one-lane
  occupancy and at `50%` divergence;
- `remote_adaptive_srem` is at least `0.90x` the remote baseline wall
  throughput and at most `1.10x` its CPU time;
- mixed-fairness p99 service-gap degradation is `<= 10%`;
- exact canonical equality, cancellation, stale/duplicate ticket, drain-race,
  one-queue-presence, forced escape, and zero-hot-allocation checks pass;
- the compiler audit confirms widening in both homogeneous vector
  superblocks;
- every gate-driving wall and CPU ratio spread is `<= 1.10x`.

`SPECIALIZED` requires both homogeneous workloads to pass the same wall and
CPU gates for one explicitly named occupancy/site/divergence envelope, while
all sparse, remote, fairness, correctness, vectorization, and stability
controls still pass.

Use `REJECT` for a stable cost-model failure. Use `INCONCLUSIVE` only for
missing data, unsupported required execution, insufficient duration,
canonical mismatch, hot allocation, absent required vectorization, or spread
above `1.10x`.

The screening phase may stop before the full gate only when a
candidate-favorable bound cannot reach `1.50x` wall or `0.70x` CPU on both
homogeneous workloads. Record that as an engineering stop, not a formal full
matrix verdict.

## File Map

### C experiment

- `experiments/srem/srem_model.h`
  - public research model types, configuration, metrics, lifecycle, equality,
    checksum, and name parsing.
- `experiments/srem/srem_model_internal.h`
  - AoS frame, waker, ticket, tile header, plane offsets, descriptor planes,
    queue slots, producer team, and batch storage.
- `experiments/srem/srem_platform.h`
- `experiments/srem/srem_platform.c`
  - POSIX/Win32 thread synchronization, monotonic and process CPU clocks,
    affinity, QoS, and host metadata.
- `experiments/srem/srem_model.c`
  - configuration, allocation, deterministic reset, completion generation,
    admission, queues, six modes, canonical projection, and metrics.
- `experiments/srem/srem_workloads.c`
  - shared scalar segment formulas plus branchless planar scalar/vector
    superblocks for four workloads.
- `experiments/srem/test_srem_model.c`
  - unit, differential, race, occupancy, fairness, and allocation tests.
- `experiments/srem/bench_srem_model.c`
  - strict paired CLI, calibration, ABBA/BAAB measurement, equality, and one
    machine-readable row.

### Python

- `scripts/bench_srem_model.py`
  - strict row parser, screening and gate matrices, subprocess runner,
    deterministic width/threshold selection, summaries, integrity checks,
    verdicts, and CSV/JSON/Markdown evidence.
- `scripts/test_bench_srem_model.py`
  - parser, command construction, selection, gate, spread, report, and
    malformed-evidence tests.

### Integration

- `CMakeLists.txt`
- `Makefile`
- `.gitignore`

### Evidence

- `object/srem-phase0-screen/srem_phase0_samples.csv`
- `object/srem-phase0-screen/srem_phase0_summary.csv`
- `object/srem-phase0-screen/srem_phase0_metadata.json`
- `object/srem-phase0-screen/srem_phase0_report.md`
- `object/srem-phase0-gate/srem_phase0_samples.csv`
- `object/srem-phase0-gate/srem_phase0_summary.csv`
- `object/srem-phase0-gate/srem_phase0_metadata.json`
- `object/srem-phase0-gate/srem_phase0_report.md`
- `docs/superpowers/reports/2026-07-26-srem-phase0-results.md`
- `docs/superpowers/reports/2026-07-26-srem-phase0-decision.md`

Raw `object/` evidence remains ignored. A tracked report must be generated
from the same in-memory rows and remain byte-identical to its raw copy.

---

## Task 1: Canonical Program And Conventional Waker Baseline

**Create:**

- `experiments/srem/srem_model.h`
- `experiments/srem/srem_model_internal.h`
- `experiments/srem/srem_model.c`
- `experiments/srem/srem_workloads.c`
- `experiments/srem/test_srem_model.c`

**Public contract:**

```c
#define SREM_MODEL_MAX_TILE_WIDTH 32U
#define SREM_MODEL_MAX_SITES 8U
#define SREM_MODEL_MIN_FRAME_BYTES 64U
#define SREM_MODEL_MAX_FRAME_BYTES 256U

typedef enum srem_model_mode {
    SREM_MODEL_WAKER_FRAME = 0,
    SREM_MODEL_TILE_SCALAR = 1,
    SREM_MODEL_TILE_VECTOR = 2,
    SREM_MODEL_ADAPTIVE = 3,
    SREM_MODEL_REMOTE_WAKER_FRAME = 4,
    SREM_MODEL_REMOTE_ADAPTIVE = 5,
} srem_model_mode_t;

typedef enum srem_model_workload {
    SREM_MODEL_HTTP_PIPELINE = 0,
    SREM_MODEL_RPC_PIPELINE = 1,
    SREM_MODEL_DIVERGENT_CANCEL = 2,
    SREM_MODEL_MIXED_FAIRNESS = 3,
} srem_model_workload_t;

typedef struct srem_model_config {
    srem_model_workload_t workload;
    srem_model_mode_t mode;
    size_t instance_count;
    size_t frame_bytes;
    unsigned tile_width;
    unsigned active_lanes;
    unsigned site_count;
    unsigned divergence_eighths;
    unsigned vector_threshold;
    unsigned remote_producers;
    uint64_t seed;
} srem_model_config_t;

typedef struct srem_model_metrics {
    uint64_t completions;
    uint64_t claims;
    uint64_t stale_tickets;
    uint64_t duplicate_tickets;
    uint64_t queue_pushes;
    uint64_t queue_pops;
    uint64_t resume_calls;
    uint64_t tile_dispatches;
    uint64_t scalar_lanes;
    uint64_t vector_lanes;
    uint64_t vector_blocks;
    uint64_t forced_escapes;
    uint64_t remote_pushes;
    uint64_t fairness_samples;
    uint64_t fairness_p99_gap;
    uint64_t hot_allocations;
} srem_model_metrics_t;

typedef struct srem_model_batch srem_model_batch_t;

int srem_model_batch_create(const srem_model_config_t *config,
                            srem_model_batch_t **out_batch);
void srem_model_batch_destroy(srem_model_batch_t *batch);
int srem_model_batch_reset(srem_model_batch_t *batch);
int srem_model_run_round(srem_model_batch_t *batch,
                         srem_model_metrics_t *metrics);
bool srem_model_batch_equal(const srem_model_batch_t *lhs,
                            const srem_model_batch_t *rhs);
uint64_t srem_model_checksum(const srem_model_batch_t *batch);
```

The canonical invocation state has six `uint64_t` hot fields, generation,
site, step count, terminal state, and a five-field next-effect descriptor. AoS
frames pad to the configured footprint.

- [ ] Write failing parser, validation, reset, workload, and baseline tests.
- [ ] Implement strict configuration and overflow validation.
- [ ] Implement deterministic completion identities and active-lane rotation.
- [ ] Implement the AoS frame slab, reusable wakers, bounded pointer ring,
      generation claim, module callback lookup, full scalar segment, and
      per-frame descriptor.
- [ ] Verify baseline counts, partial final tiles, queue wraparound, canonical
      checksum, and `hot_allocations == 0`.
- [ ] Commit: `test: add conventional SREM frame baseline`

**Focused verification:**

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -Iexperiments/srem \
  experiments/srem/srem_model.c \
  experiments/srem/srem_workloads.c \
  experiments/srem/test_srem_model.c \
  -pthread -o /tmp/test_srem_model
/tmp/test_srem_model
```

## Task 2: Persistent Tiles, Ready Masks, And Scalar Control

- [ ] Write failing tests for field-plane alignment and offsets, partial tile
      masks, site masks, empty-to-nonempty publication, exactly one queue
      presence, active-lane rotation, stale and duplicate tickets, cancellation
      reuse, and canonical equality with `waker_frame`.
- [ ] Allocate all tile headers and planes in one aligned slab.
- [ ] Implement owner-local payload storage, generation validation, ready-mask
      admission, dirty-tile ring, snapshot/clear/recheck drain protocol, and
      scalar active-lane iteration.
- [ ] Keep homogeneous lanes on one tile site and apply deterministic
      divergence to the configured eighths.
- [ ] Implement cold-sidecar forced escape without touching unrelated lanes.
- [ ] Verify queue pushes are bounded by dirty tiles rather than completions.
- [ ] Commit: `feat: model site-resident scalar effect tiles`

## Task 3: Predicated Superblocks And Adaptive Crossover

- [ ] Write failing differential tests across every width, frame footprint,
      site count, active-lane count, divergence level, workload, partial tile,
      and 19 state transitions.
- [ ] Implement one branchless planar superblock per workload. Switch on the
      workload and site once per tile, never once per lane.
- [ ] Preserve inactive fields bit-for-bit with predicated identity updates.
- [ ] Emit next-effect fields in planes and include them in canonical equality.
- [ ] Implement `tile_vector` as forced vector-width work and `adaptive_srem`
      as scalar below the configured threshold.
- [ ] Add metrics that prove the selected path and useful-lane count.
- [ ] Compile with Clang and GCC vectorization remarks. Required homogeneous
      loops must be widened; store the audit command and relevant line numbers
      in the result report.
- [ ] Commit: `feat: add predicated SREM effect superblocks`

**Vectorization audit:**

```sh
clang -std=c11 -O3 -Rpass=loop-vectorize \
  -Rpass-missed=loop-vectorize -Iexperiments/srem \
  -c experiments/srem/srem_workloads.c -o /tmp/srem_workloads.o
```

## Task 4: Remote Producers And Fairness Controls

- [ ] Write failing persistent-team tests for two real producers, identical
      ticket sets, release/acquire payload visibility, concurrent mask OR,
      empty-to-nonempty publication, owner drain/recheck, cancellation races,
      no lost or double completion, and team reuse.
- [ ] Implement POSIX and Win32 platform layers without per-round thread
      creation.
- [ ] Give the remote waker baseline and remote adaptive candidate the same
      producer team, work partition, completion order, clocks, and affinity
      opportunity.
- [ ] Record process CPU including producer CPU.
- [ ] Add mixed-fairness service-gap histograms. A tile dispatch counts as
      service for each admitted lane; due placeholders are interleaved in the
      same deterministic completion stream.
- [ ] Verify one tile cannot monopolize more than one site superblock before
      the fairness check.
- [ ] Commit: `feat: model remote and fair SREM admission`

## Task 5: Paired Native Driver

- [ ] Write parser and integration tests before the driver.
- [ ] Implement strict CLI options for workload, pair, instances, frame bytes,
      tile width, active lanes, sites, divergence eighths, threshold,
      producers, seed, minimum duration, order, and warmup.
- [ ] Calibrate one shared round count for both modes without timing setup or
      teardown.
- [ ] Run ABBA or BAAB blocks, reset deterministically outside timing, validate
      canonical equality after each block, and emit exactly one versioned
      machine-readable row.
- [ ] Include wall seconds, process CPU seconds, ratios, both checksums,
      metrics, actual duration, rounds, affinity, clocks, compiler identity,
      and host identity.
- [ ] Reject unknown, duplicate, missing, trailing, nonfinite, zero, and
      overflow arguments.
- [ ] Commit: `bench: add paired SREM cost driver`

## Task 6: Screening, Selection, And Formal Classifier

- [ ] Write Python tests for strict row parsing, command construction, balanced
      orders, median and spread, candidate-favorable bound, deterministic
      width/threshold selection, every formal gate, every inconclusive reason,
      and report byte identity.
- [ ] Screening matrix:
  - two homogeneous workloads;
  - widths `8`, `16`, `32`;
  - active lanes `1`, `25%`, `50%`, `100%`;
  - frame `128`;
  - site count `1`;
  - divergence `0`;
  - `tile_scalar`, `tile_vector`, and `adaptive_srem`;
  - five fresh paired processes, at least `100 ms` per mode.
- [ ] Select the width maximizing the lower of the two workload median
      adaptive speedups at `50%` and `100%` occupancy. Break ties by lower
      candidate CPU ratio, smaller width, then smaller threshold.
- [ ] Select the smallest threshold whose sparse median is at least `0.95x`
      baseline and whose dense median is within 5% of forced vector.
- [ ] Use an independent full-gate seed.
- [ ] Full matrix:
  - both homogeneous workloads;
  - occupancy `50%`, `100%`;
  - frames `64`, `128`, `256`;
  - sites `1`, `8`;
  - divergence `0`, `12.5%`;
  - selected width/threshold and `adaptive_srem`;
  - sparse one-lane and `50%` divergence controls;
  - remote `50%` and `100%` controls;
  - mixed-fairness occupancy sweep.
- [ ] Generate samples CSV, summary CSV, metadata JSON, raw Markdown, and
      tracked Markdown atomically after all rows are present.
- [ ] Commit: `bench: classify paired SREM evidence`

## Task 7: Build And Portability Integration

- [ ] Add `test_srem_model` and `bench_srem_model` to Make and CMake with the
      repository warning policy.
- [ ] Add `test-srem-model`, `srem-model-screen`, and `srem-model-report`
      targets without making the full experiment part of ordinary CI.
- [ ] Add CTest coverage for the native and Python contract tests.
- [ ] Add root-only binary ignores and complete clean rules.
- [ ] Compile with Clang and GCC where available.
- [ ] Cross-build with MinGW and run native tests under Wine when available.
- [ ] Run ASan/UBSan, TSan, and cppcheck where supported.
- [ ] Verify Windows Make dry routing.
- [ ] Commit: `build: integrate SREM research harness`

## Task 8: Screening Decision

- [ ] Run all focused tests and sanitizers from a clean research build.
- [ ] Run the complete screening matrix with fresh processes.
- [ ] Confirm raw/tracked counts, durations, checksums, allocation metrics,
      compiler widening, and balanced pair orders.
- [ ] Compute the candidate-favorable bound before selecting a width.
- [ ] If both homogeneous workloads cannot possibly reach the formal wall and
      CPU gates, stop and write the tracked negative decision. Do not spend a
      full matrix to search for a preferred result.
- [ ] Otherwise freeze the selected width, threshold, and full seed in the
      metadata and continue.
- [ ] Commit: `docs: record SREM phase zero screening`

## Task 9: Full Gate And Engineering Decision

- [ ] Run the complete predeclared full matrix exactly once.
- [ ] If the formal verdict is `INCONCLUSIVE`, identify the environmental or
      integrity cause. Rerun only after repairing that cause and never relax a
      gate.
- [ ] Preserve the generated formal verdict verbatim.
- [ ] Write a separate engineering decision that distinguishes statistical
      classification from implementation judgment.
- [ ] Run `make test-quick`, a clean CMake Release build, complete CTest,
      `git diff --check`, and repository status inspection.
- [ ] Commit evidence and decision separately.
- [ ] Push the research branch and run the Linux, macOS, BSD, and stress
      workflows at the exact evidence commit.

## Task 10: Production Authorization Boundary

If the result is `CATEGORY`:

- create a new branch and plan only an internal Linux owner-local module/tile
  prototype;
- keep it behind a build flag and private header;
- do not bump the ABI or release version;
- require real `io_uring` completion admission and one generated C module
  before any public proposal.

If the result is `SPECIALIZED`:

- document the exact workload/occupancy envelope;
- prototype only if the envelope maps to a credible adopter;
- retain scalar behavior as the default.

If the result is `REJECT`, `INCONCLUSIVE` with dominant negative evidence, or a
screening stop:

- keep SREM as a documented negative result;
- do not add production branches, public symbols, or version changes;
- use the decomposition metrics to identify whether admission coalescing,
  layout, vector compute, or descriptor emission failed before choosing the
  next hypothesis.
