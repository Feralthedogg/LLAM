# LCCF-CFS Phase 0 Common-Fact Model Implementation Plan

**Goal:** Decide whether building one immutable completion fact per winning generation removes measurable normalization and site-resolution work without stale decisions, lifetime errors, or direct-path regression.

**Architecture:** Create an independent branch from the accepted four-track specification, graft only the prior standalone LCCF model history, then add a fact sidecar and shared consume path under `experiments/lccf/`. Recompute and shared-fact modes use identical tickets, frames, callbacks, outcomes, and final-state oracles. No production I/O or Executor source changes.

**Tech stack:** Existing portable C11 LCCF model/platform shim, CMake/Make, Python 3 paired-evidence runner.

**Licensing:** New CFS files use `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`. Existing LCCF and build files retain their current Apache-2.0 headers.

**Phase boundary:** Standalone experiment only; no `src/`, installed header, runtime flag, or stable ABI change.

---

## Task 0: Reconstruct the minimal LCCF prerequisite branch

**Files:** Prior standalone LCCF experiment, runner, reports, and build integration only.

Cherry-pick the contiguous LCCF Phase 0 commits from `43c232e` through `707ce75`, excluding later SREM, LEIR, and production-I/O work. Resolve version/build conflicts against current `origin/main` without changing LCCF semantics. Run the original LCCF focused tests and reproduce its recorded negative decision before adding CFS.

Commit only conflict resolutions if required: `build: rebase LCCF model prerequisite`.

## Task 1: Define fact layout and state-machine contracts with failing tests

**Files:**

- Create: `experiments/lccf/lccf_fact.h`
- Create: `experiments/lccf/test_lccf_fact.c`

Define three layouts (`split64_64`, `split96_64`, `unified128`), stable fact fields, mutable guard fields, and states `ARMED`, `FACT_BUILDING`, `FACT_READY`, `RUNNING_DIRECT`, `QUEUED`, `RUNNING_QUEUED`, and `TERMINAL`.

Tests first assert:

- one winner can claim `ARMED(g)`;
- a losing or stale ticket cannot publish or queue;
- fact generation must match the cell generation;
- layout sizes and alignments meet their declared bounds;
- a published fact is immutable;
- reuse is rejected while callback, queue, backend, external, payload, or module references remain;
- invalid state transitions fail without altering visible data.

Confirm RED and commit: `test: define LCCF fact contracts`.

## Task 2: Implement fact construction, normalization, and publication

**Files:**

- Create: `experiments/lccf/lccf_fact.c`
- Create: `experiments/lccf/lccf_fact_layout.c`
- Modify: `experiments/lccf/test_lccf_fact.c`

Implement acq-rel generation claim, winner-owned reference transfer, canonical event normalization, one-time site resolution, ordinary fact writes followed by release publication, and acquire readers. Provide synthetic Linux, kqueue, IOCP, timer, cancel, external wake, and stop adapters that map equivalent logical outcomes to equal canonical events.

Use explicit counters for claim attempts, fact builds, normalization calls,
initial and continuation site lookups, pins, stale losers, and reuse delays.
Tests must show exactly one normalization and initial lookup per winning
generation, explicit lookup after a command changes `next_site`, and balanced
references for success and every injected failure.

Commit: `research: publish immutable LCCF facts`.

## Task 3: Share one consume function while rechecking mutable guards

**Files:**

- Modify: `experiments/lccf/lccf_fact.c`
- Modify: `experiments/lccf/lccf_fact.h`
- Modify: `experiments/lccf/test_lccf_fact.c`

Add failing tests for migration, stop, direct disable, fairness budget, trace mode, shard pause/offline, module policy, backend capability, callback-active, and queue pressure changes between publication and consumption.

Implement one `lccf_fact_consume()` for direct and queued modes. Stable event/site/payload fields come only from the acquired fact; every mutable condition comes from a fresh guard. The queued path may forward to a new home but may not normalize, resolve, or pin again. Stop/cancel after publication cannot overwrite the winning event.

Commit: `research: share guarded LCCF consumption`.

## Task 4: Add differential workloads and race/lifetime tests

**Files:**

- Modify: `experiments/lccf/lccf_model.h`
- Modify: `experiments/lccf/lccf_model.c`
- Modify: `experiments/lccf/lccf_workloads.c`
- Modify: `experiments/lccf/test_lccf_fact.c`

Add paired modes:

```text
recompute_queue / shared_fact_queue
recompute_fused / shared_fact_fused
mixed_recompute / mixed_shared_fact
```

First create tests that compare literal event/errno/command sequences and
existing canonical batch equality. Give each modeled resume site a distinct
function address so an initial descriptor cannot impersonate a continuation
site. Cover I/O-timeout, I/O-cancel, migration, stop, module disable, worker
offlining, generation reuse, callback failure, escape overflow, module
unregister busy, exactly-once frame/payload drop, and sidecar overwrite
prevention.

All modes must share the same seed-derived inputs and final checksum. Commit: `test: compare LCCF fact lifecycles`.

## Task 5: Add paired cost driver and strict evidence runner

**Files:**

- Create: `experiments/lccf/bench_lccf_fact.c`
- Create: `scripts/bench_lccf_fact.py`
- Create: `scripts/test_bench_lccf_fact.py`

Write strict parser/classifier tests first. Benchmark each layout and paired mode with identical operation counts, alternating ABBA/BAAB order in fresh processes. Emit wall, process CPU, optional instructions when supported, p50/p99, normalization/lookup counts, pin balance, allocations, guard rechecks, forwards, and checksum.

Retain raw samples and calculate process-local candidate/baseline ratios before
aggregation. Classify correctness before performance. Enforce queued >=98%,
direct regression <=2%, mixed CPU or instructions improvement >=5%, p99
regression <=5%, zero hot allocation, exactly one normalization and initial
site lookup per generation, and explicit continuation-site dispatch.

Commit: `bench: measure LCCF common facts`.

## Task 6: Integrate standalone targets and run cross-platform normalization

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

Add `test_lccf_fact` and `bench_lccf_fact` beside the existing LCCF targets. Do not link them into the runtime. Register the C and Python tests with CTest.

Verify direct Clang/GCC builds, ASan/UBSan, CMake, Make, and Windows compile/smoke where available. Canonical adapter tests must produce identical logical events on every platform.

Commit: `build: integrate LCCF fact research`.

## Task 7: Produce the CFS decision

**Files:**

- Generate: `object/lccf-cfs-phase0/*`
- Create: `docs/research/reports/2026-08-02-lccf-cfs-phase0-results.md`
- Create: `docs/research/reports/2026-08-02-lccf-cfs-phase0-decision.md`

Separate generic model results from platform normalization and hardware counter results. If correctness passes but measurable work does not improve, remove the persistent sidecar and keep only the canonical normalization helper. Only a passing independent result may authorize an Executor prototype.

Final verification:

```sh
git diff --check
cmake -S . -B object/lccf-cfs-full -DCMAKE_BUILD_TYPE=Release
cmake --build object/lccf-cfs-full -j4
ctest --test-dir object/lccf-cfs-full --output-on-failure
```

Do not bump or publish version 3.0.0 as part of this research.
