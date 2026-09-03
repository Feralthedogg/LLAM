# Phase 0 Worker-Role and Signal-Stack Isolation Design

Date: 2026-09-04

Status: Approved for planning

Baseline: `79f1c03e9bd81a9a38233e935df698e86d7fd427`

## Summary

Phase 0 introduces explicit scheduler-thread identity and separates alternate
signal-stack state between the primary scheduler thread and the opaque-blocking
helper. It does not introduce general G-P-M scheduling, an execution lease, or
move scheduling authority out of `llam_shard_t`.

The work has two purposes:

1. Replace scheduler-context address comparisons that currently double as
   primary/helper role tokens with explicit legacy-role predicates.
2. Fix the unsafe sharing of one alternate signal-stack range and one
   install/restore record by two native threads.

The new worker and processor objects are scaffolding only. During Phase 0,
`llam_shard_t` remains authoritative for the current task, watchdog target,
thread lifecycle, queues, timers, allocator, online state, metrics, and opaque
handoff state.

## Motivation and Current Evidence

`llam_shard_t` currently combines native-thread state, scheduler contexts,
runnable state, timers, allocation caches, metrics, and opaque compensation.
That makes a direct M/P split unsafe.

The current code also uses scheduler-context address identity as an implicit
role check:

- `src/core/api/blocking_api.c` distinguishes primary and helper execution.
- `src/core/task/spawn.c` gates the local owner fast path and task-ID fast path.
- `src/core/task/yield_join_sleep.c` gates direct yield and join handoff.
- `src/core/wait/wait_tracking.c` gates wake handoff.
- `src/core/sched/reinject.c` gates direct owner reinjection.

Other references select the concrete context used for a context switch. Those
are context-routing operations, not role checks, and remain unchanged in Phase
0.

The primary scheduler and opaque helper both call
`llam_install_thread_signal_stack(shard)`. They therefore share:

- `shard->signal_stack`
- `shard->signal_stack_size`
- `shard->previous_sigaltstack`
- `shard->sigaltstack_installed`

`sigaltstack()` is thread-local, so sharing the backing range can make two
threads handle signals on the same memory. Sharing the saved previous stack and
installed flag can also cause one thread to suppress or corrupt the other's
restore operation.

The baseline `make test` suite passes on macOS arm64. No existing focused test
covers primary/helper signal-stack uniqueness or the full scheduler-role truth
matrix.

## Goals

- Add stable, runtime-lifetime worker slots for each shard's primary scheduler
  and opaque helper.
- Add an embedded processor identity cell for the future P boundary.
- Add TLS pointers for the current worker slot and associated processor.
- Make primary/helper role checks explicit while preserving their current truth
  table exactly.
- Give primary and helper schedulers disjoint alternate signal-stack ranges and
  independent save/restore state.
- Characterize repeated run, opaque helper, nested blocking, redirect fallback,
  and shutdown behavior before refactoring it further.
- Preserve the public C ABI, exported symbol set, stats/dump schema, task
  placement, fast-path eligibility, and error behavior except for removal of
  the unsafe signal-stack sharing.

## Non-Goals

Phase 0 does not:

- Add `processor->executor`, `worker->active_g`, an execution lease, or a lease
  epoch.
- Change `shard->current` publication or repair the known lost-current
  interleaving during primary/helper takeover.
- Remove or reinterpret `shard->preempt_thread`.
- Move `pthread_t`, scheduler contexts, queues, timers, allocator state, cache
  state, wake handles, I/O ownership, online state, metrics, or trace state.
- Add a general worker pool or arbitrary worker/processor handoff.
- Add asynchronous context capture, compiler metadata, or a new public API.
- Reinterpret a pinned task as native-thread affinity; shard identity continues
  to mean the future processor identity.

## Alternatives Considered

### Signal-stack fix only

Moving only the signal-stack fields is the smallest immediate safety patch, but
it leaves role encoded by context addresses and would require another pass over
the same scheduler entry and lifecycle paths. This is rejected because the
explicit identity scaffolding is small and makes the safety fix testable.

### Full execution lease in the first change

An execution lease is required before moving `current`, the watchdog target, or
P-local scheduling state. Adding it now would couple the signal-stack fix to the
opaque takeover protocol and task-reclamation rules. This is rejected for Phase
0 because it creates a much larger failure surface.

### Selected approach

Introduce descriptive worker/P identity, preserve all existing scheduling
authority, convert only role predicates, and migrate only signal-stack
authority. This is independently mergeable and leaves a clean boundary for the
legacy execution lease in Phase 1A.

