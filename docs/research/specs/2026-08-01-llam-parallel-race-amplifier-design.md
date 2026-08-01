# LRPA — LLAM Repetition-based Parallel Amplifier

Status: accepted for isolated Phase 0 implementation  
Plane: experiments and tests only  
Primary goal: turn rare concurrency races into reproducible, shrinkable, multi-lane experiments without assuming statistical independence

## 1. Principle

LRPA is inspired by parallel repetition as a testing methodology, not as a transferred theorem. Runtime race lanes share schedulers, caches, queues, and backends, so they are deliberately correlated.

The harness measures the empirical amplification curve rather than claiming:

```text
failure_probability(n) = failure_probability(1)^n
```

## 2. Considered approaches

### A. Increase loop count in existing tests

Advantages:

- minimal code;
- straightforward CI integration.

Disadvantages:

- repeats nearly the same topology;
- weak replay information;
- no coupling control or shrinking;
- can spend CPU without increasing schedule diversity.

### B. Run independent subprocesses

Advantages:

- clean isolation;
- easy timeout and crash attribution.

Disadvantages:

- does not stress shared runtime structures;
- misses cross-lane scheduler and object interactions.

### C. N-lane in-process composition plus fresh-process sampling — selected

- compose lanes inside one runtime to create shared contention;
- run each manifest in a fresh process for isolation;
- retain deterministic plans and shrink failures.

## 3. Architecture

```text
Python campaign runner
  -> fresh process
      -> LRPA manifest parser
      -> persistent host producer team
      -> runtime and preallocated lane resources
      -> lane barrier release
      -> deterministic perturbation plan
      -> oracles and trace
      -> one machine-readable result
  -> replay/shrink/report
```

## 4. Core objects

### 4.1 Manifest

```c
typedef struct lrpa_manifest {
    uint32_t version;
    uint32_t gadget;
    uint32_t coupling;
    uint32_t lane_count;
    uint32_t worker_count;
    uint32_t rounds;
    uint32_t flags;
    uint64_t seed;
    uint64_t timeout_ns;
    uint64_t perturbation_hash;
} lrpa_manifest_t;
```

A JSON representation additionally records:

- repository commit;
- compiler and flags;
- OS and architecture;
- runtime profile and experimental flags;
- CPU affinity;
- sanitizer;
- expected failure signature for fault-calibration runs.

### 4.2 Lane

```c
typedef struct lrpa_lane {
    uint32_t id;
    atomic_uint phase;
    atomic_uint winner_count;
    atomic_uint terminal_count;
    atomic_uint invariant_failures;
    uint64_t local_seed;
    uint64_t trace_checksum;
    void *gadget_state;
} lrpa_lane_t;
```

### 4.3 Coordinator state machine

```text
ALLOCATED
  -> SETUP
  -> ARMED
  -> RELEASED
  -> RACING
  -> DRAINING
  -> VERIFIED
  -> DESTROYED
```

No lane enters the race until every lane and persistent producer reports `ARMED`.

## 5. Coupling modes

```c
typedef enum lrpa_coupling {
    LRPA_COUPLING_INDEPENDENT,
    LRPA_COUPLING_SHARED_SHARD,
    LRPA_COUPLING_SHARED_OBJECT,
    LRPA_COUPLING_RING,
    LRPA_COUPLING_BIPARTITE,
    LRPA_COUPLING_COLORED_GRAPH,
    LRPA_COUPLING_MIXED_BACKEND,
} lrpa_coupling_t;
```

### Independent

Separate objects and states; only scheduler/backends are shared.

### Shared shard

Separate objects, but lane tasks are placed or pinned to the same scheduler domain.

### Shared object

Many lanes race on one channel, token, listener, task target, or Executor cell.

### Ring

Lane `i` triggers or cancels lane `i+1`, creating long causal chains.

### Bipartite

Producer and consumer lane sets stress fanout, select, and cancellation.

### Colored graph

A deterministic low-triangle edge-coloring assigns different race operations to lane pairs. This avoids reducing every campaign to one central hotspot.

### Mixed backend

Each lane combines timer, channel/select, I/O, and cancellation boundaries.

## 6. Gadget interface

