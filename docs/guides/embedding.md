# Embed LLAM

This guide is for applications, plugins, and language runtimes that host LLAM
instead of treating it as the whole process runtime.

## Link from CMake

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

## Own the runtime

The official embedding path is the explicit runtime-handle lifecycle:

1. Create a handle with `llam_runtime_create_compat()` for plain C tasks, as below, or
   `llam_runtime_create()` for task code from the [managed C toolchain](managed-c.md).
2. Attach root work with `llam_runtime_spawn_ex()`.
3. Drive only that handle with `llam_runtime_run_handle()`.
4. Join or detach returned task handles.
5. Release the runtime with `llam_runtime_destroy()`.

```c
llam_runtime_t *rt = NULL;
if (llam_runtime_create_compat(NULL, 0, &rt) != 0) {
    return 1;
}

llam_task_t *root_task = llam_runtime_spawn_ex(rt, root, user_data, NULL, 0);
if (root_task == NULL || llam_runtime_run_handle(rt) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}

llam_task_result_t result;
if (llam_join_result(root_task, &result, sizeof(result)) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}
llam_runtime_destroy(rt);
```

Use one explicit runtime per independent embedding boundary. The legacy
process-default lifecycle remains a convenience path for simple LLAM-owned
programs, but hosts should not repeatedly initialize and shut it down from
concurrent embedding threads.

## Create objects for the same runtime

A host should use `llam_runtime_channel_create(rt, capacity)`,
`llam_runtime_mutex_create(rt)`, `llam_runtime_cond_create(rt)`,
`llam_runtime_cancel_token_create(rt)` and
`llam_runtime_task_group_create_ex(rt, &options, sizeof(options))`.
Timer and signal creation have handle-scoped `llam_runtime_*_create_ex` forms;
signal sets retain their Linux-only support. Creation pins `rt` through handle
publication. A task of a peer runtime gets `EXDEV`. Old constructors still
choose the current task runtime or the process default from host code.
Detached I/O buffers are owner-neutral and can be transferred between runtimes.

Use `llam_runtime_request_stop_handle`, `llam_runtime_write_stats_json_handle`
and `llam_dump_runtime_state_handle` to target the same runtime for control and
diagnostics. Dump writes remain best effort. The preemption query in
`<llam/managed.h>` exposes requested and effective policy separately. A compatibility
runtime or the `ASYNC_AUTO` policy accepts an unqualified C function, but that does not
make it asynchronously interruptible.

## Compose cancellation and budgets

Create groups with `LLAM_TASK_GROUP_F_LINK_CANCEL` to make each child observe
group cancellation OR its explicit token. Cancelling a group never cancels that
external token, so tasks outside the group remain unaffected. Legacy groups
retain their override contract: an explicit child token replaces the group token.

A child's `llam_spawn_opts_t.deadline_ns` is a persistent request budget, rather
than an active wait timer. Earlier sleeps and shorter I/O calls do not erase it.
Managed waits use the earlier task/call deadline. This is a waiting budget, not a
CPU-loop timeout or an instruction to unwind arbitrary native callbacks. A ready
operation/completion can win a timeout race.

For stream I/O, initialize `llam_wait_opts_t`, set an absolute `deadline_ns` and
an optional additional token, then use `llam_read_ex`, `llam_write_ex`,
`llam_connect_ex` or `llam_poll_fd_ex`. All token sources are ORed. In the options,
zero deadline adds no limit; `*_until(..., 0)` means already expired. Poll returns
zero on timeout, while read/write/connect fail with `ETIMEDOUT`. The older
`llam_read_when_ready` keeps its relative millisecond timeout.

```c
llam_wait_opts_t wait;
if (llam_wait_opts_init(&wait, sizeof(wait)) != 0) return -1;
wait.deadline_ns = llam_now_ns() + UINT64_C(100000000);
wait.cancel_token = request_cancel;
size_t sent = 0;
if (llam_write_all_ex(socket_fd, bytes, length, &wait, sizeof(wait), &sent) != 0) {
    /* sent bytes are confirmed; resending that prefix duplicates application data. */
    return -1;
}
```

`llam_read_exact[_ex]` and `llam_write_all[_ex]` return 0/-1 and always report
confirmed progress, including on cancellation or timeout. Early EOF is `EPIPE`;
a zero-length write before completion is `EIO`. Progress is not a transaction
rollback. Per-call bounded byte I/O requires a managed task and a supported
cancelable endpoint: sockets, POSIX nonblocking FIFOs/character devices, or
Windows overlapped HANDLEs. Unsupported bounded regular/block-file and synchronous
HANDLE operations fail `ENOTSUP` before the native operation. Host bounded
read/write/connect also return `ENOTSUP`; host poll supports deadlines/tokens.
Queued/running blocking callbacks retain their argument/buffer lifetime until
callback completion; cancellation never permits the caller to free memory early.

