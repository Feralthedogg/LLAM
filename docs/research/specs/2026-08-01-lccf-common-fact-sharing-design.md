# LCCF-CFS — Common-Fact Sharing for Causal Completion Fusion

Status: accepted extension design for isolated Phase 0 implementation  
Primary goal: normalize, claim, and pin a winning completion once, then share only immutable facts between direct and queued consumption paths

## 1. Problem statement

LCCF already proposes to replace:

```text
completion -> waker -> runnable queue -> scheduler -> resume
```

with a generation-checked owner-local continuation path when policy permits.

Without a common-fact layer, direct and fallback paths can separately repeat:

- backend result normalization;
- generation checks;
- module/instance resolution;
- event construction;
- resume-site lookup;
- payload ownership decisions;
- diagnostic classification.

CFS removes duplicated stable work while explicitly retaining consume-time rechecks for mutable scheduling policy.

## 2. Considered approaches

### A. Recompute everything in each path

Advantages:

- easy local reasoning;
- no shared internal record.

Disadvantages:

- duplicate instructions and reference traffic;
- direct and queued paths can normalize outcomes differently;
- more differential-test surface.

### B. One mutable cached decision object

Advantages:

- maximum apparent sharing.

Disadvantages:

- unsound when migration, stop, fairness, tracing, or module policy changes;
- difficult state ownership;
- stale direct decisions can survive queue delay.

Rejected.

### C. Immutable fact core plus mutable consume guard — selected

```text
completion fact:
  stable for one winning generation

consume guard:
  recomputed whenever and wherever the fact is consumed
```

## 3. Scope

### In scope

- Executor-plane I/O, timer, cancel, external wake, and runtime-stop events;
- canonical event normalization;
- one generation claim;
- module/site retention;
- direct and queued consumers;
- queue delay and migration;
- diagnostics and differential testing.

### Out of scope

- existing stackful task I/O paths;
- global completion memoization;
- sharing across instances or generations;
- caching fairness or home-shard decisions;
- public ABI exposure of internal fact records;
- arbitrary compiler common-subexpression elimination.

## 4. Data split

### 4.1 Stable fact core

```c
typedef struct llam_executor_fact_core {
    uint64_t generation;
    uint64_t fact_id;
    uint64_t stable_flags;

    uint32_t event_kind;
    uint32_t source_kind;
    uint32_t captured_home_shard;
    uint32_t source_node;

    int64_t result;
    int32_t error_code;
    uint32_t backend_flags;

    llam_executor_instance_t *instance;
    llam_executor_module_t *module;
    const llam_executor_site_t *site;

    llam_executor_event_t event;
} llam_executor_fact_core_t;
```

Stable facts include:

- winning generation;
- canonical result and errno;
- event kind and payload ownership;
- instance/module/site identity under retained references;
- source backend/node;
- captured home shard as history, not a current decision;
- immutable module/site capabilities.

### 4.2 Consume guard

```c
typedef struct llam_executor_consume_guard {
    uint64_t checked_at_ns;
    uint32_t current_home_shard;
    uint32_t mode;
    uint64_t escape_reasons;
    uint64_t policy_snapshot;
} llam_executor_consume_guard_t;
```

Recheck at each consumption:

- current home shard and migration;
- runtime stop/shutdown;
- module direct enablement;
- tracing/debug mode;
- fairness and causal budgets;
- timer/stackful pressure;
- callback-active state;
- shard online/pause state;
- opaque-blocking compensation;
- command/backend capability currently available.

The guard is not retained as truth after a queue hop.

## 5. Storage layout

CFS must not force the hot causal cell to grow without evidence.

Phase 0 compares:

### Layout A: split 64 + 64

```text
64-byte causal hot cell
64-byte fact/event sidecar
```

### Layout B: 96 + 64

Larger hot metadata with fewer sidecar loads.

### Layout C: 128-byte unified cell

Optimistic single-object locality at the cost of a larger footprint.

Recommended initial design: split hot cell and sidecar already owned by the Executor instance. No allocation occurs after instance creation.

## 6. Completion state machine

```text
ARMED(g)
  -> FACT_BUILDING(g)      winning completion only
  -> FACT_READY(g)
     -> RUNNING_DIRECT(g)
     -> QUEUED(g)
        -> RUNNING_QUEUED(g)
  -> ARMED(g+1)
  -> TERMINAL(g)
```

