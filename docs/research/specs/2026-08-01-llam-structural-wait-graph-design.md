# LSWG — LLAM Structural Wait Graph

Status: accepted for isolated Phase 0 implementation  
Plane: controller/watchdog and diagnostics  
Primary goal: distinguish a structurally closed deadlock, an orphan/lost wake, and an externally satisfiable wait without maintaining a graph on normal hot paths

## 1. Problem statement

The current watchdog observes a coarse progress snapshot and runtime-wide conditions such as runnable work, timers, I/O, blocking jobs, opaque blocking, and known external synchronization waiters. This is effective for detecting global quiescence but cannot explain the dependency structure or distinguish several classes of stall.

LSWG adds a second-stage structural analysis:

```text
coarse watchdog suspicion
  -> generation-pinned snapshot A
  -> AND/OR wait satisfiability analysis
  -> candidate structural verdict
  -> snapshot B and fingerprint confirmation
  -> report or fatal classification
```

It does not update a graph during ordinary mutex, channel, join, timer, or I/O operations.

## 2. Considered approaches

### A. Maintain an online wait-for graph

Advantages:

- immediate cycle detection;
- graph always available.

Disadvantages:

- writes and synchronization in every park/wake path;
- difficult lifetime and ABA handling;
- high risk of altering the races being diagnosed.

Rejected for the first implementation.

### B. Reconstruct from trace events

Advantages:

- low direct coupling to object internals;
- useful postmortem history.

Disadvantages:

- bounded trace rings can omit required events;
- reconstructing the current owner from history is ambiguous;
- stale/recycled objects need exact generations.

Useful as supporting evidence, not the primary truth source.

### C. On-demand pinned snapshot — selected

Advantages:

- zero normal-path graph maintenance;
- uses current task wait metadata and generations;
- can explicitly classify incomplete snapshots;
- suitable for human and machine-readable diagnostics.

## 3. Graph is AND/OR, not merely directed

A simple wait-for graph is insufficient because:

- channel select completes when **any** operation succeeds;
- cancellation and deadlines are alternative wake sources;
- a resource can have multiple possible providers;
- condition variables and channels may be signaled from unmanaged host code.

LSWG therefore models each parked task as a set of alternatives.

```text
task is blocked
  iff every wait alternative is blocked

task is escapable
  if at least one alternative is ready, externally open,
  or depends on another escapable task
```

Tarjan SCC is used only after a fixpoint removes all escapable nodes.

## 4. Identity model

### Task identity

```text
(task_id, wait_generation, public_handle_generation)
```

### Public resource identity

```text
(object_family, public_handle_slot, public_handle_generation)
```

### I/O and timer identity

```text
I/O: (request address, operation_generation, owner runtime)
timer: (task_id, wait_generation, deadline)
```

Raw addresses are printed for debugging but never define equality by themselves.

## 5. Snapshot safety

### 5.1 Task pinning

For each task:

1. Acquire its owner-shard task-list lock or the existing scan-safe list mechanism.
2. Confirm it is listed and increment a scan reference.
3. Copy atomic task state, wait reason, wait generation, shard IDs, deadline, and public generation.
4. Release the shard lock.
5. Enter the task's existing wait-resolver gate.
6. Copy the currently published owner pointers and owner-specific immutable/atomic fields.
7. Exit the resolver gate.
8. Release the task scan reference.

The snapshot never holds a shard lock while acquiring a primitive lock.

### 5.2 Primitive locking

- Use `pthread_mutex_trylock()` or the platform equivalent.
- Hold at most one runtime object lock at a time.
- Failure to acquire a lock marks the snapshot `INCOMPLETE_LOCK_BUSY`.
- Never wait for a runtime internal lock from the watchdog.
- Copy only fields protected by that lock or declared atomic/immutable.

### 5.3 Generation confirmation

Snapshot B must match snapshot A for every candidate dependency:

- task wait generation;
- object public generation;
- I/O operation generation;
- select completion generation/state;
- owner task identity;
- edge kind.

Any change discards the candidate verdict.

## 6. Node and edge schema

### 6.1 Node kinds

```c
typedef enum llam_swg_node_kind {
    LLAM_SWG_NODE_TASK,
    LLAM_SWG_NODE_MUTEX,
    LLAM_SWG_NODE_COND,
    LLAM_SWG_NODE_CHANNEL,
    LLAM_SWG_NODE_SELECT,
    LLAM_SWG_NODE_IO_REQ,
    LLAM_SWG_NODE_TIMER,
    LLAM_SWG_NODE_BLOCK_JOB,
    LLAM_SWG_NODE_CANCEL_TOKEN,
    LLAM_SWG_NODE_EXTERNAL_SOURCE,
    LLAM_SWG_NODE_BACKEND_SOURCE,
} llam_swg_node_kind_t;
```

### 6.2 Edge kinds