## Internal Object Model

The exact field order is an implementation detail, but the logical model is:

```c
typedef enum llam_worker_role {
    LLAM_WORKER_ROLE_PRIMARY = 1,
    LLAM_WORKER_ROLE_OPAQUE_HELPER = 2,
} llam_worker_role_t;

struct llam_processor {
    llam_runtime_t *runtime;
    llam_shard_t *compat_shard;
    unsigned id;
};

struct llam_worker {
    llam_shard_t *compat_shard;
    llam_processor_t *associated_processor;
    llam_worker_role_t role;
    atomic_uint_fast64_t incarnation;

    void *signal_stack;
    size_t signal_stack_size;
    stack_t previous_sigaltstack;
    bool sigaltstack_installed;
    uint64_t signal_stack_incarnation;
};

struct llam_shard {
    /* Existing authoritative fields remain. */
    llam_processor_t processor;
    llam_worker_t primary_worker;
    llam_worker_t opaque_helper_worker;
};
```

The objects are embedded so their addresses are stable for the runtime's
lifetime and partial initialization does not add heap-object teardown paths.

### Authoritative Phase 0 worker fields

Only these worker fields are authoritative:

- immutable role
- stable association with the embedding shard and processor identity cell
- monotonically increasing incarnation used for lifecycle diagnostics
- signal-stack range, saved previous stack, installed state, and installed
  incarnation
- the current thread's `g_llam_tls_worker` binding

### Descriptive-only fields

The processor association and ID describe topology but do not prove execution
authority. Incarnation proves which binding installed a signal stack but does
not grant scheduling rights.

Phase 0 intentionally omits native-thread snapshots and bound-state mirrors.
The existing `thread`, `thread_started`, `primary_thread`,
`opaque_helper_thread`, and `preempt_thread` fields remain authoritative.

### Fields prohibited in Phase 0

The following must not be added:

- `worker->active_g`
- `worker->owns_processor`
- `worker->preempt_target`
- `processor->executor`
- `processor->current_g`
- `processor->lease_epoch`
- duplicate queues, timers, allocators, wake handles, or online state

## TLS Contract

Add:

```c
extern _Thread_local llam_worker_t *g_llam_tls_worker;
extern _Thread_local llam_processor_t *g_llam_tls_processor;
```

The five scheduler TLS values form one tuple:

- `g_llam_tls_shard`
- `g_llam_tls_worker`
- `g_llam_tls_processor`
- `g_llam_tls_task`
- `g_llam_tls_scheduler_ctx`

At scheduler entry, shard, worker, processor, and scheduler context are installed
before managed work can run. `g_llam_tls_task` remains null until dispatch. At
scheduler exit and every early-failure exit, all scheduler TLS values are
cleared.

The processor TLS means only "associated scheduling domain." Both a blocked
primary and an active helper may be associated with the same processor. It must
not be treated as proof that the thread currently executes that processor.

When runtime creation temporarily clears and later restores scheduler TLS for a
managed caller, it must save and restore the new worker and processor values
alongside the existing shard, task, and context values.

### Incarnation

- A primary worker's incarnation increments on every entry to its scheduler
  loop, including repeated runs driven by a different caller thread.
- The helper's incarnation increments when its native helper thread starts.
- Zero means no native-thread incarnation has entered the worker slot.
- Incarnation is diagnostic and signal-stack lifecycle state only.
- It must not select fast paths, route tasks, or choose a watchdog target.

## Explicit Legacy-Role Predicates

Role checks use narrowly named predicates:

```c
bool llam_tls_is_primary_scheduler_for(const llam_shard_t *shard);
bool llam_tls_is_opaque_helper_for(const llam_shard_t *shard);
bool llam_tls_has_legacy_direct_handoff_context(const llam_shard_t *shard);
```

The predicates require matching runtime, shard, embedded processor, and worker
slot. In assertion-enabled builds they also verify equivalence with the legacy
scheduler-context address.

They do not answer whether a worker owns or may execute a processor. Generic
names such as `worker_is_executor`, `worker_owns_processor`,
`can_execute_processor`, or `processor_current_worker` are forbidden until the
Phase 1A lease exists.

Production role comparisons are converted in:

- opaque blocking enter/leave
- local spawn and local task-ID allocation
- direct yield and join handoff
- wake handoff precheck
- direct reinjection

Context selection expressions that pass a concrete `llam_ctx_t *` into switch
functions remain unchanged.

