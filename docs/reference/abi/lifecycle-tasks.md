# Lifecycle and task semantics

The exact behavior of runtime lifecycle and task functions, for bindings that
need more than the API summary.

## Runtime lifecycle

### Create and run

`llam_runtime_init_ex()` initializes the legacy process-default runtime with an
explicit option size. `llam_runtime_init()` is a C convenience wrapper for that
same default runtime. Embedders that need independent scheduler instances should
prefer `llam_runtime_create()`, `llam_runtime_spawn_ex()`,
`llam_runtime_run_handle()`, and `llam_runtime_destroy()`. A runtime must be
created or initialized successfully before managed tasks or runtime-aware
synchronization objects can be owned by it.

These constructors require asynchronous preemption:

| Condition | Result |
| --- | --- |
| No qualified native asynchronous profile | `ENOTSUP` from the constructor or the first run |
| Effective `preempt_mode` is `LLAM_PREEMPT_OFF` or `LLAM_PREEMPT_COOPERATIVE` | `EINVAL` from the constructor |
| Called from a managed task | `EBUSY` from the constructor |

`llam_runtime_create_compat()`, `llam_runtime_init_compat_ex()`, and
`llam_runtime_init_compat()` take the same arguments and select boundary and safepoint
preemption instead. None of these conditions applies to them.

`llam_run()` drives the process-default runtime. `llam_runtime_run_handle()`
drives an explicit runtime handle. Both run until all live tasks complete, the
target runtime requests stop, or a fatal runtime error is observed. A
cooperative stop request is a successful scheduler outcome: the run call returns
`0` after the target runtime observes `llam_runtime_request_stop()` and all live
tasks have exited or become irrelevant to the drained runtime. Backend or fatal
runtime failures return `-1` with `errno` set.

### Stop

`llam_runtime_request_stop()` requests cooperative stop and wakes scheduler,
I/O, and blocking workers. It does not forcibly kill live tasks; `llam_run()`
still waits for live tasks to exit or observe cancellation through user code.
Unmanaged callers target the process-default runtime. Managed callers target
their current task's owner runtime.

### Shutdown and destroy

`llam_runtime_shutdown()` is the idempotent legacy-default cleanup wrapper and
may be called after a failed or partial default-runtime initialization.
`llam_runtime_destroy()` performs the same lifecycle cleanup for an explicit
runtime handle. Cleanup requests cooperative stop, joins runtime-owned OS
threads that were started, and releases backend resources. It is not a task-join
API: callers that need a graceful task drain should request stop, drive
`llam_run()` or `llam_runtime_run_handle()` for the target runtime, and then
clean it up. After cleanup, task handles owned by that runtime are no longer
joinable, blocking runtime-aware operations that require a managed task fail by
contract, and backend I/O ownership is gone. Public cleanup handles keep the
exceptions documented by their APIs: owned buffers remain valid until
`llam_io_buffer_release()`, and already-buffered channel values may be drained
with `llam_channel_try_recv_result()` before destroy.

From a managed LLAM task or scheduler frame, shutdown performs only the
stop-request portion. This avoids freeing scheduler, stack, or I/O state while
it is still active on the current call stack.

## Task semantics

### Spawn

`llam_spawn_ex()` creates a stackful managed task with an explicit option size
and makes it runnable. `llam_spawn()` is a C convenience wrapper. The task entry
receives the user pointer exactly once. Returning from the entry function
completes the task and wakes join waiters. If `llam_spawn_opts_t` supplies a
cancellation token, that token must belong to the runtime that will own the new
task. Cross-runtime tokens are rejected with `EXDEV` before the task is
published.

A runtime from an ordinary constructor admits only entries from a compiler-qualified
image. Any other entry fails with `ENOTSUP`, and `LLAM_SPAWN_F_NO_PREEMPT` fails with
`EINVAL`, before the task is published. A compatibility runtime accepts any entry and
rejects `LLAM_SPAWN_F_NATIVE_AFFINE` with `ENOTSUP`.

### Join and detach