```text
TASK_WAITS_MUTEX
MUTEX_OWNED_BY_TASK
TASK_WAITS_JOIN
TASK_WAITS_CHANNEL_SEND
TASK_WAITS_CHANNEL_RECV
TASK_WAITS_SELECT
SELECT_ALTERNATIVE
CHANNEL_MATCHED_BY_TASK
TASK_WAITS_COND
TASK_WAITS_IO
IO_OWNED_BY_NODE
TASK_WAITS_TIMER
TASK_WAITS_BLOCK_JOB
TASK_CAN_CANCEL
RESOURCE_EXTERNAL_SIGNAL
```

Edges include flags:

```text
AND_REQUIRED
OR_ALTERNATIVE
EXTERNAL_OPEN
READY_NOW
INCONSISTENT
GENERATION_STABLE
```

## 7. Wait-kind semantics

### 7.1 Join

```text
waiting task -> join target task
```

- Target runnable/running or waiting on an open source makes the waiter escapable.
- Two or more tasks that recursively join one another can form a closed cycle.
- A terminal target with a persistent join waiter is a lost-wake or accounting anomaly.

### 7.2 Mutex

```text
waiting task -> mutex -> owner task
```

- Atomic mutex owner is copied.
- No owner while waiters persist across two snapshots is `ORPHAN_MUTEX_WAIT`.
- An owner outside the closed set that is runnable or externally escapable opens the dependency.
- A cycle of task/mutex ownership is a proven deadlock candidate.

### 7.3 Condition variable

Condition variables have no intrinsic owner and can be signaled by managed or unmanaged code.

Default **open-world mode**:

```text
task -> cond -> EXTERNAL_SIGNAL
```

It cannot be classified as a proven closed cycle.

Optional test/debug **closed-world mode** suppresses the external edge when the caller asserts that no unmanaged signaller exists. This mode is never the production default.

### 7.4 Channel send/receive

For a blocked send:

- free capacity or a live receiver means the operation should be ready;
- a live opposite waiter persisting across two snapshots is a `MATCHABLE_LOST_WAKE` candidate;
- otherwise the channel is externally open in default mode because host try-receive/close may act.

For a blocked receive:

- buffered data, closed state, or a live sender means it should be ready;
- persistent opposite waiters indicate a lost-wake anomaly;
- otherwise the channel is externally open by default.

Stale select nodes are filtered using task state, select completion state, and wait generations.

### 7.5 Channel select

Represent select as an OR-choice node:

```text
task -> select_choice
select_choice -> channel alternative 0
select_choice -> channel alternative 1
...
select_choice -> cancel alternative
select_choice -> deadline alternative
```

The task remains structurally blocked only if every alternative is closed and stable.

### 7.6 Timer/sleep

A future deadline is an open time source.

An expired deadline with a still-parked task across both snapshots is:

```text
OVERDUE_TIMER_LOST_WAKE
```

A timer node that no longer belongs to a shard heap while the task remains in sleep wait is an orphan.

### 7.7 I/O

An active request is open if one of these ownership states is valid:

- submit queue;
- inflight backend;
- poll/accept/recv watch;
- cancellation terminal event still holds a backend reference.

A task waiting on an I/O request that has no valid queue/watch/backend owner is:

```text
ORPHAN_IO_WAIT
```

Dynamic rehome is accepted if operation generation is unchanged and the owner fields form a valid transition.

### 7.8 Blocking job

Queued or running jobs are open through the blocking pool.

A task pointing to a finished/aborted job without a queued wake is a lost-wake candidate. A task with no valid job owner is an orphan.

### 7.9 Cancellation

An uncancelled token is an alternative external source in open-world mode. A canceled token with a task still parked across two snapshots is a cancellation lost-wake candidate.

## 8. Solver

### 8.1 Seed escapable nodes

Mark as escapable:

- runnable or running tasks;
- future timers;
- valid I/O backend ownership;
- valid blocking workers;
- open-world external signal nodes;
- ready channels;
- cancellation alternatives;
- dependencies leading to already terminal completion.

### 8.2 Fixpoint

Repeatedly mark:

- a resource escapable if one of its providers is escapable;
- a task escapable if any OR alternative is escapable;
- an AND dependency escapable only when the required provider is escapable.

Stop when no state changes.

### 8.3 Remaining blocked set

Classify the remaining subgraph:

- cycle exists: `PROVEN_CLOSED_CYCLE`;
- acyclic chain ends at a resource with no provider: `PROVEN_ORPHAN`;
- opposite channel waiters or ready resource persist: `MATCHABLE_LOST_WAKE`;
- graph contains unstable or unavailable observations: `INCOMPLETE`;
- no nodes remain: `OPEN_OR_PROGRESSABLE`.

Use Tarjan SCC on the remaining closed dependency graph to obtain the minimal explanatory cycle.

## 9. Two-snapshot confirmation

A structural fatal verdict requires:

1. unchanged coarse progress snapshot;
2. unchanged candidate task set;
3. unchanged wait/object/operation generations;
4. same normalized dependency fingerprint;
5. no new runnable, timer, I/O, blocking, or external-ready source;
6. candidate persisted for the configured confirmation interval.

The fingerprint is a deterministic hash over sorted:

```text
(node kind, stable identity, edge kind, target identity, flags)
```

## 10. Verdicts

