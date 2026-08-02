<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF Representation Phase 0.5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether LLAM should retain no persistent completion payload, a 48-byte canonical event, or a 64-byte full fact with its initial resume descriptor.

**Architecture:** Keep one fact state machine, ownership protocol, guard path, callback transaction, and teardown path. Add a representation boundary that changes only persistent payload and materialization work, then measure A/B, A/C, and B/C in long-lived in-process ABBA/BAAB windows with predeclared bootstrap confidence gates.

**Tech Stack:** Portable C11 atomics, existing LCCF deterministic model, Make/CMake/CTest, Python 3 standard library statistics and subprocess support.

## Global Constraints

- Work only on `research/lccf-repr-phase0-5` in its isolated worktree.
- Preserve `RUNNING(g) -> TERMINAL(g) -> ARMED(g+1)` and the single atomic lifecycle gate.
- Use eight distinct resume function addresses in every performance cell.
- Keep module, payload, backend, callback, queue, external, and ticket ownership identical across A, B, and C.
- A, B, and C advertise real sidecars of 0, 48, and 64 bytes respectively.
- Do not modify `src/`, installed headers, version metadata, or release configuration.
- Do not publish or bump version 3.0.0.
- New files use `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`; modified files retain their existing header.
- Branch, commit, report, and pull-request wording must not contain the prohibited branch marker.

---

### Task 1: Define and test representation storage contracts

**Files:**

- Create: `experiments/lccf/lccf_representation.h`
- Create: `experiments/lccf/lccf_representation.c`
- Create: `experiments/lccf/test_lccf_representation.c`
- Modify: `Makefile`

**Interfaces:**

- Consumes: tagged `struct lccf_fact_ticket`, `struct lccf_fact_core`, and
  `struct lccf_fact_counters` through forward declarations; the implementation
  includes their complete definitions from `lccf_fact.h`.
- Produces: `lccf_representation_t`, `lccf_event_core_t`, storage-size queries, publication, and materialization functions used by Tasks 2 and 3.

- [ ] **Step 1: Write the failing layout and work-count test**

Create a real C test that includes the missing header and asserts hand-derived values:

```c
CHECK(sizeof(lccf_event_core_t) == 48U, "event core is 48 bytes");
CHECK(lccf_representation_sidecar_bytes(
          LCCF_REP_CANONICAL_HELPER) == 0U,
      "helper has no sidecar");
CHECK(lccf_representation_sidecar_bytes(
          LCCF_REP_SHARED_EVENT) == 48U,
      "event sidecar is 48 bytes");
CHECK(lccf_representation_sidecar_bytes(
          LCCF_REP_FULL_FACT) == 64U,
      "fact sidecar is 64 bytes");
```

Name the mutations caught: wrong struct padding, wrong representation-to-byte mapping, normalization on A publication, site lookup on B publication, or reuse of C's initial descriptor for a changed continuation site.

- [ ] **Step 2: Run the test and verify RED**

Run:

```sh
make -j4 test_lccf_representation
```

Expected: compilation fails because `lccf_representation.h` does not exist.

- [ ] **Step 3: Add the minimal representation API and storage implementation**

Define the exact public experiment types:

```c
typedef enum lccf_representation {
    LCCF_REP_CANONICAL_HELPER = 0,
    LCCF_REP_SHARED_EVENT,
    LCCF_REP_FULL_FACT,
    LCCF_REP_COUNT
} lccf_representation_t;

typedef struct lccf_event_core {
    uint64_t generation;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    uint32_t captured_home_shard;
    uint32_t source_node;
    int32_t error_code;
    uint8_t event_kind;
    uint8_t source_kind;
    uint16_t reserved;
} lccf_event_core_t;

_Static_assert(sizeof(lccf_event_core_t) == 48U,
               "LCCF event core must occupy 48 bytes");
```

Keep `lccf_representation.h` independent of `lccf_fact.h`: include only
`stddef.h` and `stdint.h`, forward-declare the three tagged fact structs, and
use those struct names in function prototypes. `lccf_fact.h` includes the
representation header; `lccf_representation.c` includes `lccf_fact.h` for the
complete definitions. This direction prevents a circular include.

Implement these signatures without heap allocation:

```c
size_t lccf_representation_sidecar_bytes(
    lccf_representation_t representation);
int lccf_representation_publish(
    lccf_representation_t representation,
    const lccf_fact_ticket_t *ticket,
    void *storage,
    lccf_fact_counters_t *counters);
int lccf_representation_materialize(
    lccf_representation_t representation,
    const lccf_fact_ticket_t *ticket,
    const void *storage,
    uint32_t site_index,
    lccf_fact_counters_t *counters,
    lccf_fact_core_t *out_fact);
```

A publication copies no payload and increments no normalization/lookup count.
B publication normalizes once into `lccf_event_core_t` and performs no lookup.
C publication calls the existing full normalization and resolves the initial
site. Materialization implements the exact count equations in the design.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```sh
make -j4 test_lccf_representation
```

Expected: all layout, normalization, lookup, invalid-index, and immutability cases pass with no compiler warnings.

- [ ] **Step 5: Commit the storage contract**

```sh
git add Makefile experiments/lccf/lccf_representation.h \
  experiments/lccf/lccf_representation.c \
  experiments/lccf/test_lccf_representation.c
git commit -m "test: define LCCF representation contracts"
```

### Task 2: Route the fact protocol through the representation boundary

**Files:**

- Modify: `experiments/lccf/lccf_fact.h`
- Modify: `experiments/lccf/lccf_fact.c`
- Modify: `experiments/lccf/lccf_fact_layout.c`
- Modify: `experiments/lccf/test_lccf_fact.c`
- Modify: `experiments/lccf/lccf_representation.c`

**Interfaces:**

- Consumes: representation API from Task 1.
- Produces: a fact cell initialized with one fixed representation and a real external payload pointer whose stride matches 0/48/64 bytes.

- [ ] **Step 1: Add failing protocol tests for all three representations**

Table-drive the existing publish, consume, failure cleanup, continuation-site,
and rearm cases over:

```c
static const lccf_representation_t REPRESENTATIONS[] = {
    LCCF_REP_CANONICAL_HELPER,
    LCCF_REP_SHARED_EVENT,
    LCCF_REP_FULL_FACT,
};
```

Add explicit assertions after one generation with three materializations, one
at the initial site and two at different continuation sites:

```text
A: normalizations=3, site_lookups=3
B: normalizations=1, site_lookups=3
C: normalizations=1, site_lookups=3
```

Then repeat the initial site twice before changing once:

```text
A: normalizations=3, site_lookups=3
B: normalizations=1, site_lookups=3
C: normalizations=1, site_lookups=2
```

Assert byte-for-byte immutability of B and C storage after every materialize.

- [ ] **Step 2: Run and verify RED**

Run:

```sh
make -j4 test_lccf_fact
./test_lccf_fact
```

Expected: the new A/B cases fail because the cell still stores only the boolean shared/full-fact policy.

- [ ] **Step 3: Replace the shared boolean with a fixed representation**

Change cell initialization to accept:

```c
int lccf_fact_cell_init(
    lccf_fact_cell_t *cell,
    uint64_t generation,
    lccf_fact_layout_t layout,
    uint32_t ticket_count,
    lccf_representation_t representation,
    void *representation_storage);
```

Store `representation` and `representation_storage` in the cell. Remove the
atomic shared boolean. Keep publication release/acquire unchanged. The winner
calls `lccf_representation_publish`; every direct or queued consumer calls
`lccf_representation_materialize`. Failure cleanup and reference retirement
remain outside the representation module.

For A, permit a null storage pointer. For B and C, reject null or misaligned
storage before any state transition. Clear only the exact advertised sidecar
size on initialization and rearm.

- [ ] **Step 4: Verify protocol GREEN and mutation coverage**

Run:

```sh
make -j4 test_lccf_representation test_lccf_fact
./test_lccf_representation
./test_lccf_fact
```

Expected: all cases pass. Temporarily changing B publication to increment
`site_lookups`, or C materialization to mutate external storage, must make a
named test fail; revert each mutation immediately.

- [ ] **Step 5: Commit the protocol integration**

```sh
git add experiments/lccf/lccf_fact.h experiments/lccf/lccf_fact.c \
  experiments/lccf/lccf_fact_layout.c \
  experiments/lccf/lccf_representation.c \
  experiments/lccf/test_lccf_fact.c
git commit -m "research: add selectable LCCF representations"
```

### Task 3: Add shared-event model modes and differential coverage

**Files:**

