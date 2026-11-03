# Execution Model

LLAM is a user-level N:M scheduler. Many stackful tasks run over a smaller set
of OS worker threads. A task writes ordinary C control flow, while LLAM decides
when to park, resume, migrate, or wake it.

## Lifecycle

Embedding hosts should treat explicit runtime handles as the primary lifecycle:

Create an explicit runtime, spawn root/group work for that handle, run its
driver, consume completed handles and destroy the runtime. Check each operation;
the [minimal program](../getting-started.md#minimal-program) and
[complete embedding example](../examples/embedding.md) cover success, failure and
cancellation. The process-default APIs remain a compatibility path for simple
single-runtime programs.

See [Runtime Handles](runtime-handles.md) for the owner rules behind that
boundary.

## Tasks

A task is a `void (*)(void *)` function with its own stack. It can yield,
sleep, wait on synchronization primitives, perform runtime I/O, or enter a
blocking compensation region.

Every task handle returned by `llam_spawn*()` must be consumed by one of:

- `llam_join()` / `llam_join_until()` or their result-returning forms
- `llam_detach()`
- `llam_task_group_join()` / `llam_task_group_join_until()` for grouped children; destroy requires no live child handles

Deadline APIs use absolute `llam_now_ns()` timestamps:

```c
uint64_t deadline = llam_now_ns() + 10ULL * 1000ULL * 1000ULL;
int rc = llam_join_until(task, deadline);
```

## Scheduling Boundaries

Compatibility constructors use boundary/safepoint preemption. An unqualified
CPU-bound loop can delay cancellation and shutdown. Managed constructors admit
compiler/image metadata and probe native Ms before asynchronous capture; use the
[managed C guide](../reference/managed-runtime.md) and query requested/effective
policy. Arbitrary unqualified C loops have no async interruption guarantee.
Compatibility code can use safepoints in hot loops:

```c
size_t poll_counter = 0;

for (uint64_t i = 0; i < work_items; ++i) {
    process_item(i);
    LLAM_PREEMPT_POLL_EVERY(poll_counter++, 1024U);
}
```

## Runtime I/O

Runtime I/O is written like blocking C:

```c
ssize_t n = llam_read(fd, buf, sizeof(buf));
```

From a managed task, LLAM first tries a direct fast path. If the operation
would block, the task parks and the backend waits through io_uring, kqueue,
IOCP, or a blocking-helper fallback.

## Stop And Drain

`llam_runtime_request_stop()` asks workers to stop cooperatively. Shutdown then
drains runtime-owned work and releases backend resources. Managed wrappers route
to the current task's owner runtime, so explicit-runtime code does not
accidentally stop the process default runtime.

For a host-owned runtime, `llam_runtime_request_stop_handle` targets that handle
and `llam_runtime_drain_until` bounds waiting without freeing resources. Timeout
requires retaining callback memory and retrying or completing final teardown;
see [Embedding LLAM](../guides/embedding.md#bound-resources-and-shutdown-waiting).