## Signal-Stack Ownership

### Storage layout

Runtime initialization performs one mapping per shard sized for two alternate
stacks. The mapping is divided into non-overlapping primary and helper slices.
This preserves eager initialization and one mapping failure point while giving
each worker distinct stack memory.

The transitional shard may retain only the combined mapping base and total size
needed for one `munmap()`. Each worker is authoritative for its slice and all
thread-local install/restore state. The combined mapping is storage ownership,
not scheduler execution state.

The slice size is `max(LLAM_ALTSTACK_BYTES, SIGSTKSZ)`. Allocation arithmetic
must reject overflow before doubling it.

### Installation

`llam_install_thread_signal_stack()` accepts the current worker rather than a
shard. It:

1. Validates the worker, range, TLS binding, and nonzero incarnation.
2. Calls `sigaltstack()` using that worker's range.
3. Saves the previous stack in that worker.
4. Marks the worker installed for the current incarnation only after success.

The existing scheduler policy for installation errors is preserved. In
particular, a platform-reported `EAGAIN` remains nonfatal where the current code
already treats it as reduced diagnostic capability.

### Uninstallation and teardown

Uninstallation uses the same TLS worker and incarnation that installed the
stack. It disables the current thread's LLAM stack, restores that worker's saved
previous stack when enabled, and clears installed state.

The combined mapping is released only after:

- all primary scheduler incarnations for the run have exited and auxiliary
  primary threads have been joined,
- the opaque helper has been stopped and joined, and
- neither worker reports an installed signal stack.

Partial initialization may unmap a successfully allocated pair after checking
only initialized storage flags; it must not call thread-local uninstall from a
different thread.

## Lifecycle Sequences

### Primary scheduler entry

1. Select the embedded primary worker.
2. Increment its incarnation.
3. Install shard/worker/processor/context TLS as one logical operation.
4. Bind and tune the thread using the shard's existing CPU preference.
5. Install the primary worker's alternate signal stack.
6. Retain the existing `primary_thread` and `preempt_thread` authority and
   publish both before the first task dispatch. Phase 0 does not derive either
   value from the new worker shell.
7. Enter the existing scheduler loop.

If signal-stack installation is fatal, the loop records the same runtime fatal
error and clears the full TLS tuple before returning.

### Opaque helper entry

1. Select the embedded opaque-helper worker.
2. Increment its incarnation once for the newly started helper thread.
3. Install shard/worker/processor/helper-context TLS.
4. Bind and tune the helper.
5. Install the helper's distinct alternate signal stack.
6. Publish existing helper-ready/failed state and continue the existing helper
   protocol.

No helper execution-lease state is added.

### Exit and repeated run

Each scheduler thread uninstalls only its own stack and clears the full TLS
tuple. Re-entering the primary scheduler on a later `llam_run()` increments its
incarnation and captures that caller thread's previous alternate-stack state
afresh.

## Error Handling

- Combined signal-stack size overflow: runtime initialization fails with
  `ENOMEM` before mapping.
- Mapping failure: preserve the existing runtime-initialization failure path.
- Invalid worker/TLS/range on install: fail with `EINVAL` and let the existing
  scheduler fatal/nonfatal policy handle it.
- `sigaltstack()` failure: preserve `errno`; `EAGAIN` handling remains as it is
  today.
- Uninstall called through the wrong worker or incarnation is internal
  corruption. Assertion-enabled and production builds fail fast before
  restoring an unrelated thread's saved stack.
- Helper creation failure preserves the existing redirect fallback. Helper
  signal-install failure preserves the existing `opaque_helper_failed`
  publication and runtime-fatal recording; Phase 0 does not silently downgrade
  that failure to a new policy.
- Phase 0 does not claim to fix the existing `{helper active, preempt target,
  current G}` publication race.

## Testing Strategy

Add a dedicated internal test target,
`tests/test_worker_identity_internal.c`, linked against
`llam_runtime_testhooks`. Test-only observations are compiled only under
`LLAM_ENABLE_TEST_HOOKS` and are not exported from the public shared-library
ABI.

### Characterization tests

- Run one runtime twice and verify existing drain/re-run behavior.
- Drive shard 0 from different caller threads across successive runs.
- Trigger primary-to-helper takeover and helper-to-primary return.
- Exercise nested opaque enter/leave on the primary.
- Have a task dispatched by the helper enter opaque blocking and verify the
  existing redirect fallback rather than recursive helper creation.