```c
typedef struct lrpa_gadget_ops {
    const char *name;
    size_t state_size;
    int (*allocate)(lrpa_context_t *, lrpa_lane_t *);
    int (*arm)(lrpa_context_t *, lrpa_lane_t *);
    int (*trigger)(lrpa_context_t *, lrpa_lane_t *);
    int (*drain)(lrpa_context_t *, lrpa_lane_t *);
    int (*verify)(lrpa_context_t *, lrpa_lane_t *,
                  lrpa_failure_t *);
    void (*destroy)(lrpa_context_t *, lrpa_lane_t *);
} lrpa_gadget_ops_t;
```

All measured resources are allocated before `ARMED`.

## 7. Initial gadget library

### 7.1 Select completion race

Competing actions:

- send;
- close;
- cancel;
- timeout;
- cleanup of non-winning select nodes.

Oracle:

- exactly one selected/terminal outcome;
- no stale payload;
- no leftover live wait nodes;
- winner and error code are from the allowed outcome set.

### 7.2 Wait-generation race

Competing actions:

- timeout callback from generation `g`;
- wake of generation `g`;
- clear and re-arm generation `g+1`;
- stale callback arrival.

Oracle:

- generation `g` cannot complete `g+1`;
- exactly one wake publication per generation;
- recyclable storage is not reused early.

### 7.3 I/O cancel/complete/stop race

Competing actions:

- backend completion;
- cancellation;
- timeout;
- runtime stop;
- dynamic owner rehome.

Oracle:

- one winning event;
- balanced lifetime/backend references;
- no task parked after terminal request ownership;
- operation generation never aliases.

### 7.4 Join/detach/reclaim race

Competing actions:

- task exit;
- join claim;
- detach;
- group ownership;
- public handle resolve;
- reclaim.

Oracle:

- exactly one consumption of join/detach rights;
- object storage not reclaimed while pinned;
- terminal task observed consistently.

### 7.5 Dynamic merge/steal race

Competing actions:

- steal selection;
- lock-free normal queue steal;
- global steal pause;
- merge pause;
- worker offlining;
- task rehome.

Oracle:

- no pinned task migration;
- no task lost or duplicated;
- no enqueue to non-accepting shard;
- migration accounting matches queue movement.

### 7.6 LSWG capture race

Competing actions:

- snapshot pin;
- wake/cancel;
- object generation change;
- task reclaim;
- dynamic rehome.

Oracle:

- unstable snapshot is inconclusive, never proven;
- confirmed fingerprints contain only stable generations.

### 7.7 LCCF fact race

Competing actions:

- I/O completion;
- timeout/cancel loser;
- direct guard decision;
- queue fallback;
- module disable/unregister;
- migration;
- fact reuse.

Oracle:

- one fact builder;
- direct and queued consumption cannot both execute;
- fact generation and retained refs remain valid;
- canonical output equals scalar reference.

## 8. Perturbation plan

A seed produces a fixed list of actions:

```c
typedef enum lrpa_step_kind {
    LRPA_STEP_YIELD,
    LRPA_STEP_SPIN,
    LRPA_STEP_BARRIER,
    LRPA_STEP_TRIGGER,
    LRPA_STEP_CANCEL,
    LRPA_STEP_CLOSE,
    LRPA_STEP_TIMER_OFFSET,
    LRPA_STEP_HOST_WAKE,
    LRPA_STEP_REQUEST_STOP,
    LRPA_STEP_AFFINITY_ROTATE,
} lrpa_step_kind_t;
```

Guidelines:

- prefer logical barriers/yields for deterministic coverage;
- use precise timer offsets only for kernel races;
- persistent host threads perform unmanaged actions;
- thread creation and teardown are outside the race window;
- every step has a lane mask and deterministic sequence number.

## 9. Trace and failure signature

Each lane has a bounded trace ring:

```text
sequence
timestamp or logical clock
lane
actor
event kind
object identity
generation
state before/after
result
```

The failure signature is a stable hash over:

```text
gadget
oracle identifier
normalized expected/actual state
top relevant generations
coupling mode
```

Raw timing and addresses are excluded from the signature.

## 10. Oracles

Cross-gadget invariants include:

```text
armed waits
  = winning completions
  + winning cancellations
  + winning timeouts
  + explicitly abandoned setup failures

task starts = task terminals
successful sends = receives + buffered remainder
published wake generations are unique
backend refs and active refs return to baseline
no hot allocation during a measured race
```

An outcome can be nondeterministic but must belong to an explicitly enumerated allowed set.

## 11. Replay

Every failure writes a manifest and trace bundle:

```text
object/lrpa/failures/<signature>/<seed>/
  manifest.json
  result.json
  trace.bin
  trace.txt
  runtime_dump.txt
  wait_graph.json        # when LSWG is enabled
```

