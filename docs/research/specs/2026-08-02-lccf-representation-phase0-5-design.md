<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF Representation Phase 0.5 Design

Status: approved standalone experiment

## 1. Decision

Phase 0.5 compares three representations on one generation, ownership, guard,
routing, callback, and teardown implementation:

```text
A. canonical helper
   raw ticket retained by the existing cell
   normalize and resolve the current site at each materialization
   advertised sidecar: 0 bytes

B. shared event
   immutable canonical event retained for the winning generation
   resolve the current site at each materialization
   advertised sidecar: 48 bytes

C. full fact
   immutable canonical event plus the initial resolved descriptor
   resolve only when a continuation changes site
   advertised sidecar: 64 bytes
```

The experiment must choose the smallest representation whose benefit is
stable. C is not selected merely because it is faster in a point estimate; it
must justify its extra 16 bytes and descriptor lifetime against B.

No Executor source, installed header, stable ABI, version metadata, or release
artifact changes in Phase 0.5.

## 2. Approaches considered

### 2.1 Extend only the full fact

This preserves the current implementation, but cannot determine whether the
gain comes from event normalization or initial descriptor retention. Rejected
because it cannot answer the Phase 0.5 question.

### 2.2 Maintain three independent models

This gives each representation a locally simple implementation. It also
duplicates state transitions, failure cleanup, queue ownership, and callback
logic. Semantic drift would become a larger signal than the representation
cost. Rejected.

### 2.3 One protocol with an orthogonal representation axis

Selected. The state machine and all mutable decisions remain shared. Only the
publication payload and materialization work differ. This isolates the two
questions:

1. Does one canonical event per generation beat repeated normalization?
2. Does retaining the initial descriptor beat resolving every consumed site?

## 3. Non-negotiable semantic contract

All representations use:

- generation-tagged ticket owners;
- the single atomic closed/in-flight lifecycle gate;
- `ARMED(g) -> BUILDING(g) -> READY(g)` publication;
- fresh mutable guards at every direct or queued admission;
- one callback transaction and abort path;
- `RUNNING(g) -> TERMINAL(g) -> ARMED(g+1)` reuse;
- identical module, payload, backend, callback, queue, and external reference
  accounting;
- the same intrusive overflow fallback;
- the same distinct eight-site function table;
- literal event, errno, command, and callback traces.

Module and payload ownership is deliberately identical in A, B, and C. This
experiment measures representation and materialization work, not a weaker
unload contract.

## 4. Representation records

### 4.1 Canonical helper

A publishes no persistent canonical payload. The winning raw ticket remains
owned by the cell. Each materialization calls the canonical normalization
helper and resolves the requested site into a transient 64-byte fact on the
consumer's stack.

Expected work for a measured window:

```text
normalizations = materializations
site lookups    = materializations
sidecar bytes   = 0
```

### 4.2 Shared event

B publishes the following immutable 48-byte record:

```c
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
```

Publication normalizes once but performs no site lookup. Materialization copies
the event into a transient fact, resolves the requested site, validates
pointer/index correspondence, and derives the transient fact ID.

Expected work:

```text
normalizations = completions
site lookups    = materializations
sidecar bytes   = 48
```

### 4.3 Full fact

C is the hardened Phase 0 representation. Publication normalizes the event
and resolves the initial site once. Materialization copies the fact. When a
callback changes `next_site`, only the transient copy is updated with a newly
resolved descriptor; the published fact remains immutable.

Expected work:

```text
normalizations = completions
site lookups    = completions + changed continuation sites
sidecar bytes   = 64
```

Every representation stores its advertised payload in a real, tightly
strided per-instance allocation. The larger test control object remains
constant across representations and is reported separately; this experiment
does not claim production Executor bytes per instance.

## 5. Internal API boundary

`experiments/lccf/lccf_representation.h` owns representation-specific storage
and conversion. It contains only standard integer/size includes and forward
declarations for the tagged fact structs, so `lccf_fact.h` can include it
without an include cycle. The implementation includes the complete fact
definitions. It exposes:

```c
typedef enum lccf_representation {
    LCCF_REP_CANONICAL_HELPER = 0,
    LCCF_REP_SHARED_EVENT,
    LCCF_REP_FULL_FACT,
    LCCF_REP_COUNT
} lccf_representation_t;

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

The fact state machine calls this boundary after winning the generation. It
does not branch separately in direct, queued, failure, or teardown paths.

Existing textual modes remain supported. New internal modes add shared-event
queue, fused, and mixed variants. Mode parsing maps each mode to two orthogonal
properties: route family and representation.

## 6. Long-lived paired benchmark

`bench_lccf_repr` creates independent long-lived batch pairs for these
contrasts:

```text
A / B  isolates persistent event normalization
A / C  measures the complete full-fact effect
B / C  isolates initial descriptor retention and the extra 16 bytes
```

Each pair uses the same seed, workload, frame size, instance count, chain
length, and eight distinct resume functions. A calibration loop doubles the
round count until both sides meet the minimum measured-window duration, then
resets both batches before evidence collection.

Each block executes either `ABBA` or `BAAB`. The order is selected from a
seeded generator before the block, retained in raw output, and balanced across
the run. A pair is recorded only after both sides have executed the same
cumulative number of rounds and their canonical checksums and routing/lifetime
metrics agree.

Defaults:

```text
sites               8 only
instances           257
chain length        8
minimum window      25 ms per side
blocks              4 per contrast and process
paired ratios       8 per contrast and process
process repetitions 5
frame sizes         64 and 256 bytes
workloads           all four Phase 0 workloads
route families      queue, fused, mixed
```

This produces 24 cells and 40 paired ratios per contrast per cell while using
120 fresh processes rather than 1,440 short-lived processes.

The C benchmark emits raw window records only. Classification and confidence
intervals belong to the Python evidence runner.

## 7. Statistical contract

Ratios are calculated within a process and block:

```text
wall speedup = left wall / right wall
CPU ratio    = right CPU / left CPU
p99 ratio    = right round-p99 / left round-p99
```

The gate uses one process median per cell and contrast so multiple windows in
one process are not treated as independent machines. With five process
repetitions, the runner pools 40 cell/process medians for each route family:
four workloads by two frame sizes by five processes. It computes a
deterministic percentile bootstrap 95% confidence interval for the median with
20,000 resamples. Bootstrap seeds are derived from the experiment seed and the
serialized route/contrast key.

Correctness is evaluated before timing. A result is `INCONCLUSIVE` when a
confidence interval crosses its declared boundary; the runner never converts
an inconclusive result to pass from the point estimate alone.

## 8. Eligibility and selection gates

Every raw row must satisfy:

- checksum and literal trace equivalence;
- identical completion, callback, command, routing, and reference counts;
- no hot allocation, build failure, generation mismatch, reuse delay, or task
  loss;
- exact representation-specific normalization and lookup counts;
- balanced queue, overflow, module, payload, backend, and ticket ownership;
- measured duration at least the declared minimum.

B and C are independently eligible against A only when all route-family gates
pass:

| Gate | Requirement on 95% CI |
|---|---:|
| queue wall speedup | lower bound >= 0.98 |
| fused wall speedup | lower bound >= 1 / 1.02 |
| mixed CPU ratio | upper bound <= 0.95 |
| every route p99 ratio | upper bound <= 1.05 |

Selection then follows:

1. If neither B nor C is eligible, retain A.
2. If only one is eligible, select it.
3. If both are eligible, C must also beat B with an overall CPU-ratio upper
   confidence bound at or below 0.98, while every B/C route has wall lower
   bound at least 0.98 and p99 upper bound at most 1.05.
4. Otherwise select B because it is smaller and has no retained descriptor.
5. Any correctness failure rejects the affected representation regardless of
   performance.

The runner may report `INCONCLUSIVE` instead of selecting when evidence is
insufficient. Thresholds are fixed here before measurements.

## 9. Test strategy

### Unit and differential tests

- literal 48-byte event layout and 0/48/64 sidecar sizes;
- publication/materialization count equations for A, B, and C;
- eight distinct site pointers and continuation-site correctness;
- published B/C records remain byte-identical after consumption;
- all existing failure, race, overflow, and generation tests run for all three
  representations;
- 72 existing Phase 0 differential cases remain unchanged;
- new A/B/C traces are identical for every workload and route family.

### Benchmark contract tests

- option rejection and minimum-duration calibration;
- ABBA and BAAB balancing;
- malformed, duplicate, missing, and non-finite raw fields;
- exact work-count rejection for each representation;
- process-median collapse before bootstrap;
- deterministic bootstrap fixtures with hand-checked decisions;
- boundary cases for eligible, ineligible, and inconclusive selection.

### Platform verification

- macOS arm64 Clang build, focused tests, full CTest, ASan/UBSan, and static
  analyzer;
- Linux arm64 GCC normal, ASan/UBSan, and TSan;
- Windows x86_64 MinGW compile and Wine execution;
- GitHub CI across the repository's existing Linux, macOS, BSD, Windows,
  sanitizer, stress, security, and documentation jobs.

Performance selection is made only from the declared primary host run. Other
platforms validate semantics and portability, not cross-machine timing.

## 10. File boundaries

Create:

```text
experiments/lccf/lccf_representation.h
experiments/lccf/lccf_representation.c
experiments/lccf/test_lccf_representation.c
experiments/lccf/bench_lccf_repr.c
scripts/bench_lccf_repr.py
scripts/test_bench_lccf_repr.py
docs/research/reports/2026-08-02-lccf-repr-phase0-5-results.md
docs/research/reports/2026-08-02-lccf-repr-phase0-5-decision.md
```

Modify only the standalone LCCF model, tests, and build registration required
to route the three representations. Files under `src/`, installed headers,
version metadata, and release configuration remain untouched.

New files use
`SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`.
Existing files retain their current license headers.

## 11. Stop conditions

Stop without Executor integration when:

- any representation changes canonical behavior or weakens lifetime safety;
- the benchmark cannot keep both sides above the minimum duration;
- bootstrap or ordering results are not reproducible from retained raw data;
- no persistent representation clears the eligibility gates;
- B and C cannot be distinguished reliably.

An eligible Phase 0.5 winner authorizes only a test-only Executor prototype.
It does not authorize a version change or release.
