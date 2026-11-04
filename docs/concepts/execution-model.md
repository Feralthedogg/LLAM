# Execution model

LLAM is a user-level N:M scheduler. Many stackful tasks run over a smaller set
of OS worker threads. A task writes ordinary C control flow, while LLAM decides
when to park, resume, migrate, or wake it.

## Lifecycle

Embedding hosts should treat explicit runtime handles as the primary lifecycle:

Create an explicit runtime, spawn root/group work for that handle, run its
driver, consume completed handles and destroy the runtime. Check each operation;
the [minimal program](../start/quickstart.md#run-a-minimal-program) and
[complete embedding example](../examples/embedding.md) cover success, failure and
cancellation. `llam_runtime_create()` and `llam_runtime_init()` require asynchronous
admission. Explicit `*_compat` constructors select compatibility execution.

See [Runtime handles](runtime-handles.md) for the owner rules behind that
boundary.

## Tasks

A task is a `void (*)(void *)` function with its own stack. It can yield,
sleep, wait on synchronization primitives, perform runtime I/O, or enter a
blocking compensation region.

Every task handle returned by `llam_spawn*()` must be consumed by one of:

- `llam_join()` / `llam_join_until()` or their result-returning forms
- `llam_detach()`
- `llam_task_group_join()` / `llam_task_group_join_until()` for grouped children; destroy requires no live child handles

Keep task arguments and pointed-to resources alive until the task has completed,
including cancellation and runtime-error cleanup. `llam_task_set_result()` and
`llam_join_result()` carry an application value and error independently of runtime
`errno`. Group child handles are borrowed: inspect a completed child with
`llam_task_get_result()` before joining its group, which consumes child handles.

Deadline APIs use absolute `llam_now_ns()` timestamps:

```c
uint64_t deadline = llam_now_ns() + 10ULL * 1000ULL * 1000ULL;
int rc = llam_join_until(task, deadline);
```

## Scheduling boundaries

The ordinary constructors use required asynchronous admission. Explicit
`*_compat` constructors use boundary/safepoint preemption. An unqualified
CPU-bound loop can delay cancellation and shutdown. Managed constructors admit
compiler/image metadata and probe native Ms before asynchronous capture; use the
[managed C guide](../guides/managed-c.md) and query requested/effective
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

## Stop and drain

`llam_runtime_request_stop()` asks workers to stop cooperatively. Shutdown then
drains runtime-owned work and releases backend resources. Managed wrappers route
to the current task's owner runtime, so explicit-runtime code does not
accidentally stop the process default runtime.

For a host-owned runtime, `llam_runtime_request_stop_handle` targets that handle
and `llam_runtime_drain_until` bounds waiting without freeing resources. Timeout
requires retaining callback memory and retrying or completing final teardown;
see [Embed LLAM](../guides/embedding.md#bound-resources-and-shutdown).
