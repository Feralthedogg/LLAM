# Execution Model

LLAM is a user-level N:M scheduler. Many stackful tasks run over a smaller set
of OS worker threads. A task writes ordinary C control flow, while LLAM decides
when to park, resume, migrate, or wake it.

## Lifecycle

The default-runtime path is:

```c
llam_runtime_init(NULL);
llam_task_t *task = llam_spawn(root, arg, NULL);
llam_run();
llam_join(task);
llam_runtime_shutdown();
```

Embedding hosts should usually prefer explicit runtime handles instead of the
process-default wrappers. See [Runtime Handles](runtime-handles.md).

## Tasks

A task is a `void (*)(void *)` function with its own stack. It can yield,
sleep, wait on synchronization primitives, perform runtime I/O, or enter a
blocking compensation region.

Every task handle returned by `llam_spawn*()` must be consumed by one of:

- `llam_join()` or `llam_join_until()`
- `llam_detach()`
- `llam_task_group_join()` or `llam_task_group_destroy()` for grouped children

Deadline APIs use absolute `llam_now_ns()` timestamps:

```c
uint64_t deadline = llam_now_ns() + 10ULL * 1000ULL * 1000ULL;
int rc = llam_join_until(task, deadline);
```

## Scheduling Boundaries

LLAM is stackful and cooperative at the actual context-switch boundary. A
CPU-bound loop that never calls LLAM can delay cancellation, wake handoff, and
shutdown. Use safepoints in hot loops:

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