Losing timeout, cancellation, and backend tickets observe that `ARMED(g)` was already claimed and only retire their retained ownership.

`FACT_BUILDING` has one owner and is never consumed by another thread.

## 7. Publication protocol

### Step 1: decode ticket identity

The immutable ticket provides:

```text
cell/instance
captured generation
expected event kind
backend slot
owner runtime
source node/shard
retained backend reference
```

### Step 2: claim generation

```text
ARMED(g) -> FACT_BUILDING(g)
```

with acquire-release compare/exchange.

Failure means stale or losing completion. It must not build or publish a fact.

### Step 3: transfer lifetime ownership

The winning ticket transfers or retains references needed by the fact:

- instance;
- module/site code;
- event payload or owned buffer;
- backend result storage when required.

Module unregistration remains `EBUSY` while a fact or callback can reach module code.

### Step 4: normalize once

Platform adapters produce one canonical event:

```text
Linux CQE
kqueue event
IOCP completion
timer expiry
cancellation
external wake
runtime stop
  -> llam_executor_event_t
```

No direct or queued consumer sees raw backend conventions.

### Step 5: resolve site once

Resolve the site descriptor from the instance's next-site index and retained module descriptor. Validate:

- site index;
- callback presence;
- frame-layout identity;
- event compatibility;
- module generation/registration;
- immutable capability flags.

### Step 6: publish

Write the complete fact non-atomically under winner ownership, then publish:

```text
FACT_BUILDING(g) -> FACT_READY(g)
```

with release ordering.

Consumers acquire the state before reading fact fields.

## 8. Direct versus queued path

### 8.1 Direct candidate

On the home fused worker:

1. acquire `FACT_READY(g)`;
2. evaluate a fresh consume guard;
3. claim `RUNNING_DIRECT(g)`;
4. call the common `consume_fact()` routine;
5. validate the command;
6. arm next generation or terminate.

### 8.2 Queue fallback

If the guard rejects direct execution:

1. record exact escape reasons;
2. transition `FACT_READY(g) -> QUEUED(g)`;
3. publish the causal cell to the allocation-free escape queue;
4. queue consumer acquires the cell;
5. recompute consume guard;
6. claim `RUNNING_QUEUED(g)`;
7. call the same `consume_fact()` routine.

The queue path does not redo backend decode, winning-generation CAS, event normalization, or site lookup.

## 9. Common consume function

Conceptual interface:

```c
int llam_executor_consume_fact(
    llam_fused_shard_t *shard,
    llam_executor_cell_t *cell,
    const llam_executor_fact_core_t *fact,
    llam_executor_command_t *command);
```

Responsibilities:

- verify state/generation correspondence;
- initialize command storage to `FAIL/EPROTO`;
- invoke the retained site callback;
- convert callback panic/exception through the language adapter;
- validate command fields and reserved zeros;
- apply lane-local failure semantics;
- never retry a partially executed callback.

Direct and queued modes differ only in admission and guard evaluation.

## 10. Stable and unstable checks

### Hoisted once

- ticket generation and kind;
- winning claim;
- raw backend result decode;
- errno/result normalization;
- event construction;
- module/site descriptor lookup;
- immutable capability validation;
- payload ownership pin;
- fact ID and trace classification.

### Rechecked

- current cell state and generation at consume;
- runtime stop;
- home shard;
- migration;
- direct enablement;
- fairness and time budget;
- tracing/debug mode;
- shard pause/offline;
- callback-active bit;
- current backend command support.

This split is the central correctness rule.

## 11. Memory ordering

- winning CAS: `memory_order_acq_rel`;
- fact field writes: winner-local ordinary writes;
- fact-ready publication: release;
- direct/queue consumer state load: acquire;
- queue link publication: release/acquire;
- terminal or next-generation transition: release;
- fact storage reuse only after:
  - callback inactive;
  - previous generation no longer runnable/queued;
  - backend refs zero;
  - external refs zero;
  - payload refs returned.

No fact pointer is exposed beyond its instance lifetime.

## 12. Migration

A fact stores `captured_home_shard` for diagnostics only.

Before consuming:

- read current home shard;
- if the current worker is not the home worker, queue or forward the cell;
- if the shard is paused/offline/migrating, use the escape path;
- do not rebuild the fact;
- preserve FIFO and exactly-once state transitions.

