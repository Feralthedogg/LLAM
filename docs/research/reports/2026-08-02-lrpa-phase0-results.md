# LRPA Phase 0 Results

Date: 2026-08-02

Track: LLAM Repetition-based Parallel Amplifier

Scope: standalone select-completion model and calibration faults only

Harness result: PASS

Runtime-integration status: HELD

## What was tested

The Phase 0 harness composes 1, 2, 4, or 8 lanes inside a fresh process. Each
lane uses persistent actors, a common release barrier, deterministic
perturbations, a bounded trace, and a generation-protected select-completion
cell. The model covers send, close, cancel, timeout, and stale-generation
completion attempts under independent, shared-object, ring, and colored-graph
coupling.

The semantic oracle checks:

- exactly one terminal winner for every armed generation;
- an explicitly allowed outcome;
- losing payload invisibility;
- complete wait-node drain;
- rejection of stale-generation completion;
- balanced armed, winner, cancel, timeout, and discard accounting;
- complete actor join and resource cleanup after success, setup failure, or
  timeout.

The ordinary target cannot activate calibration faults. A separately compiled
target provides skipped-winner-CAS and stale-generation-reuse faults. Fault
activation uses a deterministic, seed-derived rare window whose width is
`min(64, lane_count^2) / 64`. This is a synthetic calibration device, not an
estimate of a real LLAM defect probability.

## Equal-cost calibration

Every row in a fault profile received the same budget of 1,024 lane
executions: 64 lane slots, four workers, and four rounds. Higher-lane profiles
therefore used proportionally fewer fresh processes. Configuration order was
alternated and each profile used the same seed prefix. No independence between
lanes is assumed.

### Skipped winner CAS

| Lanes | Processes | Detections | Process rate | Detections / 1M budgeted executions | Budget to first | End-to-end wall to first | Replay | Cleanup failures |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 1 | 1.56% | 976.56 | 224 | 366.29 ms | 100% | 0 |
| 2 | 32 | 5 | 15.63% | 4,882.81 | 96 | 69.52 ms | 100% | 0 |
| 4 | 16 | 6 | 37.50% | 5,859.38 | 128 | 46.01 ms | 100% | 0 |
| 8 | 8 | 8 | 100.00% | 7,812.50 | 128 | 24.30 ms | 100% | 0 |

All 20 failures produced the same normalized signature,
`c75021df281c5663`.

### Stale generation reuse

| Lanes | Processes | Detections | Process rate | Detections / 1M budgeted executions | Budget to first | End-to-end wall to first | Replay | Cleanup failures |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 0 | 0.00% | 0.00 | — | — | — | 0 |
| 2 | 32 | 3 | 9.38% | 2,929.69 | 192 | 130.24 ms | 100% | 0 |
| 4 | 16 | 8 | 50.00% | 7,812.50 | 192 | 73.79 ms | 100% | 0 |
| 8 | 8 | 8 | 100.00% | 7,812.50 | 128 | 23.18 ms | 100% | 0 |

All 19 failures produced the same normalized signature,
`1eeee92553126a09`.

“Budget to first” counts scheduled work so early oracle termination cannot make
one profile appear cheaper. The raw result also records actual completed work.
End-to-end wall time includes process launch and environment capture, while the
driver separately reports measured race time.

## Clean campaign

The ordinary, fault-free binary completed 1,048,576 lane executions on an
Apple M4 running Darwin 25.5.0 with Apple Clang 21.0.0:

| Lanes | Processes | Executions | Oracle failures | Cleanup failures | Mean driver time | p99 driver time |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 262,144 | 0 | 0 | 30.22 ms | 53.06 ms |
| 2 | 32 | 262,144 | 0 | 0 | 72.05 ms | 109.66 ms |
| 4 | 16 | 262,144 | 0 | 0 | 153.74 ms | 241.87 ms |
| 8 | 8 | 262,144 | 0 | 0 | 285.65 ms | 367.84 ms |

This satisfies the one-million clean-execution quantity on the measured host,
but not the design's cross-platform gate. Trace truncation was permitted and
reported in this long bounded campaign; it did not weaken the state-based
oracle.

## Replay and shrinking

- Immediate replay reproduced 39 of 39 detected calibration failures with the
  same semantic signature.
- Ten retained failures, five per fault, were passed through the real C driver
  and semantic shrinker. All ten preserved the target signature and produced a
  smaller manifest.
- The minimized skipped-CAS cases required between one and six lanes, one
  worker, one round, queue capacity one, and no perturbation steps.
- The minimized stale-generation cases required between two and seven lanes,
  one worker, two rounds, queue capacity one, and no perturbation steps.
- Timeout, invalid output, a successful run, or a different signature is never
  accepted as a shrink.

The fact that all sampled failures shrink to zero perturbation steps is useful
negative evidence: this campaign validates synchronized lane composition and
the replay/oracle pipeline, but does not yet demonstrate additional value from
the current perturbation generator.

## Verification

- 14 C contract/oracle/coordination cases: PASS;
- 3 separately compiled fault-calibration cases: PASS;
- 10 runner/schema/replay/equal-cost/manifest-integrity cases: PASS;
- 3 shrinker cases, including the real C fault driver: PASS;
- focused Make target: PASS;
- focused Release CMake/CTest profile: 4 of 4 PASS;
- full Release CMake/CTest suite: 28 of 28 PASS;
- AddressSanitizer and UndefinedBehaviorSanitizer focused profile: PASS;
- Apple Clang static analyzer and Python bytecode compilation: PASS;
- production `src/`, installed headers, and public ABI changes: none.

## Defects found during hostile review

Four harness defects were reproduced and corrected before classification:

1. equal-cost accounting initially used completed work, allowing early fault
   termination to reduce a profile's apparent budget;
2. the zero-failure case incorrectly satisfied a vacuous two-times improvement
   comparison;
3. the shrinker was initially proven only against a fake executable and could
   not consume the C driver's manifest;
4. relative executable paths lost their `./` prefix when converted through
   `pathlib`, breaking the focused Make test.

The final driver now writes explicit, hash-checked perturbations; replay and
shrinking consume that same manifest format.

## Limits of this evidence

- The gadget models LLAM semantics but does not call production channel,
  select, timer, I/O, scheduler, or lifecycle code.
- The injected rare window deliberately grows with lane count. It validates
  calibration sensitivity, not real-world effect size.
- The one-million clean campaign ran on one local platform. Required-platform
  clean soaks remain outstanding.
- CPU affinity was unavailable on the measured macOS host.
- The test matrix is bounded for pull requests; nightly and weekly profiles do
  not yet exist.
- No real LSWG capture, I/O completion, join/reclaim, dynamic stealing, or LCCF
  gadget has been connected.

The corresponding advancement decision is recorded in
`2026-08-02-lrpa-phase0-decision.md`.