- Destroy or shut down with the helper idle and active.
- Exercise helper-start failure and redirect fallback through a test-only fault
  injection point where portable.

### Identity tests

- Primary tasks observe the primary worker slot and matching processor/shard.
- Helper-dispatched tasks observe the helper slot and the same processor/shard.
- Worker slot addresses remain stable across repeated runs.
- Primary incarnation increases across repeated runs.
- Helper incarnation changes only when its native helper thread starts.
- All new TLS values are null after scheduler exit.
- Explicit legacy predicates reproduce the previous context-address truth
  matrix.
- Primary-only direct paths remain unavailable to the helper.

### Signal-stack tests

- Primary and helper ranges are non-null, large enough, and non-overlapping.
- `sigaltstack(NULL, ...)` on each scheduler thread reports that thread's worker
  range while installed.
- The previous alternate stack is restored independently on exit.
- A repeated primary run captures and restores the new caller's previous stack.
- Helper active/idle/failure shutdown does not leave an installed stack pointing
  into unmapped storage.
- Partial-init failure cleans the combined mapping once.

The known lost-current interleaving is not encoded as expected behavior.

## Verification Gates

Every implementation commit must build with `-Wall -Wextra -Wpedantic
-Werror`. The final Phase 0 gate is:

- `make test`
- the focused worker-identity test repeated under stress
- ASan/UBSan for the focused internal test and shutdown tests
- TSan where the platform/runtime combination yields actionable results
- CMake configure/build/CTest for the new target
- `make audit-production-test-hooks`
- public header byte hashes unchanged from the baseline
- exported symbol set unchanged from the baseline
- public stats/dump schema unchanged
- source audit finds no production role decision using raw primary/helper
  scheduler-context address comparisons
- source audit finds no executor/lease/current-G authority in the new shells

Windows builds must continue compiling the internal object model and tests that
are platform-neutral. POSIX-only `sigaltstack()` observations are conditionally
compiled and Windows native verification continues through the existing CI
path.

## Commit Structure

### Commit 1: Characterize legacy scheduler roles and helper lifecycle

Add the focused test target, test-only observations/fault injection, and tests
for repeated run, opaque takeover, nested blocking, redirect fallback, and
shutdown states. Production scheduling behavior remains unchanged.

### Commit 2: Add embedded identity shells and TLS

Add the worker/processor types and embedded slots, initialize stable topology,
bind/clear the new TLS values at primary/helper entry/exit, and preserve them
across nested runtime creation. Add tuple and incarnation tests.

### Commit 3: Replace implicit role tokens

Add the narrowly named legacy-role predicates and replace production role
comparisons. Keep context-routing expressions unchanged. Verify direct-path hit
and fallback behavior.

### Commit 4: Isolate per-worker signal stacks

Create the paired non-overlapping mapping, move install/restore authority into
each worker, update lifecycle teardown, and add focused signal-stack and failure
tests.

### Commit 5: Enforce the Phase 0 boundary

Add internal invariant/source audits, ABI/symbol/schema checks, remove obsolete
shard-level signal-stack fields, run the complete verification matrix, and
document the handoff to Phase 1A.

## Acceptance Criteria

Phase 0 is complete only when all of the following hold:

- Public headers, exported symbols, and public struct size/version contracts are
  unchanged.
- Existing stats, dump, trace, pinned-task, errno, and repeated-run semantics
  are unchanged.
- Primary and helper worker slots are stable and explicitly distinguishable.
- New TLS is internally consistent and fully cleared on every exit.
- Production role decisions no longer depend on raw scheduler-context address
  identity.
- Primary and helper never install overlapping alternate signal-stack ranges.
- Saved previous-stack and installed state cannot be overwritten by the other
  worker.
- The signal-stack mapping is freed only after both possible users have exited.
- Existing `current`, `preempt_thread`, opaque state, and all P-like resources
  remain solely authoritative in `llam_shard_t`.
- No API implies that worker/P association is execution ownership.
- Baseline and focused verification gates pass.

## Follow-On Boundaries

- Phase 0: preserve shard scheduling authority while adding explicit
  scheduler-role/TLS identity and per-worker signal-stack safety.
- Phase 1A: make primary/helper/redirect transitions an epoch-bearing legacy
  execution lease and make current-task and watchdog publication observe the
  same executor.
- Phase 1B: after the lease is stable, physically move runnable queues, timers,
  allocator/cache state, wake state, and logical I/O ownership from the shard
  compatibility cell into the processor.