- Modify: `experiments/lccf/lccf_model.h`
- Modify: `experiments/lccf/lccf_model_internal.h`
- Modify: `experiments/lccf/lccf_model.c`
- Modify: `experiments/lccf/test_lccf_model.c`
- Modify: `experiments/lccf/bench_lccf_fact.c`
- Modify: `scripts/bench_lccf_fact.py`
- Modify: `scripts/test_bench_lccf_fact.py`

**Interfaces:**

- Consumes: representation-aware fact cell from Task 2.
- Produces: `shared_event_queue`, `shared_event_fused`, and `mixed_shared_event` model modes while preserving every existing mode.

- [ ] **Step 1: Write failing mode and trace-equivalence tests**

Add enum/name/parser expectations for:

```text
shared_event_queue
shared_event_fused
mixed_shared_event
```

For every four workloads and three route families, compare A/B/C with frame
sizes 64 and 256, cell bytes 64, sites 8, and chain 8. Assert literal trace,
checksum, routing, callback, command, completion, and reference equality.
Assert exact work formulas using the materialization count already derived by
the model.

- [ ] **Step 2: Run and verify RED**

Run:

```sh
make -j4 test_lccf_model
./test_lccf_model
python3 scripts/test_bench_lccf_fact.py
```

Expected: enum/parser and shared-event work assertions fail because the modes do not exist.

- [ ] **Step 3: Implement orthogonal route and representation mapping**

Add internal helpers returning route family and representation instead of
range-based enum assumptions:

```c
static lccf_representation_t mode_representation(lccf_model_mode_t mode);
static bool mode_is_fact_queue(lccf_model_mode_t mode);
static bool mode_is_fact_fused(lccf_model_mode_t mode);
static bool mode_is_fact_mixed(lccf_model_mode_t mode);
```

Allocate `instance_count * sidecar_bytes` with overflow checks. Use byte
strides from `lccf_representation_sidecar_bytes`; pass null for A. Report
`fact_sidecar_bytes` as 0/48/64 and retain the 64-byte causal hot-cell metric.

Update the existing Phase 0 benchmark parser only enough to recognize and
validate the new modes. Preserve its version-2 schema and old decisions.

- [ ] **Step 4: Run focused and regression tests GREEN**

Run:

```sh
make -j4 test-lccf-model test-lccf-fact
```

Expected: model, fact, old benchmark parser, and new differential matrix all pass.

- [ ] **Step 5: Commit model integration**

```sh
git add experiments/lccf/lccf_model.h \
  experiments/lccf/lccf_model_internal.h experiments/lccf/lccf_model.c \
  experiments/lccf/test_lccf_model.c experiments/lccf/bench_lccf_fact.c \
  scripts/bench_lccf_fact.py scripts/test_bench_lccf_fact.py
git commit -m "research: compare LCCF event and fact modes"
```

### Task 4: Define the evidence schema and bootstrap classifier first

**Files:**

- Create: `scripts/test_bench_lccf_repr.py`
- Create: `scripts/bench_lccf_repr.py`

**Interfaces:**

- Consumes: raw JSON lines from Task 5.
- Produces: strict parsing, process-median collapse, deterministic bootstrap intervals, representation eligibility, and final selection.

- [ ] **Step 1: Write failing Python tests with literal fixtures**

Create fixtures for one raw pair row with these required identity fields:

```text
schema_version, process_id, cell, contrast, block, pair, order,
minimum_window_ns, rounds, seed, left, right
```

Each side contains representation, wall/cpu/p50/p99, checksum, operation and
callback counts, normalization and lookup counts, sidecar bytes, all lifetime
counters, and balanced-reference booleans.

Tests must reject duplicate JSON keys, unknown fields, non-finite ratios,
duration below the minimum, checksum mismatch, wrong sidecar size, wrong work
counts, unbalanced order, and reference imbalance. Hand-write ratio fixtures
for:

```text
eligible B
eligible C
both eligible but C/B improvement below 2% -> select B
only C eligible -> select C
confidence interval crossing a gate -> INCONCLUSIVE
correctness mismatch -> REJECT
```

- [ ] **Step 2: Run and verify RED**

Run:

```sh
python3 scripts/test_bench_lccf_repr.py
```

Expected: import fails because `bench_lccf_repr.py` does not exist.

- [ ] **Step 3: Implement the minimal strict parser and classifier**