## Bound resources and shutdown

The ABI 2.3 runtime-options tail supplies `worker_count`,
`blocking_worker_count`, `max_tasks` and `max_blocking_jobs`. Zero retains prior
automatic/unlimited defaults. Worker count selects portable/legacy processors; the
`ASYNC_AUTO` and `COOPERATIVE` managed policies reserve one more compatibility
processor, and `ASYNC_REQUIRED` reserves none. Explicit values take precedence over automatic pool sizing. `worker_count` must
fit available CPUs after an SQPOLL reservation; deterministic mode permits at
most one. Blocking pool size accepts 1..1024. `max_tasks` includes completed,
unjoined objects until reclamation; join/detach releases capacity after all
lifetime pins drain. Blocking admission counts queued and running callbacks.
Rejected admissions return `EAGAIN` without invoking a callback.

Call `llam_runtime_get_resources_ex` for an advisory snapshot; its
[fields and flags](../reference/options/runtime.md#runtime-resources) are listed with
the options. To stop accepting
new tasks/objects and wait for the active driver, use
`llam_runtime_drain_until(rt, deadline, &resources, sizeof(resources))` from a
host thread. Success consumes no handle: join completed work, destroy caller-owned
objects and finally call `llam_runtime_destroy`. Ordinary request-stop remains
reusable; entering drain permanently closes this runtime's admission.

On `ETIMEDOUT`, preserve the runtime, callback arguments, buffers and handles;
inspect live tasks/blocking jobs/public operations and retry after accepted work
finishes. An arbitrary foreign callback cannot be forcibly unwound. This deadline
bounds waiting and does not promise hard real-time OS scheduling or timed final
native-thread destruction. Pending tasks without an active driver yield
`EBUSY`/`NEEDS_RUNNER` before admission closes: start the driver and retry.
Host joins and drain do not implicitly drive the scheduler.

## Results and local ownership

A task can call `llam_task_set_result(value, application_error)`. A successful
`llam_join_result` consumes a normal handle and copies the result before
reclamation. Runtime errors use -1/errno; application errors use `result.error`,
and a successful NULL value remains distinct. A timeout preserves handle and
output. For group children, use `llam_task_get_result` after completion and before
group join, because child handles are borrowed. Unset results have all-zero fields.
The application owns the result pointer's lifetime.

Plain task-local set remains borrowed. `llam_task_local_set_owned` installs a
non-NULL value/callback at an empty key. Exit invokes it once, including after
cancellation and key deletion. Callbacks return normally, do not block and do not
call LLAM task/wait APIs. Compiler-qualified tasks retain the same dedicated
native M, and their imports require the native-affine execution class.
Compatibility tasks invoke cleanup on their exit worker and provide no native
thread affinity: keep thread-affine resources out of that path. Portable/expert
asynchronous tasks reject registration with `ENOTSUP`. Take a value with `llam_task_local_take_result` to
transfer ownership without cleanup; plain set cannot overwrite an owned entry.
Cleanup runs before task completion/result publication and outside runtime locks.

The complete explicit-runtime example linked from
[Quickstart](../start/quickstart.md#run-a-minimal-program) shows the recommended
group/result flow. Close a channel and drain all queued values before destroying
it. After a fatal runtime failure, destroy the runtime to retire accepted tasks;
caller-owned handles still require cleanup. A group containing only invalidated
children can then be destroyed without attempting a stale join.

## Dynamic loading

Resolve ABI symbols first:

```c
uint32_t abi = llam_abi_version();

llam_abi_info_t info;
llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE);
```

Reject libraries whose `abi_major` differs from the binding. Minor ABI changes
are additive.

## Struct size handshakes

Use the public size macros:

```c
llam_spawn_opts_t opts;
llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);

llam_runtime_spawn_ex(rt, fn, arg, &opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
```

Bindings should model ABI-facing enum values and flags as fixed-width integers,
as described in [ABI contract](../reference/abi/index.md).

## Choose a boundary

In-process opaque handles harden against stale use, wrong-family casts, simple
forgery, and owner mismatch. They are not a sandbox against arbitrary
same-process memory read/write.

Use a process boundary and broker-mode isolation when untrusted code must not
read runtime internals. See [Security model](../reference/security/index.md).
