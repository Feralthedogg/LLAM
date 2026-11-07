# Execution controls

!!! note "Development ABI 2.6"
    These interfaces require the development headers and runtime with ABI minor
    6 or newer. Published 3.0.0 archives provide ABI 2.4.

Include `<llam/executor.h>` for execution controls. They preserve the runtime's
requested asynchronous policy and apply to the current logical task and native
execution binding.

## Short no-preemption regions

| API | Purpose |
| --- | --- |
| `llam_preempt_region_enter` | Enter a nested, bounded region owned by the current native M/G execution. |
| `llam_preempt_region_leave` | Leave one matching region; final leave restores ordinary preemption. |

A successful enter prevents LLAM native capture and cooperative task handoff
until the matching leave. Nested calls preserve the outer guard. Host callers,
guarded foreign callbacks and announced blocking frames are rejected with `ENOTSUP`.
A foreign native-entry guard cannot enter or leave a caller's region. Underflow
in a valid managed task returns `EINVAL`; nesting exhaustion returns `EOVERFLOW`.
Failed transitions preserve the prior state. Returning from the task with an open
region is a process-fatal contract violation, reported with exit status 125.

Keep protected work short, bounded, nonblocking and balanced. Do not allocate,
spawn, park, unwind, deliver cancellation or call unknown foreign code inside it.
Stateful LLAM calls are refused with `ENOTSUP`; native allocation or foreign effects
must also be excluded by the caller/compiler. A language compiler must check all
control-flow exits and transitive effects, including indirect calls. Calling this
API does not certify a `@nopreempt` annotation or an arbitrary language frontend.

Bracket the **complete call** of a no-preemption function, including its epilogue.
Restore protected invariants before leave: final leave may service pending
preemption immediately. Cancellation requests remain pending and can be observed
at the next permitted checkpoint. An expired sleep can still succeed immediately
under its existing deadline semantics.

This is not a lock or an allocation-lifetime proof. The OS can deschedule the
native thread, and other OS threads can continue accessing shared state. Continue
to enforce the appropriate synchronization, borrow and lifetime rules.

The native regression uses one processor and a no-poll rendezvous to establish
asynchronous progress before entry. A separate host probe checks that preemption
counters stay stable while the region is held, and a peer detects any execution
inside that interval. It also requests cancellation and verifies delivery after
balanced leave. Core tests cover nesting, foreign entry refusal, overflow,
suspension refusal and fatal unbalanced task exit.

## Shared local executors

| API | Purpose |
| --- | --- |
| `llam_local_executor_opts_init` | Initialize the reserved 8-byte options record; both fields must be zero. |
| `llam_runtime_local_executor_create_ex` | Reserve one native-affine thread slot on an explicit managed runtime; publish an opaque executor handle on success. |
| `llam_local_executor_close` | Seal admission without waiting or cancelling existing members. Idempotent. |
| `llam_local_executor_drain` | Seal and wait for all member execution and actual native-thread/TLS retirement, deferring caller cancellation and deadlines. |
| `llam_local_executor_destroy` | Consume a closed, quiescent executor with no concurrent public operation. |
| `LLAM_LOCAL_EXECUTOR_OPTS_CURRENT_SIZE` | Current size for the options initializer and executor creation. |

Creation accepts `NULL, 0` options and leaves the output untouched on failure.
`flags` and `reserved0` are reserved zero fields. The runtime must have managed
execution pools; a compatibility-only runtime returns `ENOTSUP`. One executor
uses one `native_affine_capacity` slot, shared by all its members. Exhausted
capacity returns `EAGAIN`; task admission limits still apply separately.

Set `llam_spawn_opts_t.local_executor` on the root and every local child and submit
through `llam_runtime_spawn_ex`, `llam_spawn_ex` or `llam_task_group_spawn_ex`, with
the complete current option size. Unsized legacy spawn wrappers read only the old
prefix and ignore this new field. An
executor is **not implicitly inherited**: leaving that field `NULL` requests
ordinary scheduling. All selected members run on the same LLAM-owned OS thread
through yield, parking, asynchronous capture/restore and task-local exit cleanup.
The creating host thread is not that thread. Submit only data valid on the
executor thread; create thread-affine values inside its root before lending them
to local children. `ASYNC_REQUIRED` and `ASYNC_AUTO` still require qualified local
entries. Plain C local entries need an explicitly `COOPERATIVE` managed policy.

Closing rejects later submissions with `EPIPE`; accepted spawn reservations are
included in drain. A native slot already entering retirement also rejects later
admission. When the run driver finishes, its native threads retire and open local
domains close; create a new executor for a later run. Draining from any member
returns `EDEADLK`. Concurrent drains
or premature destroy return `EBUSY`; stale/wrong-family handles return `EINVAL`
and managed cross-runtime use returns `EXDEV`. A host drain needs a running driver
for outstanding members and never runs the scheduler itself. Runtime stop seals
the domain and requests ordinary member cancellation. Drain keeps waiting through
that request; fatal failure does not authorize release of borrowed storage.

An executor drain does not consume member task handles. Join or detach ordinary
handles separately; a group drain consumes its borrowed child handles. Public
member handles may outlive a destroyed executor after execution has retired.
A normal group drain establishes member retirement; executor drain additionally
establishes native TLS/FLS teardown. Never drain the executor from its own TLS
callback. Domain TLS must not retain a pointer to a member's stack after that
member returns. Storage intentionally borrowed until domain exit must instead
remain valid through successful executor drain.

Local tasks can interleave between instructions even on one OS thread. Protect
multi-instruction local invariants, such as a non-atomic reference-count update,
with the short regions above. This also requires language-level effect and borrow
checks. Owned-scope/future value destructors keep their thread-neutral contract;
place thread-affine cleanup in the local task wrapper or task-local exit cleanup.

The [installed SDK fixture](https://github.com/Feralthedogg/LLAM/blob/main/tests/fixtures/managed/local_executor_consumer.c)
uses a root and three local children, parent cancellation, borrowed group drain,
no-poll asynchronous progress and same-thread cleanup. The native runtime suite
also delays a native TLS destructor to verify that executor drain waits for it.
