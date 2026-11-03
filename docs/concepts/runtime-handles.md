# Runtime Handles

Explicit runtime handles are the canonical embedding boundary for LLAM 3.0 (ABI 2.x).
They let hosts create independent scheduler instances and attach root tasks to
the intended runtime.

## Default Runtime

The legacy wrappers use the process-default runtime:

`llam_runtime_init`, `llam_spawn`, `llam_run` and `llam_runtime_shutdown`
retain their process-default compatibility behavior.

This is convenient for a single LLAM-owned application, but it is a poor fit for
embedding multiple independent subsystems in one process.

## Explicit Runtime

Use explicit handles for embedding and for any host-owned lifecycle:

Follow the checked [minimal program](../getting-started.md#minimal-program)
and [complete group example](../examples/embedding.md). A failed spawn must not
become a successful empty run, and host joins require another active driver or
already-completed tasks. A successful `llam_join_result` consumes a normal task
handle. Group child handles are borrowed and are consumed by group join.

## Ownership Rules

Runtime-aware objects are bound to the runtime that created them. Managed use
from another runtime fails with `EXDEV`. Destroy can fail with `EBUSY` while
waiters, active operations, buffered values, or live observers still exist.

Objects covered by owner checks include:

- tasks and task groups
- channels, mutexes, condition variables
- cancellation tokens
- timers and signal sets
- runtime-owned I/O buffers (detached buffers are owner-neutral)
- backend I/O waits

Use the `llam_runtime_*_create`/`create_ex` constructors from a host to stamp
objects with the intended runtime. The matching stop, statistics, dump and resource
query forms accept that same runtime handle. `llam_runtime_drain_until` closes
admission and bounds host waiting while preserving all handles on timeout.

## FFI Guidance

Bindings should prefer size-aware entry points and public size macros:

```c
llam_runtime_opts_t opts;
if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) return 1;

llam_runtime_t *rt = NULL;
if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &rt) != 0) return 1;
```

The ABI rules are documented in [ABI Contract](../abi.md).
