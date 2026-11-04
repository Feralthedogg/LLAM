# Worker identity and leases

How LLAM separates worker identity from scheduling authority, and how the
execution lease moves between workers. These boundaries are internal and can
change.

## Worker identity

Phase 0 kept the shard as the scheduling authority. Phase 1A adds the single
legacy execution lease and makes the validated lease owner's worker-local
`active_g` plus run/safepoint/idle timestamps authoritative for execution
publication. The P owns its allocator and processor-local stack cache. The
shard still owns I/O-node backend state, public metrics, task registries, and
trace state; worker cells do not copy those resources.
Watchdog targets are derived from the validated lease owner's live worker
binding. The
primary worker and opaque helper worker each receive a distinct slice of the
shard's combined alternate-signal-stack mapping; each owning thread installs,
restores, and clears only its own slice. Phase 1B moved the immutable
CPU/locality/I/O-route tuple, online state, ordinary scheduler wake pair,
runnable queues, and active timer heap into the stable processor cell. The wake
descriptor remains open across online/offline transitions and closes only
after runtime workers stop. The processor owner projection is generation-checked
under the existing `shard->lock`, the first P payload lock, and transactionally
composed with lease changes under `opaque_lock -> shard->lock`. The Phase 1A
lease remains the sole execution-publication authority; a P ticket is a
non-owning snapshot that must be revalidated at each operation boundary.
Per-P allocator and stack-cache state now follow the stable processor cell;
allocation-owner IDs remain separate from task home/live placement, and the
runtime-wide stack fallback remains runtime-owned. Logical I/O route accessors
resolve through P while attached requests continue to use their node-owned
completion route. See the [Phase 1B
processor-resource contract](https://github.com/Feralthedogg/LLAM/blob/133723562cd0f441678f1f422eef667f4b914950/docs/superpowers/specs/2026-09-09-phase1b-processor-resources.md)
for the full state table, ticket, lock, and lifetime rules.

## Lease transitions

The `test_legacy_lease_internal` target observes the Phase 1A coordinator only
through declarations guarded by `LLAM_ENABLE_TEST_HOOKS`. Its snapshot copies
the lock-protected lease record; it does not derive ownership from the legacy
`current` or helper/redirect flags. The transfer-request, owner-clear, grant,
and handback gates are called only after releasing scheduler locks, so focused
tests can inspect each publication window without changing production
behavior. The watchdog gate covers the signal boundary after its task scan
reference is held; a stale lease/binding skips the advisory signal.

The Phase 1A execution-publication inventory is intentionally worker-local:
`scheduler.c`, `blocking_api.c`, `yield_join_sleep.c`, `reinject.c`,
`wait_tracking.c`, and task-exit paths publish or clear `worker->active_g` only
after state and timestamp preparation. `core_queue.c`, watchdog merge/probe/
scale paths, and `debug.c` inspect the validated lease owner's active-G through
`llam_shard_load_lease_current` while holding `opaque_lock` before
`shard->lock`; no shard-level current field remains.

* Watchdog target: `scheduler.c`
  (`llam_legacy_lease_snapshot_watchdog_locked`) snapshots the validated lease
  owner, incarnation, epoch, and native binding under `opaque_lock`;
  `watchdog_probe.c` acquires a task scan reference, revalidates that tuple, and
  sends only the existing advisory `pthread_kill` request. There is no mutable
  shard-side target field.
* `opaque_helper_active`: `timer.c` (`llam_ensure_opaque_helper_locked`) resets
  the projection under `opaque_lock` before helper creation; `scheduler.c`
  (`llam_opaque_helper_main`) writes helper readiness/activation and exit
  projections; `blocking_api.c`
  (`llam_enter_blocking`, `llam_leave_blocking`) reads the projection while
  waiting for helper activation/return; `watchdog_probe.c`
  (`llam_runtime_has_opaque_blocking`) reads it under `opaque_lock`; `debug.c`
  (`llam_dump_runtime_state`) reports it; and `timer.c`'s test-hook snapshot
  reads it under `opaque_lock`.
* `opaque_compensation_depth`: `blocking_api.c` (`llam_enter_blocking`,
  `llam_leave_blocking`) increments/decrements it and maintains its peak;
  `scheduler.c` (`llam_opaque_helper_main`) reads it to decide whether the
  helper should fetch or hand back; `watchdog_probe.c`
  (`llam_runtime_has_opaque_blocking`) reads it under `opaque_lock`; and
  `debug.c` (`llam_dump_runtime_state`) reports both depth values.
* Pause acknowledgements: `init.c` initializes `merge_pause_ack` and
  `steal_pause_ack` plus their request/ACK sequence, lease-epoch, and worker
  incarnation fields; `watchdog_merge.c` (`llam_runtime_set_steal_pause`,
  `llam_runtime_wait_steal_pause_ack`, `llam_shard_request_merge_pause`,
  `llam_shard_release_merge_pause`, `llam_shard_wait_merge_pause_ack`, and
  `llam_merge_lockfree_normq_locked`) clears, waits on, or validates them;
  `scheduler.c` (`llam_shard_pause_for_merge`) publishes and clears both
  acknowledgements while pausing; and `watchdog_scale.c`
  (`llam_runtime_offline_one_shard`) consumes the pause-wait results. The
  acknowledgement fields remain atomic and are versioned by the pause request
  sequence, lease epoch, and acknowledged worker incarnation. A boolean ACK is
  valid only when all three values still match the live lease owner under
  `opaque_lock`.

Phase 1A completed the migration of transfers, current-G publication, watchdog
targeting, and pause generations into the shard-owned legacy lease boundary
without introducing a second authority. Phase 1B has moved processor topology
and the ordinary scheduler wake pair while keeping the legacy lease as the
execution authority. Public headers, public statistics, and the exported ABI
remain unchanged. Queue, timer, allocator, processor-local stack-cache, and
logical I/O-route ownership now follow the processor-cell authority. Phase 2
adds generation-checked M/G/P foreign detach/reattach through the existing
helper/redirect handoff and gates relief and watchdog decisions on safe worker
state plus a live P ticket. See [Phase 2 Foreign Detach and Fail-Closed Relief
M](https://github.com/Feralthedogg/LLAM/blob/133723562cd0f441678f1f422eef667f4b914950/docs/superpowers/specs/2026-09-09-phase2-foreign-detach.md). Rust, Python,
JVM, .NET, and Go language wrappers are explicitly out of scope per the user's
instruction.

Phase 3 adds an opt-in experimental Linux x86-64 async context path for
manually registered safe intervals. It reserves a process-wide real-time
signal only when the prior disposition is default, validates the live M/G/P
ticket and execution generations, and captures into two preallocated G-owned
capsule banks. A dedicated M landing path returns to the scheduler before
publishing `READY`; origin-M dispatch restores through a fresh Linux signal
frame. Restore observation and bank retirement are separate, and direct
task-to-task handoffs are disabled in activated experimental runtimes. Compatibility
runtimes that do not opt in remain cooperative; unknown PCs and unsupported contexts defer instead of being captured. The
experiment prepares every configured primary, probes each native binding behind
an all-worker startup barrier, and binds started Gs to their origin M through
yield, wait and offload completion. Origin-ready queues and service pins are
separate from ordinary stealable P queues. It refuses helper-M activation and
returns `ENOTSUP` from `llam_enter_blocking()` while enabled; declared callback
offload remains supported with bounded admission tickets. A private async drain
service keeps accepted execution alive through stop without widening ordinary
STOPPING ticket rules. It
is not a general-purpose mechanism for arbitrary C or foreign code. See
[Phase 3 Async Context Capture](https://github.com/Feralthedogg/LLAM/blob/133723562cd0f441678f1f422eef667f4b914950/docs/superpowers/specs/2026-09-09-phase3-async-context.md).

Phase 6 adds the versioned `.llam.preempt` metadata parser/inspector and an
optional LLVM pass plus `llam-clang` driver. Cooperative polls are inserted only
at proven natural-loop latches. Async-safe annotations are limited to
structurally restricted Linux x86-64 leaf code and remain an explicit author
review obligation. `off`/`auto`/`required` are opt-in build/tooling modes;
`required` verifies the final link contains at least one accepted range. The
runtime does not automatically load or register these records, so metadata
does not enable async preemption. The private module-registration hook can
validate and copy `ASYNC_SAFE` ranges against an image before enable; absent
optional metadata is a legacy no-op and required metadata fails closed. Compatibility
runtime behavior stays cooperative.
See [Preemption metadata](../../guides/preempt-metadata.md). Rust,
Python, JVM, .NET, and Go language-binding wrappers are not included.

Phase 7 records compiled async capability, safe-range readiness, active backend,
and deterministic fallback reason privately on each runtime. On a compatibility
runtime, `OFF` stays off, `COOPERATIVE` stays cooperative, and `AUTO`/`STRICT` use hybrid capture only
when the experimental Linux x86-64 backend is enabled with safe ranges; all
other PCs remain deferred to a safepoint. Unsupported platforms report
cooperative fallback/`ENOTSUP`. Internal dumps include async capture, collision,
context-byte, and acknowledgment-latency metrics alongside foreign-detach and
relief-worker counters. Public structs, exports, and JSON statistics remain
unchanged. See [Phase 7 capability and fallback policy](../capability-policy.md).

## Boundary audit

`scripts/check_legacy_lease_boundary.py` composes the unchanged Phase 0
worker-identity scanner with the narrow Phase 1A allowlist.  `active_g` is
allowed only as the field of `struct llam_worker`, during initialization of the
embedded `primary_worker` and `opaque_helper_worker` slots, and inside the
three `llam_worker_{publish,load,clear}_active_g` helpers in `scheduler.c`.
Every other direct `active_g` use remains a boundary error. The audit also
rejects a shard-level `current` or `preempt_thread` authority, duplicate
processor topology/wake, queue/timer, or allocator/cache fields on the shard,
copied node backend/task registry/metrics/trace/opaque-helper-wake/lease state
on the processor cell, bare pause
ACK decisions without request/epoch/incarnation matching, watchdog targets not
held through the lease snapshot and revalidation, reverse `opaque_lock`/
`shard->lock` acquisition, and ownerless lease grants without a validated
claimant. It scans only the native C runtime sources; Rust, Python, JVM, .NET,
and Go language wrappers remain explicitly excluded by user scope.

The checker is intentionally a conservative lexical audit rather than a full C
parser or preprocessor/data-flow engine.  Its exact canonical allowlist is:
the worker `active_g` declaration, two shard-slot initializers, and three
direct worker helpers; merge/steal matchers with a positive request bit and
generation plus the ACK bit and all three exact lease/worker tuple comparisons;
direct tuple sampling arguments/publication stores with stable field provenance;
direct lease-coordinator origins and the four named owner transitions (plus
the audited init/finalize/redirect/unbind clears); the two watchdog snapshots,
exact positive/failure guards, active-G check, and lock-held signal; and direct
pthread scheduler locks with a visible first/second ID order.  Reassignments,
compound/unary or macro-hidden writes, parenthesized/cast/comma lvalues,
unknown ACK/owner/lock wrappers, unsupported pointer/address aliases,
conditional nonzero tuple stores, duplicate tuple/ACK stores, any tuple/ACK
store after the sole nonzero ACK, noncanonical indices, and proof markers in
unreachable or non-dominating branches are rejected rather than inferred.  A
publication's conditional zero-reset stores are allowed only before that
publication edge; the tuple values themselves remain unconditional and
field-derived. Trylock/timedlock success is treated as held for ordering
purposes, and watchdog failure exits must use the braced direct-return shape.
Macro expansion and compiler-specific control-flow semantics are not inferred;
any future source shape that needs those semantics must add an explicit
audited helper or a dedicated checker update.  These are lexical
canonical-allowlist rules, not a replacement for a C parser, preprocessor, CFG,
or whole-program data-flow analysis. Phase 1B queue, timer, allocator, cache,
and logical I/O-route ownership is implemented. Phase 2 foreign detach/reattach
and fail-closed relief checks are implemented. Phase 3 async context capture is
experimental and opt-in; Rust, Python, JVM, .NET, and Go wrappers remain
explicitly excluded. Phase 6 compiler metadata tooling is implemented, but its
records are not automatically registered with the runtime. Phase 7 private
capability and fallback reporting is implemented without extending the public
ABI or stats schema.

Make and CMake use the same `scripts/audit_production_test_hooks.py` policy
for both production static and shared artifacts.  The checker invokes
`nm -a` (falling back to `llvm-nm`), rejects uppercase externally defined
`llam_*_test_*` symbols, permits lowercase/local no-op fallback symbols in
unoptimized Debug artifacts, and retains the existing
`llam_broker_client_self_test_*` symbols as the sole named exception.  Make
uses the configured `$(PYTHON)` interpreter; CMake registers the audit when
Python 3 is found and marks it skipped with code 77 when neither symbol tool
is available.  The native source and Phase 0 checks remain required for
normal supported builds.