`llam_join()` waits for task completion. Managed LLAM task callers park
cooperatively. A task inside `llam_enter_blocking()` has relinquished its
processor and instead waits on its existing native thread; `llam_yield()` in
that foreign scope is a no-op. A foreign join preserves timeout/cancellation
and completion ownership, including after a stop-time failed leave. Unmanaged
OS-thread callers block the calling thread and do not
drive scheduler progress by themselves; another thread must call `llam_run()` or
the target must already be able to complete. `llam_join_until()` uses an
absolute deadline in `llam_now_ns()` units. Passing deadline `0` is an already
expired absolute deadline, not "disabled". Timeout failures return `-1` with
`errno` set to `ETIMEDOUT`; after timeout, the task remains joinable by the same
handle.

A task handle returned by `llam_spawn*()` must be completed by exactly one of
`llam_join()`, `llam_join_until()` success, or `llam_detach()`. A successful
join or detach consumes the task handle. The caller must not use the handle
again, and double completion is invalid. A concurrent second join attempt fails
with `errno = EBUSY` before the handle is consumed. `llam_detach()` fails with
`errno = EBUSY` if a join waiter already owns completion. Detached tasks still
run to completion and remain counted as live runtime work until their entry
function returns.

### Spawn from a host thread

Unmanaged host calls to legacy `llam_spawn*()` target the default runtime. They
pin that runtime's public-operation gate before allocating or publishing a task,
so a concurrent default-runtime shutdown cannot tear down scheduler storage under
spawn. If the default runtime is missing, already destroying, or in a saturated
lifecycle state, spawn fails closed with the runtime gate's `errno` rather than
publishing work.

### Task groups

Task groups own the child task handles spawned through the group. Group
cancellation cancels only the group's automatically attached cancellation token;
children spawned with an explicit caller-owned token remain controlled by that
token. The child task pointer returned by `llam_task_group_spawn*()` is a
borrowed diagnostics handle; regular `llam_join()` and `llam_detach()` reject it
with `EBUSY` so only `llam_task_group_join*()` can consume group-owned children.
The child is marked as group-owned before it can execute, so a self-join or
self-detach path cannot consume the borrowed handle first. Destroying a task
group with live children, active spawn/cancel operations, or an in-progress join
fails with `EBUSY`. Calls racing with a completed group destroy fail with
`EINVAL` instead of dereferencing reclaimed group storage. Child tasks are
spawned on the group's owner runtime, including host-thread calls that are not
currently running inside a managed task. If the group's public-operation
lifecycle counter reaches its saturated sentinel, new group operations fail
closed with `EBUSY` instead of spawning or consuming children while teardown
safety cannot be proven.

### Yield and safepoints

`llam_yield()` is cooperative. It never transfers ownership of user memory and
does not imply a memory fence beyond the synchronization performed by the
runtime scheduler. It also does not promise a context switch: a
[direct yield](../environment.md#runtime-profiles) with no runnable peer can return to
the same task for up to 64 consecutive turns. `llam_task_safepoint()` is the
lower-overhead progress hook for CPU-bound loops; it is a no-op outside a managed task
and may yield only when runtime policy or preemption requests require it.

`LLAM_PREEMPT_POLL()` and `LLAM_PREEMPT_POLL_EVERY(counter, interval)` are the
public hot-loop helpers for request-based automatic preemption. They do not let
the runtime asynchronously switch stacks at arbitrary instruction boundaries;
they provide explicit LLAM boundaries where an already requested preemption can
be honored. `LLAM_PREEMPT_POLL_EVERY` evaluates its arguments once, treats
`interval` values `0` and `1` as "poll every call", and is safe to use in tight
loops where a full `llam_yield()` would be too expensive.

Task code admitted by a runtime that requires asynchronous preemption needs no
polls; see the [managed C guide](../../guides/managed-c.md). On a compatibility
runtime, the separately enabled experimental Win64 backend is an exception only for
caller-certified PE-image intervals, not arbitrary instructions or arbitrary
C/C++ code. Its public opt-in is host-only, one-way, and may terminate the
hosting process if a committed remote context transaction becomes uncertain.
