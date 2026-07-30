# Embedding LLAM

This guide is for applications, plugins, and language runtimes that host LLAM
instead of treating it as the whole process runtime.

## Link From CMake

Installed SDK:

```cmake
find_package(llam CONFIG REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam::runtime)
```

Source checkout:

```cmake
add_subdirectory(path/to/LLAM)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam_runtime)
```

Use `llam_runtime_shared` when a host loads LLAM dynamically.

## Own The Runtime

The official embedding path is the explicit runtime-handle lifecycle:

1. Create a handle with `llam_runtime_create()`.
2. Attach root work with `llam_runtime_spawn_ex()`.
3. Drive only that handle with `llam_runtime_run_handle()`.
4. Join or detach returned task handles.
5. Release the runtime with `llam_runtime_destroy()`.

```c
llam_runtime_t *rt = NULL;
if (llam_runtime_create(NULL, 0, &rt) != 0) {
    return 1;
}

llam_task_t *root_task = llam_runtime_spawn_ex(rt, root, user_data, NULL, 0);
if (root_task == NULL || llam_runtime_run_handle(rt) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}

if (llam_join(root_task) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}
llam_runtime_destroy(rt);
```

Use one explicit runtime per independent embedding boundary. The legacy
process-default lifecycle remains a convenience path for simple LLAM-owned
programs, but hosts should not repeatedly initialize and shut it down from
concurrent embedding threads.

## Own The Process-Signal Policy

POSIX embedders can choose how LLAM participates in process-wide signals.
Always initialize the full option struct before changing the policy:

```c
#include <signal.h>

llam_runtime_opts_t opts;
llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);

/* Preserve the host's complete signal policy. */
opts.signal_flags = 0U;
```

The initializer enables both `LLAM_RUNTIME_SIGNAL_F_PREEMPT` and
`LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT` for compatibility with the default
runtime. `preempt_signal=0` selects the platform default, currently `SIGUSR1`
on POSIX. A custom preemption signal must be catchable and cannot overlap a
guard-fault signal. The preemption field is ignored when its flag is clear.

Signal-participating runtimes share one process configuration. Their flags and
resolved preemption signal must match exactly; an incompatible runtime create
fails with `EBUSY`. A runtime with `signal_flags=0` takes no process-signal
reference and can coexist with either configuration. Windows accepts the
fixed-width fields for ABI portability but does not install POSIX actions.

LLAM chains a non-guard fault to the classic or `SA_SIGINFO` action that
preceded its installation. It restores an action at final teardown only while
it still owns that action, so a host or sanitizer that replaces the handler
remains authoritative. Guard-page hits stay fatal. Darwin and BSD also cover
`SIGBUS`; Linux uses `SIGSEGV`.

Alternate signal stacks belong to OS threads, not logical shards. A scheduler
entry borrows a sufficiently large enabled host stack. Otherwise the entering
thread creates a guarded alternate stack and restores and releases it before
leaving. Successive external-drive calls may therefore use different host
threads without sharing saved stack state.

## Fork Before Initialization

Prefer `posix_spawn()` or fork before creating any LLAM runtime. If a
multithreaded host forks after initialization, the child may perform only
async-signal-safe preparation followed immediately by `execve()` or `_exit()`.
It must not call LLAM, use an inherited runtime/task/synchronization handle, or
attempt runtime teardown. LLAM deliberately installs no partial `pthread_atfork`
repair for locks and worker state that cannot be made usable in the child.

The parent remains supported after the child execs or exits. The regression
suite creates a live runtime, forks a child directly into `execve()`, and then
runs and joins new work on the original parent runtime.

## Dynamic Loading

Resolve ABI symbols first:

```c
uint32_t abi = llam_abi_version();

llam_abi_info_t info;
llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE);
```

Reject libraries whose `abi_major` differs from the binding. Minor ABI changes
are additive.

## Struct Size Handshakes

Use the public size macros:

```c
llam_spawn_opts_t opts;
llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);

llam_runtime_spawn_ex(rt, fn, arg, &opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
```

Bindings should model ABI-facing enum values and flags as fixed-width integers,
as described in [ABI Contract](../abi.md).

## Boundary Choice

In-process opaque handles harden against stale use, wrong-family casts, simple
forgery, and owner mismatch. They are not a sandbox against arbitrary
same-process memory read/write.

Use a process boundary and broker-mode isolation when untrusted code must not
read runtime internals. See [Security Model](../security.md).
