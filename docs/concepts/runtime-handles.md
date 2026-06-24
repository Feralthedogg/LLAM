# Runtime Handles

Explicit runtime handles are the canonical embedding boundary for LLAM 2.x.
They let hosts create independent scheduler instances and attach root tasks to
the intended runtime.

## Default Runtime

The legacy wrappers use the process-default runtime:

```c
llam_runtime_init(NULL);
llam_spawn(root, arg, NULL);
llam_run();
llam_runtime_shutdown();
```

This is convenient for a single LLAM-owned application, but it is a poor fit for
embedding multiple independent subsystems in one process.

## Explicit Runtime

Use explicit handles when the host owns lifecycle:

```c
llam_runtime_t *rt = NULL;

if (llam_runtime_create(NULL, 0, &rt) != 0) {
    return 1;
}

llam_task_t *task = llam_runtime_spawn_ex(rt, root, arg, NULL, 0);
int run_rc = llam_runtime_run_handle(rt);

if (task != NULL) {
    (void)llam_join(task);
}

llam_runtime_destroy(rt);
return run_rc;
```

## Ownership Rules

Runtime-aware objects are bound to the runtime that created them. Managed use
from another runtime fails with `EXDEV`. Destroy can fail with `EBUSY` while
waiters, active operations, buffered values, or live observers still exist.

Objects covered by owner checks include:

- tasks and task groups
- channels, mutexes, condition variables
- cancellation tokens
- timers and signal sets
- owned I/O buffers
- backend I/O waits

## FFI Guidance

Bindings should prefer size-aware entry points and public size macros:

```c
llam_runtime_opts_t opts;
llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);

llam_runtime_t *rt = NULL;
llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &rt);
```

The ABI rules are documented in [ABI Contract](../abi.md).
