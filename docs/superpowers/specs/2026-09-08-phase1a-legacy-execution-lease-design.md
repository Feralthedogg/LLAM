# Phase 1A Legacy Execution Lease and Publication Safety

## Status

This specification implements the next architectural increment on top of the
approved Phase 0 worker-role and signal-stack boundary.  The Pro review marked
the boundary `CHANGES`: the implementation must define ownerless-state return
rights, helper handback completion, native-thread lifetime validation, and
pause-acknowledgement generations before it is considered mergeable.

## Goal

Make the existing primary/helper/redirect handoff an explicit, epoch-bearing
legacy execution lease while preserving the public C ABI, cooperative behavior,
statistics schema, and all Phase 0 ownership decisions.

## Scope

Phase 1A changes only the internal execution-publication boundary:

1. A shard has one lease coordinator that is authoritative for the current
   legacy execution holder, lease epoch, transfer state, and return claimant.
2. Primary, opaque helper, redirect, ownerless, and stopping transitions are
   serialized without adding a general worker pool or arbitrary handoff.
3. A worker's active task and run timestamps become worker-local, with task
   lifetime protected while watchdog/debug paths sample them.
4. Watchdog target selection is derived from the validated lease owner and its
   live native-thread binding; the mutable `preempt_thread` field is removed as
   an independent authority.
5. Pause acknowledgements are tied to request generations and lease epochs.
6. Existing helper blocking, helper re-blocking, redirect fallback, repeated
   `llam_run()`, dynamic shard scaling, shutdown, and fatal-error semantics are
   retained.

## Explicit non-goals

Phase 1A does not move runnable queues, timers, allocator/cache, wake handles,
or logical I/O ownership into the processor.  It does not add foreign detach,
async context capture, compiler metadata, Windows/BSD async preemption, or a
new public execution-contract API.  Rust, Python, JVM, .NET, and Go wrappers
are not part of this work or of the later Phase 6 scope.

## Authority model

`llam_shard_t` remains the composition root and continues to own queues,
timers, allocator/cache, I/O routing, online state, public metrics, and task
registry state.  The following internal state is authoritative after the
corresponding migration commit:

| State | Authority | Compatibility rule |
| --- | --- | --- |
| Lease holder, epoch, transfer state, return claimant | One embedded lease coordinator on the shard, protected by the existing opaque/handoff mutex | Helper-active and redirect booleans are projections only; grant decisions never read them as ownership truth |
| Task executing on an M, including a foreign-blocked task | `llam_worker_t.active_g` and worker-local timestamps | The old shard `current` is removed as a writable authority once the active-G migration lands |
| Runnable queues, timers, allocator/cache, I/O, online/pause policy | Existing shard fields | P-field migration is Phase 1B |
| Native thread creation/join compatibility | Existing shard lifecycle fields plus a worker binding/liveness record used by the lease | Join never uses a stale worker snapshot; target validity is checked under the lease lock |
| Watchdog preemption target | Current lease owner's live worker binding | `preempt_thread` is removed after all readers are migrated |
| Public stats, trace, ABI | Existing public aggregation and headers | No public struct, symbol, or stat-schema change |

The worker and processor shells introduced in Phase 0 remain descriptive for
association and role.  Phase 1A adds only the M-local execution and binding
fields needed to make the lease and watchdog truthful; it does not create a
second processor owner, a generic `is_executor()` predicate, or a copied P
resource set.

## Lease state machine

The coordinator uses the following states:

```text
PRIMARY_OWNED
  -> TRANSFER_TO_HELPER -> HELPER_OWNED
  -> TRANSFER_TO_PRIMARY -> PRIMARY_OWNED

PRIMARY_OWNED or HELPER_OWNED
  -> REDIRECTED_NO_EXECUTOR
  -> the recorded return claimant completes its foreign return
  -> the claimant reacquires through a new epoch

Any state -> STOPPING
```

The coordinator stores:

- current owner worker pointer (nullable only where the state permits it),
- owner worker incarnation and lease epoch,
- transfer sequence and completion acknowledgement,
- return claimant worker plus its incarnation for ownerless redirect episodes,
- state and stop/failure publication needed by the existing helper wake path.

`owner == NULL` is legal only for `REDIRECTED_NO_EXECUTOR` and shutdown
transitions.  A primary that merely observes `owner == NULL` may not claim the
lease.  In particular, when the primary is foreign-blocked, the helper becomes
foreign-blocked, and redirect is active, the helper's return token remains the
only claimant.  The primary waits until that token's incarnation completes the
legacy return boundary; otherwise two returning tasks could both resume the
same P.

`opaque_helper_active`, `opaque_helper_failed`, redirect depth, and helper
readiness remain available for compatibility, diagnostics, and wake policy.
They are updated under the coordinator lock and are never used as an
independent proof of lease ownership.

## Locking and publication rules

The existing `shard->opaque_lock` is the handoff/lease lock for this phase; no
second mutex is introduced.  The established order when both locks are
needed is:

```text
opaque_lock (handoff/lease) -> shard->lock (P/task state)
```

All `_locked` helpers assume their documented lock and never reacquire it.
Watchdog, merge, and debug paths must not acquire `opaque_lock` after holding
`shard->lock`; they either observe under the order above or perform separate
non-nested snapshots.  No lock may be held while waiting on a condition,
joining a thread, switching context, or running user/foreign code.

### Transfer to helper