```c
typedef enum llam_swg_verdict {
    LLAM_SWG_VERDICT_NONE,
    LLAM_SWG_VERDICT_OPEN,
    LLAM_SWG_VERDICT_PROGRESS_CHANGED,
    LLAM_SWG_VERDICT_INCOMPLETE,
    LLAM_SWG_VERDICT_PROVEN_CYCLE,
    LLAM_SWG_VERDICT_PROVEN_ORPHAN,
    LLAM_SWG_VERDICT_MATCHABLE_LOST_WAKE,
    LLAM_SWG_VERDICT_OVERDUE_SOURCE,
} llam_swg_verdict_t;
```

Suggested fatal mapping after observe/advisory phases:

| Verdict | Result |
|---|---|
| PROVEN_CYCLE | `EDEADLK` |
| PROVEN_ORPHAN | `EPROTO` or `EDEADLK`, with explicit report |
| MATCHABLE_LOST_WAKE | `EPROTO` |
| OVERDUE_SOURCE | retry once, then `EPROTO` |
| OPEN | do not fatal |
| INCOMPLETE/OOM | retry; after extended threshold retain conservative legacy behavior |

## 11. Workspace

The controller thread owns one reusable workspace:

```c
typedef struct llam_swg_workspace {
    llam_swg_node_t *nodes;
    llam_swg_edge_t *edges;
    uint32_t *hash_slots;
    uint32_t *tarjan_index;
    uint32_t *tarjan_lowlink;
    uint32_t *stack;
    uint8_t *flags;
    size_t node_capacity;
    size_t edge_capacity;
    uint64_t capture_seq;
} llam_swg_workspace_t;
```

Allocation occurs only on suspicion and is reused. OOM produces an explicit inconclusive verdict and a normal runtime dump.

## 12. Integration state machine

```text
IDLE
  -> COARSE_SUSPECT
  -> CAPTURE_A
  -> ANALYZE_A
  -> WAIT_CONFIRM
  -> CAPTURE_B
  -> CONFIRMED_REPORT
  -> IDLE

Any progress or generation change -> IDLE
Any incomplete capture -> RETRY_BACKOFF
```

The controller does not block scheduler shards for a global stop-the-world snapshot.

## 13. Reporting

### Human-readable dump

```text
wait_graph:
  verdict=proven_cycle capture=42 fingerprint=...
  task=17 state=PARKED wait=mutex generation=8
    -> mutex family=2 slot=4 generation=...
  mutex ...
    -> owner task=23
  task=23 state=PARKED wait=join generation=3
    -> task=17
```

### Machine-readable report

A size-prefixed internal structure includes:

- verdict and errno;
- capture IDs and timestamps;
- node/edge counts;
- minimal cycle;
- incomplete reasons;
- external/open-source counts;
- deterministic fingerprint.

Public exposure, if later needed, should be a separate diagnostics query rather than an enlargement of hot task APIs.

## 14. File boundaries

```text
experiments/lswg/
  lswg_graph.h
  lswg_solver.c
  lswg_synthetic.c
  test_lswg_solver.c

src/engine/watchdog/
  wait_graph_snapshot.c
  wait_graph_solver.c
  wait_graph_report.c
  watchdog_worker.c
  runtime_watchdog_internal.h

src/core/debug/
  debug_wait_graph.c
  debug.c
  debug_stats_json.c

tests/
  test_wait_graph_solver.c
  test_runtime_wait_graph.c
  test_runtime_wait_graph_races.c
```

## 15. Tests

### Proven closed structures

- two-task mutex cycle;
- three-task mutex cycle;
- join cycle;
- mixed mutex/join cycle;
- orphan mutex owner;
- orphan join target.

### Open structures that must not false-positive

- condition wait signaled by host;
- channel wait satisfied by host;
- future timer;
- live I/O;
- running blocking job;
- uncancelled token;
- owner task blocked on live I/O.

### Lost-wake/anomaly structures

- opposite channel waiters persist;
- expired timer remains parked;
- canceled token remains parked;
- finished blocking job remains parked;
- I/O request has no backend owner;
- terminal join target remains unsignaled.

### Snapshot races

- wake during capture;
- cancellation during capture;
- select completion during capture;
- task reclaim and object generation change;
- dynamic shard rehome;
- runtime shutdown;
- lock busy;
- workspace growth failure.

### Scale

- at least 100,000 synthetic nodes;
- bounded capture memory;
- deterministic report ordering;
- multi-runtime isolation.

## 16. Gates

- zero false proven-cycle verdicts across clean LRPA and soak profiles;
- every injected closed cycle is classified after confirmation;
- normal execution overhead below 0.5%;
- no per-wait graph allocation or edge update;
- 10,000-task graph analysis completes within the watchdog diagnostic budget selected by measurement;
- incomplete snapshots never become proven verdicts;
- open-world host-wake cases are never fatal;
- all reports reproduce the same fingerprint for the same stable state.

## 17. Interaction with other tracks

- LRPA amplifies snapshot/wake/cancel races and validates false-positive resistance.
- LCRS metrics do not count as progress and LCRS offlining races appear as dedicated graph tests.
- LCCF instances later require Executor-specific node kinds, but they are out of LSWG v1 until the Executor lifecycle is stable.
