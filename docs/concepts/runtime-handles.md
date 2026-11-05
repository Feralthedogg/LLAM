# Runtime handles

Explicit runtime handles are the canonical embedding boundary for LLAM 3.0 (ABI 2.x).
They let hosts create independent scheduler instances and attach root tasks to
the intended runtime.

## Default runtime

The legacy wrappers take no runtime handle. Which runtime they act on depends on
where they are called from:

| Wrapper | From a host thread | From a managed task |
| --- | --- | --- |
| `llam_runtime_init`, `llam_runtime_init_ex` | Initializes the process-default runtime. | Fails with `EBUSY`. |
| `llam_runtime_init_compat`, `llam_runtime_init_compat_ex` | Initializes the process-default runtime. | Also the process-default runtime, whichever runtime owns the task. |
| `llam_run` | Drives the process-default runtime. | Fails with `EBUSY`, or `EINVAL` when the process-default runtime is not initialized. |
| `llam_spawn`, `llam_spawn_ex` | Spawns on the process-default runtime. | Spawns on the task's owner runtime. |
| `llam_runtime_request_stop` | Stops the process-default runtime. | Stops the task's owner runtime. |
| `llam_runtime_shutdown` | Stops and releases the process-default runtime. | Requests a cooperative stop of the task's owner runtime. |

`llam_runtime_init` requires asynchronous preemption, like `llam_runtime_create`.
`llam_runtime_init_compat` selects compatibility execution instead.

This is convenient for a single LLAM-owned application, but it is a poor fit for
embedding multiple independent subsystems in one process.

## Explicit runtime

Use explicit handles for embedding and for any host-owned lifecycle. Choose the
constructor by how the task code is built: `llam_runtime_create()` runs tasks from the
[managed C toolchain](../guides/managed-c.md), and `llam_runtime_create_compat()` runs
plain C. Both return the same kind of handle.

Follow the checked [minimal program](../start/quickstart.md#run-a-minimal-program)
and [complete group example](../examples/embedding.md). A failed spawn must not
become a successful empty run, and host joins require another active driver or
already-completed tasks. A successful `llam_join_result` consumes a normal task
handle. Group child handles are borrowed and are consumed by group join.

## Ownership rules

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

## FFI guidance

Bindings should prefer size-aware entry points and public size macros:

```c
llam_runtime_opts_t opts;
if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) return 1;

llam_runtime_t *rt = NULL;
if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &rt) != 0) return 1;
```

A binding whose task entries come from another compiler or language needs
`llam_runtime_create_compat()`, which takes the same arguments. The symbol exists from
ABI 2.4, so check the loaded ABI minor before resolving it.

The ABI rules are documented in [ABI contract](../reference/abi/index.md).
