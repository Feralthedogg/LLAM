# Runtime Embedding and Host Boundary Design

**Date:** 2026-07-29

**Status:** Approved as part of the runtime-backend productization program.

## Goal

LLAM must be usable as a language-runtime backend without assuming that OS TLS,
signals, scheduler threads, or the host event loop belong exclusively to LLAM.

## Task context

Each task owns a caller-supplied `user_context` and four inline pointer slots.
They start null, follow the logical task across migration, are cleared before
allocator reuse, and are never dereferenced or freed by LLAM. Access is O(1).
Outside a managed task the accessors return `ENOTSUP`; an invalid slot returns
`EINVAL`.

Spawn options append `user_context`. ABI metadata reports the inline slot
count.

## Switch hooks

Runtime options append opt-in resume/suspend callbacks and one callback
context. The caller owns that context through runtime destruction.

Every real scheduler-to-task, task-to-scheduler, and direct task-to-task
transition passes one sanitizer-aware switch gateway. Direct paths install the
next task cursor only after suspending the current task. Logical task `errno`
is saved/restored around callbacks.

Required ordering is:

```text
scheduler -> task: install cursor; resume(next); switch
task -> scheduler: save errno; suspend(current); switch
task A -> task B: save A errno; suspend(A); install B cursor;
                  resume(B); restore B errno; switch
```

Callbacks are nonblocking, non-reentrant into LLAM, non-throwing, and
thread-safe across different tasks. The same task is never hooked
concurrently. Destruction does not invent synthetic switch events.

## Hard pinned contract

`PINNED` means execution only on the task's logical home shard, not a preference
and not a particular pthread. A same-shard opaque compensation helper may run
it.

Steal, merge, rehome, spawn redirect, mark-runnable redirect, opaque redirect,
and global overflow selection must all reject a foreign pinned task and kick
its home shard. Queue fallback cannot silently weaken the contract.

## External host-loop mode

The initial coherent external mode has exactly one scheduler shard and
disables dynamic workers. I/O, blocking, and controller helpers may still
exist. `llam_runtime_run_handle` returns `ENOTSUP`.

An unmanaged, non-reentrant `drive_once` claims the runtime for one nonblocking
quantum, drains injection and due timers, runs at most one task segment, clears
TLS and execution ownership, and returns:

- `PROGRESS` when work or a task segment was processed;
- `IDLE` when live work remains but nothing is runnable or due;
- `DONE` when no live task remains.

Concurrent driving returns `EBUSY`; managed/reentrant driving returns
`ENOTSUP`. The runtime remains reusable after `DONE`.

`next_deadline` returns an absolute LLAM clock deadline or `UINT64_MAX`.
A dedicated coalesced doorbell is owned by LLAM and borrowed by the host:
eventfd on Linux, nonblocking CLOEXEC pipe/socketpair on Darwin/BSD, and a
manual-reset waitable handle on Windows. Driving drains and race-safely rearms
it. Host code must not close it.

## Sanitizer fiber contract

The raw assembly/backend switch is renamed and callable only from the common
gateway. CI symbol/call-site auditing rejects other callers.

ASan task state stores the fake-stack token. The gateway calls
`__sanitizer_start_switch_fiber` before the raw switch and
`__sanitizer_finish_switch_fiber` when the source resumes, with exact usable
stack bounds. Bootstrap and terminal transitions follow sanitizer API
requirements.

TSan creates one token per task after context creation and one native scheduler
token per scheduler OS thread. Fiber switches preserve the physical thread's
ordering because LLAM scheduler state is thread-local and intentionally shared
across successive logical fibers; using the no-sync flag makes those TLS
handoffs appear concurrent. Cross-worker task races remain observable and are
covered by a two-worker positive control. Task tokens are destroyed only after
control has safely returned to the scheduler.

Capability wrappers allow deterministic contract-stub tests without requiring
an instrumented binary. Positive-control ASan and TSan tests prove the
instrumentation actually detects an intentionally bad fiber case.

## Signal and alternate-stack ownership

Signal installation is opt-in/configurable, including preemption signal and
full opt-out. Multiple runtimes may coexist only with compatible process-wide
configuration.

Alternate signal stacks are per OS thread. LLAM borrows a valid host altstack
or allocates a guarded mapping owned, restored, and freed by that same thread.
Primary schedulers and opaque helpers never share shard-level saved altstack
state.

For non-LLAM faults, the handler chains the saved `SA_SIGINFO` or classic host
action with recursion protection. Teardown restores an action only if LLAM
still owns it. Guard faults retain the documented fatal path. SIGBUS handling
is platform-gated.

## Host hardening and fork

Build profiles are `off`, `compatible`, and `strict`. Supported profiles add
stack protector, FORTIFY, RELRO/NOW, non-executable-stack markings, and
capability-probed stack-clash protection. arm64 assembly receives the required
GNU-stack marker.

After any LLAM runtime initializes, a forked child may only perform
async-signal-safe preparation followed by `execve` or `_exit`. It may not call
LLAM or use inherited handles. LLAM installs no partial `atfork` repair.
Callers should fork before initialization or use `posix_spawn`; the parent
remains supported after child exec/exit.

## Verification

Tests cover ABI prefixes, context/slot reuse, exact hook order including a
proven direct handoff, TLS installation across migration, every pinned escape
route, external doorbell/deadline/drive races, sanitizer contract and positive
controls, per-thread altstack ownership, host-handler chaining/replacement,
signal opt-out, hardening artifacts, and parent-continues/child-exec fork use.