1. Under the handoff lock, mark `TRANSFER_TO_HELPER` and reserve a transfer
   sequence; do not publish the helper as owner yet.
2. Under the shard lock, finish the primary's current-task transition: mark
   the task `BLOCKED_OPAQUE`, clear the primary active-G publication, and finish
   the owner-local accounting needed for quiescence.
3. Under the handoff lock, verify the primary incarnation and publish the
   helper owner, increment the lease epoch, record the helper incarnation, and
   set `HELPER_OWNED` with a completion sequence.
4. Wake the helper and release locks.  Only after this point may the helper
   consume the shard queues.

The helper's dispatch publishes task state, timestamps, and worker active-G in
the order required by the watchdog snapshot.  A late primary clear cannot
erase the helper's active task because the primary publication was retired
before the grant.

### Handback to primary

When compensation depth reaches zero or shutdown requests helper stop, the
helper stops fetching new work, completes its current task boundary, clears
its active-G under `shard->lock`, and only then publishes a handback
acknowledgement under `opaque_lock` containing its worker incarnation and
transfer sequence.  `opaque_compensation_depth == 0` is only a request to
hand back, never proof that handback is complete.  The primary may reacquire
only when the recorded return claimant and incarnation match the completed
acknowledgement and a fresh lease epoch is published.

### Ownerless redirect

Redirect keeps a return claimant and episode sequence even while no worker
holds the lease.  Redirected runnable tasks remain governed by existing shard
queue locks and overflow rules.  No arbitrary worker may claim the lease from
an ownerless observation.  The helper's nested `llam_enter_blocking()` path
uses this state rather than attempting to create a second helper.

## Current-G and watchdog publication

The active-G migration adds an atomic worker-local task pointer and the
worker-local run/safepoint/idle timestamps needed by current readers.  The
dispatch protocol is:

1. set task state, shard association, and timestamps;
2. publish the worker active-G with release semantics;
3. perform the context switch.

The return protocol is:

1. finish metrics and task-state publication;
2. clear worker active-G under the shard/reclaim barrier;
3. release task scan references only after the clear is visible.

The legacy shard `current` is not maintained as a second writable pointer.
During the migration commits, any compatibility observation is one-way and
must be derived from the worker active-G; no code may write both authorities.

The watchdog takes the handoff lock, snapshots owner worker, incarnation,
lease epoch, native-thread-live state, and target handle, then takes the shard
lock to acquire the active task and a task scan reference.  It revalidates the
owner and epoch before setting the existing advisory preemption request and
calls `pthread_kill()` while the target lifetime remains protected.  A stale
signal is ignored by the existing cooperative request path; Phase 1A does not
claim exact task delivery or asynchronous context capture.

## Native-thread lifetime and pause generations

Worker binding publishes the actual native handle, live/ready state, and
incarnation before the lease can be granted.  Unbind clears target eligibility
before the handle can be reused.  Helper exit may restore the primary target
only after validating the current primary binding and incarnation; an
unconditional restore is forbidden.

Merge/steal pause acknowledgements carry a request generation (and, where a
lease is active, the lease epoch).  While a merge pause is requested, lease
transfer and queue mutation are mutually exclusive.  A controller accepts an
ack only when the request generation, lease epoch, and worker incarnation it
observed still match.  Waiting occurs without either scheduler lock; final
state validation occurs under the documented lock order.  Existing shard-ID
ordering for two-shard merge locks is preserved.

## Error and shutdown behavior

Helper creation failure, helper signal-stack failure, redirect fallback,
runtime fatal recording, cancellation, and shutdown retain their existing
errno and wake semantics.  A failed transfer rolls back its reserved
sequence while keeping the old owner or redirect state authoritative.  A
shutdown transition first prevents new grants, requests helper stop, waits for
handback/exit acknowledgement without holding scheduler locks, and only then
joins or frees worker resources.  No ownerless state may be reclaimed merely
because `owner == NULL` or `shutdown_requested` is set; foreign-blocked tasks
and their return claimant remain live until the existing task/reclaim barriers
complete.

## Verification gates

Every commit must pass the existing full suite plus the focused internal
identity/lease tests available at that commit.  The focused matrix covers:

- primary-to-helper and helper-to-primary transfer ordering;
- ownerless redirect with a retained helper return claimant;
- helper re-blocking without spawning a second helper;
- handback completion versus compensation-depth timing;
- watchdog sampling during transfer, task exit, reclaim, and late signal;
- stale native-thread incarnation and helper-exit target restoration;
- pause request/ack generation reuse and pause-by-handoff exclusion;
- repeated shard-0 runs on different caller threads;
- dynamic merge/offline with active, idle, failed, and stopping helpers;
- shutdown and partial-init unwind at every transfer state;
- public stats/dump golden output, public-header byte identity, and exported
  symbol identity;
- debug, release `-DNDEBUG`, CMake/CTest, ASan/UBSan, TSan, and boundary
  source audits where supported by the host.

Native Windows/BSD execution remains an environment limitation on the current
Darwin host; platform-neutral compilation and registration checks still run.

## Later boundaries

Phase 1B physically moves P-owned queues, timers, allocator/cache, wake state,
and logical I/O routing after the lease and watchdog invariants are stable.
Phase 2 adds explicit foreign detach/reattach and relief-worker behavior.
Async context capture, compiler metadata, platform-specific asynchronous
backends, capability flags, observability expansion, and completion ABI are
separate later increments; language wrappers remain explicitly out of scope.