Use only the standard library. Collapse the eight window ratios from each
process/cell/contrast to one median before route aggregation. Implement a
deterministic bootstrap median interval:

```python
def bootstrap_median_ci(
    values: Sequence[float], *, seed: int,
    resamples: int = 20_000,
) -> tuple[float, float]:
    rng = random.Random(seed)
    medians = [statistics.median(
        values[rng.randrange(len(values))] for _ in values
    ) for _ in range(resamples)]
    medians.sort()
    return medians[int(0.025 * resamples)], \
        medians[int(0.975 * resamples) - 1]
```

Derive bootstrap seeds from a stable byte-wise FNV-1a digest, not Python's
process-randomized `hash()`.

- [ ] **Step 4: Run tests GREEN and compile-check**

Run:

```sh
python3 scripts/test_bench_lccf_repr.py
python3 -m py_compile scripts/bench_lccf_repr.py \
  scripts/test_bench_lccf_repr.py
```

Expected: all strict parser, confidence, and decision fixtures pass.

- [ ] **Step 5: Commit the evidence contract**

```sh
git add scripts/bench_lccf_repr.py scripts/test_bench_lccf_repr.py
git commit -m "test: define LCCF representation evidence"
```

### Task 5: Implement the long-lived three-contrast C benchmark

**Files:**

- Create: `experiments/lccf/bench_lccf_repr.c`
- Modify: `scripts/test_bench_lccf_repr.py`
- Modify: `scripts/bench_lccf_repr.py`

**Interfaces:**

- Consumes: model modes from Task 3 and the raw schema from Task 4.
- Produces: calibrated ABBA/BAAB raw evidence for A/B, A/C, and B/C.

- [ ] **Step 1: Add a failing binary contract test**

When `LCCF_REPR_TEST_BINARY` is set, invoke the binary with one workload,
64-byte frame, eight sites, 17 instances, chain 8, minimum window 1 ms, and
two blocks. Assert:

```text
3 contrasts x 2 blocks x 2 adjacent pairs = 12 rows
both ABBA and BAAB occur
every row meets minimum duration
all pair checksums and logical metrics match
rounds are positive and constant within a contrast
```

Also assert duplicate options, zero blocks, sites other than eight, minimum
window zero, and arithmetic overflow exit nonzero and write no sample rows.

- [ ] **Step 2: Run and verify RED**

Run:

```sh
LCCF_REPR_TEST_BINARY=./bench_lccf_repr \
  python3 scripts/test_bench_lccf_repr.py
```

Expected: binary is missing.

- [ ] **Step 3: Implement calibration and paired execution**

Create six batches, two per contrast. Before measured work, run equal warmup
and calibration work, doubling rounds until both sides of each contrast meet
the requested duration. Reset both batches and zero metrics.

For each block, choose ABBA or BAAB from SplitMix64 seeded with the process
seed, but invert any choice that would make the cumulative count imbalance
exceed one block. Pair adjacent unlike samples and verify equality when both
batches have completed equal cumulative rounds.

Allocate latency arrays before timing. Measure monotonic wall time and process
CPU, sort round latencies after each window, and emit p50/p99. Reject clock
rollback, zero CPU delta, failed model calls, count mismatch, checksum mismatch,
or unbalanced references.

- [ ] **Step 4: Run contract and focused tests GREEN**

Run:

```sh
make -j4 bench_lccf_repr
LCCF_REPR_TEST_BINARY=./bench_lccf_repr \
  python3 scripts/test_bench_lccf_repr.py
make -j4 test-lccf-model test-lccf-fact
```

Expected: raw schema and binary contract pass; all earlier tests remain green.

- [ ] **Step 5: Commit the benchmark**

```sh
git add experiments/lccf/bench_lccf_repr.c \
  scripts/bench_lccf_repr.py scripts/test_bench_lccf_repr.py
git commit -m "bench: pair LCCF representations in process"
```

### Task 6: Register portable build and test targets

**Files:**

- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: all Task 1-5 sources.
- Produces: `test_lccf_representation`, `bench_lccf_repr`, CTest registration, Windows focused target, clean-list entries, and `lccf-repr-report`.

- [ ] **Step 1: Write the failing build expectation**

Configure a fresh build and request the targets before registering them:

```sh
cmake -S . -B object/lccf-repr-red -DCMAKE_BUILD_TYPE=Release
cmake --build object/lccf-repr-red -j4 \
  --target test_lccf_representation bench_lccf_repr
```

Expected: target-not-found failure.

- [ ] **Step 2: Add Make and CMake registration**

Add `lccf_representation.c` to the fact core sources. Add the test and benchmark
executables to the same include definitions, platform definitions, and pthread
linkage as existing LCCF targets. Register:

```text
test_lccf_representation
test_bench_lccf_repr
```

The Python CTest receives
`LCCF_REPR_TEST_BINARY=$<TARGET_FILE:bench_lccf_repr>`. Extend clean, Windows
CTest regex, focused Windows build, aggregate `test`, and `.PHONY` entries.

- [ ] **Step 3: Verify fresh CMake and Make GREEN**

Run:

```sh
cmake -S . -B object/lccf-repr-final -DCMAKE_BUILD_TYPE=Release
cmake --build object/lccf-repr-final -j4
ctest --test-dir object/lccf-repr-final --output-on-failure
make -j4 test-lccf-repr
```

Expected: every CTest and focused representation test passes.

- [ ] **Step 4: Commit build integration**

```sh
git add Makefile CMakeLists.txt
git commit -m "build: integrate LCCF representation research"
```

### Task 7: Run evidence, validate platforms, and record the decision

**Files:**

- Create: `docs/research/reports/2026-08-02-lccf-repr-phase0-5-results.md`
- Create: `docs/research/reports/2026-08-02-lccf-repr-phase0-5-decision.md`
- Generate only: `object/lccf-repr-phase0-5/**`

**Interfaces:**

- Consumes: verified benchmark and classifier.
- Produces: retained raw evidence, summary JSON/CSV, a conservative selection, and the next-stage authorization boundary.

- [ ] **Step 1: Run the declared primary-host matrix**

Run:

```sh
python3 scripts/bench_lccf_repr.py \
  --binary ./bench_lccf_repr \
  --output-dir object/lccf-repr-phase0-5 \
  --samples 5 \
  --blocks 4 \
  --minimum-window-ms 25 \
  --instances 257 \
  --chain 8 \
  --frame-bytes 64,256
```

Expected: 24 cells, 120 processes, 40 paired ratios per contrast/cell, and a
decision derived without threshold changes.

- [ ] **Step 2: Audit raw evidence independently**

Use a separate read-only script invocation to confirm row counts, ABBA/BAAB
balance, minimum durations, exact work equations, zero bad references, and
that recomputing the summaries from raw JSONL reproduces `summary.json` byte
for byte.

- [ ] **Step 3: Run sanitizer, analyzer, and portability checks**

Run focused macOS Clang ASan/UBSan and static analyzer commands, Linux arm64
GCC normal/ASan/UBSan/TSan containers, and Windows x86_64 MinGW+Wine compile
and execution for the representation and model tests. Record unavailable
environment limitations exactly; never convert an unavailable platform into a
pass.

- [ ] **Step 4: Write result and decision reports**

The reports must state:

```text
correctness verdict
selected A/B/C or INCONCLUSIVE
all confidence intervals and gate boundaries
raw/process/cell sample counts
normalization and lookup reductions
0/48/64 physical payload scope and constant-control limitation
cross-platform evidence
no production, ABI, version, or release authorization
```

If C does not clear the B/C 2% upper-confidence gate, explicitly reject initial
descriptor retention even if its point estimate is faster.

- [ ] **Step 5: Run final verification and commit**

Run:

```sh
git diff --check
python3 -m py_compile scripts/bench_lccf_repr.py \
  scripts/test_bench_lccf_repr.py
cmake --build object/lccf-repr-final -j4
ctest --test-dir object/lccf-repr-final --output-on-failure
```

Then commit only source, tests, build files, spec/plan, and reports. Do not add
`object/` evidence:

```sh
git add docs/research/reports/2026-08-02-lccf-repr-phase0-5-results.md \
  docs/research/reports/2026-08-02-lccf-repr-phase0-5-decision.md
git commit -m "docs: record LCCF representation decision"
```

- [ ] **Step 6: Publish for review without release work**

Push `research/lccf-repr-phase0-5`, create a draft pull request targeting
`research/lccf-cfs-phase0`, and monitor every CI job to completion. Fix only
evidence-backed failures. Do not merge, tag, bump a version, or publish a
release.