Replay command concept:

```text
test_lrpa --replay manifest.json
```

Replay requires the same semantic failure signature, not identical nanosecond timing.

## 12. Shrinking

The Python shrinker preserves a selected failure signature while attempting:

1. reduce lane count by binary search;
2. remove perturbation steps with delta debugging;
3. reduce rounds;
4. simplify coupling:
   - colored graph -> ring -> shared object -> independent;
5. reduce worker count;
6. remove optional timeout/cancel sources;
7. minimize payload and queue capacities.

Every accepted reduction is written to a new immutable manifest.

## 13. Fault calibration

Fault points are compiled only into dedicated calibration targets:

```c
LLAM_FAULT_POINT(SELECT_SKIP_WINNER_CAS)
LLAM_FAULT_POINT(WAIT_REUSE_GENERATION)
LLAM_FAULT_POINT(IO_DROP_BACKEND_REF)
LLAM_FAULT_POINT(MERGE_SKIP_OWNER_UPDATE)
LLAM_FAULT_POINT(LCRS_ACCEPT_OFFLINE_VICTIM)
LLAM_FAULT_POINT(LSWG_ACCEPT_UNSTABLE_SNAPSHOT)
LLAM_FAULT_POINT(LCCF_DOUBLE_FACT_CONSUME)
LLAM_FAULT_POINT(LCCF_SKIP_MODULE_PIN)
```

Release and ordinary test binaries contain no active fault hooks.

Calibration measures:

- trials to first detection;
- smallest lane count that reliably detects;
- replay rate;
- shrink rate;
- unique signature stability.

## 14. Empirical amplification metrics

For lane count `n`, report:

```text
process failure rate p_n
median trials to first failure
unique failure signatures
mean and p99 execution time
replay success rate
minimum shrunk lane count
```

When `0 < p_1 < 1`, optional hazard metrics are:

```text
h_n = -ln(1 - p_n)
efficiency_n = h_n / (n * h_1)
```

These are descriptive only; no independence assumption is made.

## 15. Process runner

The Python campaign runner:

- launches a fresh process per sample;
- alternates configuration order where comparing modes;
- enforces timeout and descendant cleanup;
- retains partial logs;
- never discards outliers;
- emits CSV, JSON, and Markdown summaries;
- records CPU and affinity metadata.

Use the repository's existing timeout-wrapper approach for hung races.

## 16. CI profiles

### PR

- deterministic gadgets;
- lane counts 1, 2, 4, 8;
- small seed set;
- no long real-I/O campaign;
- bounded runtime.

### Nightly

- all gadgets;
- lane counts through 32;
- hundreds of fresh-process seeds;
- ASan/UBSan;
- platform I/O races;
- fault-calibration subset.

### Weekly soak

- lane counts through 64 when host capacity allows;
- thousands of seeds or fixed-duration campaign;
- dynamic workers and multi-runtime;
- replay check on sampled successes and all failures;
- TSan diagnostic profile where supported.

## 17. File map

```text
experiments/lrpa/
  lrpa.h
  lrpa_internal.h
  lrpa_core.c
  lrpa_platform.c
  lrpa_trace.c
  lrpa_oracle.c
  lrpa_gadget_select.c
  lrpa_gadget_wait.c
  lrpa_gadget_io.c
  lrpa_gadget_lifecycle.c
  lrpa_gadget_scheduler.c
  lrpa_gadget_lccf.c
  test_lrpa_core.c
  test_lrpa_faults.c
  bench_lrpa.c

scripts/
  run_lrpa.py
  shrink_lrpa.py
  test_run_lrpa.py
  test_shrink_lrpa.py
```

Initial integration stays separate from `test_runtime_fuzz.c`; proven gadgets may later be shared through small helper modules.

## 18. Gates

- zero false positives over at least one million clean lane executions across required platforms;
- every supported injected fault is detected by its assigned PR, nightly, or soak profile;
- replay reproduces at least 99% of deterministic/core failures;
- shrinker preserves the signature and reduces at least 90% of calibration cases;
- no unreaped child process or contaminated later CI job;
- all result rows pass schema and accounting validation;
- adding lanes materially improves detection for at least the rare-race calibration set.

Rejection gate:

> If N-lane composition does not improve detection over equal-cost ordinary seed repetition, retain only the replay/oracle library and remove the amplifier scheduler.
