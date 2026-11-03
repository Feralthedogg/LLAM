# LRPA Phase 0 Race-Amplifier Harness Implementation Plan

**Goal:** Determine whether synchronized N-lane composition detects and reproduces calibrated concurrency failures more effectively than equal-cost ordinary seed repetition.

**Architecture:** Build a test-only C11 harness under `experiments/lrpa/` with deterministic manifests, preallocated lane state, bounded traces, semantic failure signatures, and a first select/cancel/timeout model gadget. Python launches fresh processes, validates result schemas, replays failures, shrinks manifests, and compares empirical detection curves. Nothing is linked into release targets.

**Tech stack:** C11 threads/platform shim, CMake/Make, Python 3 standard library.

**Licensing:** New sources use `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`; existing build files retain Apache-2.0.

**Phase boundary:** Test and experiment targets only; no production library symbols, runtime flags, installed headers, or active release fault hooks.

---

## Task 1: Define manifest, lane, trace, and result contracts with failing tests

**Files:**

- Create: `experiments/lrpa/lrpa.h`
- Create: `experiments/lrpa/lrpa_internal.h`
- Create: `experiments/lrpa/test_lrpa_core.c`

Define size-fixed manifest fields for seed, lane count, rounds, gadget, coupling, worker count, queue capacity, perturbation count/hash, and fault ID. Define the lane states `ALLOCATED` through `DESTROYED`, bounded trace entries, allowed outcomes, accounting totals, and a normalized failure signature that excludes timestamps and addresses.

Write table-driven tests first for invalid dimensions, unknown enums, overflow, duplicate object IDs, malformed perturbations, illegal state transitions, trace truncation, and signature stability. Expected hashes and transitions are literal fixtures.

Confirm RED from absent implementation and commit: `test: define LRPA phase zero contracts`.

## Task 2: Implement platform-neutral coordination and bounded tracing

**Files:**

- Create: `experiments/lrpa/lrpa_core.c`
- Create: `experiments/lrpa/lrpa_platform.c`
- Create: `experiments/lrpa/lrpa_trace.c`
- Modify: `experiments/lrpa/test_lrpa_core.c`

Implement manifest validation, preallocation, barrier coordination, persistent host actors, lane state CAS transitions, abort propagation, bounded trace recording, and teardown. The platform shim must cover Windows and POSIX threads/events without exposing them in the public model header.

Add failing tests before each behavior: all actors must reach `ARMED` before release; one setup failure aborts every lane; timeout drains and joins all actors; trace overflow is reported without memory corruption; repeated reset returns accounting to baseline.

Run under ordinary and sanitizer builds, then commit: `research: coordinate LRPA lanes`.

## Task 3: Add the select completion gadget and real oracle

**Files:**

- Create: `experiments/lrpa/lrpa_oracle.c`
- Create: `experiments/lrpa/lrpa_gadget_select.c`
- Modify: `experiments/lrpa/lrpa_internal.h`
- Modify: `experiments/lrpa/test_lrpa_core.c`

Model send, close, cancel, timeout, and stale-node cleanup actors against one generation-protected completion cell. Coupling modes in Phase 0 are independent, shared object, ring, and colored graph; unsupported modes fail validation.

Test the observable invariants first:

- exactly one winner per armed generation;
- outcome belongs to the manifest's allowed set;
- losing payload is never exposed;
- no live wait node remains after drain;
- stale generation cannot complete a rearmed lane;
- global armed/winner/cancel/timeout/discard accounting balances.

The oracle consumes the actual lane state and trace; do not assert calls to test doubles. Commit after GREEN: `research: add LRPA select race gadget`.

## Task 4: Add deterministic perturbation and fault calibration

**Files:**

- Create: `experiments/lrpa/test_lrpa_faults.c`
- Modify: `experiments/lrpa/lrpa_core.c`
- Modify: `experiments/lrpa/lrpa_gadget_select.c`

Generate a fixed action list from the manifest seed using only defined-width integer arithmetic. Implement `YIELD`, `SPIN`, `BARRIER`, `TRIGGER`, `CANCEL`, `CLOSE`, and `TIMER_OFFSET`; reject platform-affinity actions until the platform shim supports them.

Faults exist only in a separately compiled target with `LRPA_ENABLE_FAULTS=1`. Start with skipped-winner-CAS and stale-generation-reuse calibration faults. Tests must prove the normal target cannot request a fault and each fault target produces the expected semantic signature.

Commit: `test: calibrate LRPA race detection`.

## Task 5: Add strict process output and replay

**Files:**

- Create: `experiments/lrpa/bench_lrpa.c`
- Create: `scripts/run_lrpa.py`
- Create: `scripts/test_run_lrpa.py`

Write Python parser tests first for an exact versioned JSON document, unknown/missing fields, invalid enum values, non-finite metrics, process timeout, partial output, and signature mismatch. The C driver accepts an explicit manifest path or complete CLI manifest and writes exactly one result document to stdout; traces and dumps go to the requested artifact directory.

The runner launches one fresh process per seed, uses the repository timeout helper, records commit/compiler/OS/arch/flags/affinity, preserves all results, and verifies deterministic failures by immediate replay. Artifact paths follow:

```text
object/lrpa/failures/<signature>/<seed>/
  manifest.json
  result.json
  trace.bin
  trace.txt
  runtime_dump.txt
  wait_graph.json
```

Commit: `bench: add LRPA campaign replay`.

## Task 6: Implement semantic shrinking

**Files:**

- Create: `scripts/shrink_lrpa.py`
- Create: `scripts/test_shrink_lrpa.py`

Build tests around a deterministic fake executable that accepts only manifests preserving a literal target signature. Verify binary lane reduction, perturbation delta debugging, round reduction, coupling simplification, worker reduction, alternative removal, and payload/capacity minimization. A candidate is accepted only when the real subprocess returns the same normalized signature; timeout or a different failure never counts.

Write every attempt and decision to a shrink log, emit a minimized manifest, and prove rerunning it reproduces the signature.

Commit: `bench: shrink LRPA failure manifests`.

## Task 7: Compare amplification against equal-cost repetition

**Files:**

- Modify: `scripts/run_lrpa.py`
- Modify: `scripts/test_run_lrpa.py`
- Create: `docs/research/reports/2026-08-02-lrpa-phase0-results.md`
- Create: `docs/research/reports/2026-08-02-lrpa-phase0-decision.md`

Run paired campaigns using the same total lane-executions and process budget:

```text
ordinary: lane_count=1, many seeds
amplified: lane_count=2/4/8, proportionally fewer seeds
```

Report detection probability, executions and wall time to first detection, replay rate, shrink success, signature stability, and cleanup failures without assuming independence. Retain raw JSON/CSV and classify `PASS`, `NARROW`, or `REJECT`. If lane composition gives no material improvement, remove the amplifier scheduler and retain only oracle/replay/shrink components.

Commit: `docs: record LRPA phase zero decision`.

## Task 8: Integrate test-only build targets and verify

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

Add normal core, fault-calibration, and driver targets. Register bounded PR-profile tests only. Do not install, export, or link LRPA objects into any runtime target.

Final verification:

```sh
git diff --check
cmake -S . -B object/lrpa-full -DCMAKE_BUILD_TYPE=Release
cmake --build object/lrpa-full -j4
ctest --test-dir object/lrpa-full --output-on-failure
```

Also run ASan/UBSan, timeout cleanup checks, deterministic replay, and the quick ordinary-versus-amplified calibration matrix before the decision commit.