Migration cannot invalidate module/site/event facts because their references are retained.

## 13. Stop and cancellation

The event that wins generation `g` is immutable.

A later runtime stop or cancellation does not rewrite that fact. Instead:

- it is observed by the consume guard or next callback boundary;
- the callback may receive a stop/cancel event in a subsequent generation where the lifecycle permits;
- bounded cleanup policy can force terminal teardown.

This prevents two sources from mutating one published event.

## 14. Errors

| Error | Result |
|---|---|
| stale/losing ticket | retire refs, increment stale metric |
| malformed backend result | publish lane-local `FAIL/EPROTO` |
| invalid site | publish `FAIL/EPROTO` |
| module unavailable despite retained contract | runtime corruption, fail instance |
| escape queue ring full | intrusive overflow queue, never drop |
| invalid callback command | fail instance; no retry |
| generation exhaustion | terminal retirement |
| payload pin failure before publication | fail event or conservative queue path |
| guard change after queue delay | recompute and route safely |

## 15. Metrics

```text
facts_attempted
facts_built
facts_build_failed
fact_stale_losers
fact_module_pins
fact_payload_pins
fact_direct_consumes
fact_queued_consumes
fact_guard_rechecks
fact_queue_forwards
fact_generation_mismatches
fact_reuse_delays
fact_bytes_hot
fact_bytes_sidecar
```

Escape reasons are individually counted:

```text
wrong_shard
budget
fairness
trace
stop
migration
module_policy
backend_capability
callback_active
queue_pressure
```

## 16. Phase 0 model

Extend the existing standalone LCCF experiment with paired modes:

```text
recompute_queue
shared_fact_queue
recompute_fused
shared_fact_fused
mixed_recompute
mixed_shared_fact
```

All modes use identical:

- tickets;
- frames;
- callback formulas;
- generation claims;
- event outcomes;
- command validation;
- canonical final state.

Only duplicated normalization/resolution work differs.

## 17. Tests

### Differential equivalence

For every workload, frame footprint, fact layout, site count, and direct/queued mixture:

- final frame state identical;
- command sequence identical;
- event payload/errno identical;
- checksum identical;
- module and payload refs balanced.

### Races

- I/O versus timeout;
- I/O versus cancellation;
- direct guard versus migration;
- direct guard versus stop;
- queue delay versus module disable;
- queue forwarding during worker offlining;
- fact generation reuse attempt;
- callback failure;
- escape queue overflow.

### Lifetime

- module unregister returns busy while any fact exists;
- frame drop occurs exactly once;
- payload returned exactly once;
- losing tickets cannot reuse visible storage;
- fact sidecar cannot be overwritten before all prior references retire.

### Platform normalization

Equivalent logical completions from Linux, kqueue, and IOCP adapters produce the same canonical event.

## 18. File boundaries

### Standalone phase

```text
experiments/lccf/
  lccf_fact.h
  lccf_fact.c
  lccf_fact_layout.c
  test_lccf_fact.c
  bench_lccf_fact.c

scripts/
  bench_lccf_fact.py
  test_bench_lccf_fact.py
```

### Executor prototype

```text
src/executor/completion_fact.c
src/executor/completion_consume.c
src/executor/completion_queue.c
src/executor/executor_types.h
src/executor/linux_fact.c
src/executor/kqueue_fact.c
src/executor/windows_fact.c
```

Do not modify existing stackful I/O completion code in the first Executor prototype.

## 19. Performance gates

CFS must pass correctness first, then:

- no hot allocation;
- queued path throughput at least 98% of recompute baseline;
- direct path does not regress more than 2%;
- mixed direct/queue workloads reduce instructions or process CPU by at least 5%;
- result normalization and site lookup counts fall to exactly one per winning generation;
- fact layout does not cause more than 5% p99 latency regression;
- spread of gate-driving paired ratios is within the existing research threshold.

Complexity gate:

> If common-fact sharing does not reduce measurable work, retain only a small canonical event-normalization helper and remove the persistent fact record.

## 20. Interaction with other tracks

- LRPA provides fact publication, double-consume, migration, and module-lifetime race gadgets.
- LSWG does not include Executor nodes until LCCF lifecycle fields are stable.
- LCRS does not steal Executor escape cells in the first implementation.
