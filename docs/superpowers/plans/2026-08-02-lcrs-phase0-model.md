# LCRS Phase 0 Standalone Model Implementation Plan

**Goal:** Decide whether bounded coded-rotation victim discovery materially reduces probe work and victim fan-in while preserving the exact full-scan fallback contract.

**Architecture:** Build a private C11 topology constructor and selector simulator under `experiments/lcrs/`. The simulator operates on immutable synthetic shard snapshots; it never links to or changes the production scheduler. A strict Python runner compares global-deepest, deterministic random-k, coded rotation, and coded-plus-fullscan in fresh processes and retains raw evidence.

**Tech stack:** C11 atomics-free model code, CMake/Make, Python 3 standard library.

**Licensing:** Every new C, header, and Python file starts with `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`. Existing Apache-2.0 build files retain their existing header.

**Phase boundary:** No `src/`, installed header, runtime option, or stable ABI change.

---

## Task 1: Pin the public model contract with failing tests

**Files:**

- Create: `experiments/lcrs/lcrs_model.h`
- Create: `experiments/lcrs/test_lcrs_topology.c`

Define opaque topology ownership plus literal input/output records:

```c
typedef enum lcrs_policy {
    LCRS_POLICY_GLOBAL_DEEPEST,
    LCRS_POLICY_RANDOM_K,
    LCRS_POLICY_CODED_ROTATION,
    LCRS_POLICY_CODED_WITH_FULLSCAN,
} lcrs_policy_t;

typedef struct lcrs_shard_desc {
    uint32_t shard_id;
    uint32_t node_id;
} lcrs_shard_desc_t;

typedef struct lcrs_candidate {
    uint32_t shard_id;
    uint32_t depth;
    uint8_t is_remote;
} lcrs_candidate_t;
```

Expose constructor/destructor, row lookup, topology validation, and a snapshot-only selection function. The selector input explicitly carries online, paused, eligible, and depth fields; output carries the selected shard, probe count, fallback flag, and miss result.

Write table-driven tests whose expected candidate IDs and verdicts are hand-derived. Name the breaks they catch: self-candidate, duplicate row entry, locality escape, non-deterministic rebuild, accepting malformed partitions, selecting offline/paused victims, and failing to fall back under pressure.

Run the direct compile before implementation and confirm RED because the implementation symbols are absent:

```sh
mkdir -p object/lcrs-tdd
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g \
  -Iexperiments/lcrs \
  experiments/lcrs/test_lcrs_topology.c \
  -o object/lcrs-tdd/test_lcrs_topology
```

Commit after the failing contract is reviewed: `test: define LCRS phase zero contracts`.

## Task 2: Implement and verify the topology constructor

**Files:**

- Create: `experiments/lcrs/lcrs_topology.c`
- Modify: `experiments/lcrs/test_lcrs_topology.c`

Implement local nonzero cyclic offsets with a step coprime to `node_size - 1`. Construct remote slots by deterministic minimum-indegree assignment, with a stable integer mixer used only for tie-breaking. Store every row at initialization; row lookup performs no allocation and no modulo-based candidate generation.

Add exhaustive property loops for total shard counts 1 through 512 and partitions including one node, one shard per node, uneven nodes, primes, powers of two, and empty/duplicate/overflow-invalid descriptors. Independently count indegree from returned rows. Verify coverage before the palette repeats and ensure every constructor failure leaves the output pointer null.

Run and confirm GREEN:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -Iexperiments/lcrs \
  experiments/lcrs/lcrs_topology.c \
  experiments/lcrs/test_lcrs_topology.c \
  -o object/lcrs-tdd/test_lcrs_topology
object/lcrs-tdd/test_lcrs_topology
```

Commit: `research: construct verified LCRS palettes`.

## Task 3: Add selector simulation and conservative fallback

**Files:**

- Create: `experiments/lcrs/lcrs_simulator.c`
- Modify: `experiments/lcrs/lcrs_model.h`
- Modify: `experiments/lcrs/test_lcrs_topology.c`

First add failing selector cases for global deepest, random-k determinism, coded depth ordering, two-row epoch transition, candidate race fall-through, invalid/offline majority, miss streak, pressure, overflow, small-runtime cutoff, and deterministic no-steal mode.

Implement all four policies over the same immutable snapshot. Keep the exact global-deepest helper separate and call it for every fallback. Model a race-loss mask without mutating queues. Selection may sort at most eight stack candidates and must allocate nothing after topology construction.

Add an allocation counter around constructor-owned memory and assert that one million selections leave it unchanged.

Run the test binary again and commit: `research: simulate bounded LCRS selection`.

## Task 4: Add the paired benchmark driver

**Files:**

- Create: `experiments/lcrs/bench_lcrs_model.c`
- Modify: `experiments/lcrs/lcrs_model.h`

Before implementation, add a smoke invocation to the C test that expects one strict `LCRS_SAMPLE version=1` record from the benchmark entry point.

Model balanced, one-hot, rotating-hotspot, drain-tail, uneven-NUMA, and offline-churn workloads. For identical seed-derived snapshots, report policy, probes, successful selections, p50/p99 fan-in, throughput proxy, remote selections, fallbacks, and checksum. Reject zero iterations and malformed dimensions. One process emits exactly one schema row.

Run a four-policy smoke matrix and verify checksums agree where policies are required to drain identical work.

Commit: `bench: add LCRS selector cost driver`.

## Task 5: Classify fresh-process evidence

**Files:**

- Create: `scripts/bench_lcrs_model.py`
- Create: `scripts/test_bench_lcrs_model.py`

Write parser tests first for the exact schema, missing/duplicate/extra fields, non-finite numbers, wrong checksum, process failure, timeout, and row cardinality. Add hand-checked fixtures for ABBA and BAAB ordering and process-local ratio aggregation.

The runner must retain:

```text
object/lcrs-phase0/
  lcrs_samples.csv
  lcrs_summary.csv
  lcrs_metadata.json
  lcrs_report.md
```

Classify each workload independently. Enforce the design gates and the random-k complexity gate; never average away a failing core workload. Verdicts are `PASS`, `NARROW`, or `REJECT`.

Run:

```sh
LCRS_MODEL_TEST_BINARY=object/lcrs-tdd/bench_lcrs_model \
  python3 scripts/test_bench_lcrs_model.py
```

Commit: `bench: classify LCRS phase zero evidence`.

## Task 6: Integrate only research targets

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

Add `test_lcrs_topology` and `bench_lcrs_model` targets, include only `experiments/lcrs`, and register the C test plus Python parser/runner test with CTest. Do not link either target into `llam_runtime` or installation/export rules.

Verify Make, CMake, direct Clang sanitizers, and a Windows cross-compile configuration. Run the full existing test suite after focused tests.

Commit: `build: integrate LCRS research harness`.

## Task 7: Produce evidence and a decision record

**Files:**

- Generate: `object/lcrs-phase0/*`
- Create: `docs/superpowers/reports/2026-08-02-lcrs-phase0-results.md`
- Create: `docs/superpowers/reports/2026-08-02-lcrs-phase0-decision.md`

Run quick local evidence first, then a release build on a Linux host with at least 32 logical shards when available. Record platform-specific and generic model results separately. A negative result is valid and must not be rewritten as success.

Final verification:

```sh
git diff --check
cmake -S . -B object/lcrs-full -DCMAKE_BUILD_TYPE=Release
cmake --build object/lcrs-full -j4
ctest --test-dir object/lcrs-full --output-on-failure
```

Only a `PASS` or explicitly scoped `NARROW` decision may authorize Phase 1 shadow-mode work.
