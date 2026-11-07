# ABI stability

This contract defines the public ABI and runtime semantics that language
implementations may rely on when loading LLAM dynamically.

## Stable public ABI

LLAM exposes one stable public ABI family: public symbols declared in
`include/llam/runtime.h`, `include/llam/io.h`, `include/llam/managed.h`, and
`include/llam/scope.h` (ABI 2.5+) and `include/llam/executor.h` (development ABI 2.6).

All other non-static implementation symbols are private even if a platform
linker happens to export them. Language runtimes must bind only documented
public symbols.

The development tree public ABI version is (published 3.0.0 archives remain ABI 2.4):

```text
LLAM_VERSION         = 3.0.0
LLAM_ABI_VERSION_MAJOR = 2
LLAM_ABI_VERSION_MINOR = 6
LLAM_ABI_VERSION       = (major << 16) | minor
```

ABI 2.6 adds cancellation-shielded borrowed group drain, short nested no-preemption
regions and shared local executors. `llam_spawn_opts_t` grows from 32 to 40 bytes
on supported 64-bit ABIs, appending `local_executor` at offset 32. Callers with the
old 32-byte prefix retain ordinary scheduling; an incomplete pointer tail is never
read. Unsized `llam_spawn` and `llam_task_group_spawn` also read only that original
32-byte prefix; appended fields require the size-aware `_ex` APIs.
`llam_local_executor_opts_t` is 8 bytes, with zero flags and reserved fields.
Other public layouts are unchanged. Resolve the new symbols and require minor >= 6
before using them. See [execution controls](../api/executors.md) for thread ownership
and [borrowed group drain](../api/tasks.md#development-borrowed-group-drain) for the
successful-return lifetime boundary.

ABI 2.5 adds owned scopes, typed value handles, independent futures and cancellation-safe
scope drain in `<llam/scope.h>`. It preserves all existing layouts and group/result
contracts. The new descriptor and result-contract records are fixed 32/64-byte
layouts; outcome and scope reports are 24/72 bytes on supported 64-bit ABIs.
Ownership-bearing calls require complete V1 prefixes before transferring pointers.
Scope reports allow compatible-prefix copies. New symbol availability requires
minor >= 5. The optional `<llam/future.hpp>` C++17 host wrapper adds no runtime
symbols. See [owned scopes](../api/scopes.md) for ownership and callback lifetimes.

ABI 2.4 adds `llam_runtime_create_compat`, `llam_runtime_init_compat_ex` and
`llam_runtime_init_compat` for the former boundary/safepoint behavior. In 3.0 the
ordinary and managed constructors default to `ASYNC_REQUIRED`. Function calling
conventions and struct layouts are retained; the default scheduling behavior
intentionally changes for the 3.0 release. A binary that passes a plain C entry to a
runtime from `llam_runtime_create()` now gets `ENOTSUP` from spawn, and one that sets
`LLAM_PREEMPT_OFF` or `LLAM_PREEMPT_COOPERATIVE` gets `EINVAL` from the constructor.
Those callers need a `*_compat` constructor. New function availability requires ABI
minor >= 4; resolve symbols before binding them. See the
[managed runtime contract](../../guides/managed-c.md).

ABI 2.3 adds explicit-runtime object constructors and diagnostics, runtime resource
limits and bounded drain, linked group cancellation, per-operation I/O wait
options, progress-preserving transfers, task results and owned task-local cleanup.
`llam_runtime_opts_t` grows from 56 to 80 bytes at its tail. Old 56-byte callers
retain automatic worker counts and unlimited task/blocking-job admission. New
limits use fixed-width fields; input tails are read only when completely present.
`llam_wait_opts_t` is 24 bytes, group options are 8 bytes, task results are 16 bytes,
and runtime resources are 64 bytes on supported 64-bit targets. Query functions
copy only the compatible prefix and preserve an unknown caller tail. New function
availability requires ABI minor >= 3; resolve symbols before binding them.

ABI 2.2 adds `<llam/managed.h>` and the managed constructor, module, policy
query and native-affine callback interfaces. `llam_preempt_opts_t` and
`llam_module_opts_t` are 24 bytes; `llam_preempt_info_t` is 168 bytes on the
supported 64-bit ABIs. The information structure retains its earlier 88-byte
prefix, the 144-byte quantum tail, and the appended prepared-pool counters.
All option/query interfaces copy only the caller/library compatible prefix,
use defaults for omitted input tails and preserve unknown caller output tails.

ABI rules:

- Major version changes are binary-incompatible.
- Minor version changes are additive and binary-compatible. ABI 2.4 keeps that binary
  compatibility and also changes the default constructor policy, as described above.
- Opaque handles remain opaque; callers must not inspect or allocate them.
- Public structs can grow only at the tail during the same ABI major version.
- Callers must pass their compiled struct size to `llam_abi_get_info()`.
- The library copies only the overlapping struct prefix for ABI metadata.
- Pointers returned as static strings are owned by the library and remain valid
  while its module remains loaded. Retain the loader reference while using them.

## Dynamic loader checks

Dynamic loaders should resolve and check these symbols first:

```c
uint32_t llam_abi_version(void);
const char *llam_version_string(void);
int llam_abi_get_info(llam_abi_info_t *info, size_t info_size);
```

A loader should reject the library when `info.abi_major` differs from the
header it was compiled against.

## Contract matrix

These symbols are the minimum stable surface that language runtimes may bind
directly. Unless noted otherwise, integer APIs return `0` on success and `-1`
with `errno` set on failure.

| Group | Symbols | Stable contract |
| --- | --- | --- |
| ABI/version | `llam_abi_version`, `llam_version_string`, `llam_abi_get_info` | Safe to call before runtime initialization. Returned strings are library-owned static storage. |
| Lifecycle | `llam_runtime_opts_init`, `llam_runtime_init_ex`, `llam_runtime_init`, `llam_runtime_init_compat_ex`, `llam_runtime_init_compat`, `llam_runtime_create`, `llam_runtime_create_compat`, `llam_runtime_default`, `llam_runtime_run_handle`, `llam_runtime_request_stop`, `llam_run`, `llam_runtime_destroy`, `llam_runtime_shutdown` | Explicit runtime handles are the canonical embedding boundary. The ordinary constructors require asynchronous preemption and compiler-qualified task entries; the `*_compat` constructors run plain C entries with boundary and safepoint preemption. Legacy host-thread lifecycle calls operate on `llam_runtime_default()`; managed task spawn/stop/shutdown wrappers target the current owner runtime and cannot cross-stop peer runtimes. Shutdown releases backend resources while public cleanup handles keep their documented post-shutdown behavior. FFI bindings should prefer `_ex` and option initializers. |
| Managed runtime | `llam_preempt_opts_init`, `llam_runtime_create_managed`, `llam_runtime_preempt_info_ex`, `llam_runtime_module_register_ex`, `llam_runtime_module_unregister`, `llam_call_native_affine_result` | Policy, query, and module interfaces in `<llam/managed.h>`, with caller-size handshakes. Creation and module changes are host operations. See the [managed runtime contract](../../guides/managed-c.md). |
| Experimental Windows async | `llam_runtime_enable_experimental_windows_async_preempt_ex` | ABI 2.1 additive expert-only, pre-run one-way opt-in. Caller certifies PE-image instruction ranges and explicitly acknowledges possible process-wide fail-fast; LLAM verifies image backing/lifetime and machine support, not code semantics. Unavailable builds export the symbol but return `ENOTSUP`. |
| Tasks | `llam_spawn_opts_init`, `llam_spawn_ex`, `llam_spawn`, `llam_runtime_spawn_ex`, `llam_join`, `llam_join_until`, `llam_detach`, `llam_yield`, `llam_task_safepoint`, `LLAM_PREEMPT_POLL`, `LLAM_PREEMPT_POLL_EVERY`, `llam_current_task` | Task handles are opaque and runtime-owned. One successful join or detach consumes the task handle. A runtime that requires asynchronous preemption refuses an unqualified entry with `ENOTSUP`. Embedders with explicit runtimes should spawn with `llam_runtime_spawn_ex()`. FFI bindings should prefer `_ex` and option initializers. |
| Time | `llam_now_ns`, `llam_sleep_ns`, `llam_sleep_until`, `llam_timer_create_ex`, `llam_timer_create`, `llam_timer_wait`, `llam_timer_wait_until`, `llam_timer_reset`, `llam_timer_cancel`, `llam_timer_destroy` | Monotonic nanosecond clock. Deadline APIs use absolute `llam_now_ns()` units. Waitable interval timers are runtime-aware public handles; destroy returns `EBUSY` while a wait/reset/cancel operation is active. |
| Cancellation | `llam_cancel_token_create`, `llam_cancel_token_destroy`, `llam_cancel_token_cancel`, `llam_cancel_token_is_cancelled` | Cancellation tokens are explicit handles. Destroy fails with `EBUSY` while live waiters or task/I/O observers still hold references. Cancel/query also fail with `EBUSY` during active public-handle teardown. Managed cross-runtime token operations fail with `EXDEV`. |
| Mutex/cond | `llam_mutex_*`, `llam_cond_*` | Runtime-aware waits park managed tasks. Mutex lock, try-lock, and unlock and condition waits fail with `ENOTSUP` outside a managed task; condition signal and broadcast also work from host threads. Destroy returns `EBUSY` while owners, waiters, in-flight wake returns, or public operations remain. |
| Channels | `llam_channel_*` | Pointer-valued bounded channel. Capacity must be at least `1`; `llam_channel_create(0)` fails with `EINVAL`. Destroy returns `EBUSY` while buffered values, waiters, close-woken returns, or public operations remain. Use result-style receive APIs when `NULL` is a valid payload. |
| Blocking | `llam_call_blocking_result`, `llam_call_blocking`, `llam_enter_blocking`, `llam_leave_blocking` | Prevents long blocking work from pinning scheduler workers. `llam_call_blocking_result` is the unambiguous FFI-safe form. Blocking callback return value is user-owned. |
| I/O | `llam_read`, `llam_write`, `llam_read_handle`, `llam_write_handle`, `llam_close`, `llam_close_handle`, `llam_read_owned`, `llam_recv_owned`, `llam_recvfrom`, `llam_sendto`, `llam_recvfrom_owned`, `llam_accept`, `llam_connect`, `llam_poll_fd`, `llam_poll_handle` | Scheduler-safe descriptor/socket/HANDLE operations. Datagram wrappers preserve peer-address output for UDP/QUIC/DNS-style workloads. File descriptor and handle ownership follows platform convention unless owned-buffer APIs say otherwise. Descriptors used with LLAM I/O should be closed with `llam_close()` or `llam_close_handle()` so runtime-local descriptor state observes the close boundary. |
| Positional I/O | `llam_pread`, `llam_pwrite`, `llam_preadv`, `llam_pwritev`, `llam_pread_handle`, `llam_pwrite_handle`, `llam_preadv_handle`, `llam_pwritev_handle` | Explicit-offset file I/O that does not modify the current file offset. Windows fd/socket positional APIs fail with `ENOTSUP`; Windows file users should use the HANDLE APIs. |
| Blocking syscall wrappers | `llam_getaddrinfo_result`, `llam_freeaddrinfo_result`, `llam_open_async`, `llam_stat_path_ex` | DNS and filesystem metadata/open wrappers use the runtime blocking path from managed tasks and direct execution from unmanaged callers. `getaddrinfo` resolver errors are returned through `gai_error`; LLAM submission/cancellation failures still use `-1` and `errno`. |
| Owned buffers | `llam_io_buffer_opts_init`, `llam_io_buffer_alloc_ex`, `llam_io_buffer_alloc`, `llam_io_buffer_alloc_aligned`, `llam_io_buffer_alignment`, `llam_io_buffer_data`, `llam_io_buffer_size`, `llam_io_buffer_capacity`, `llam_io_buffer_release`, `llam_pread_owned_aligned`, `llam_pread_handle_owned_aligned` | Buffers returned by owned-read APIs are runtime-allocated and caller-released through `llam_io_buffer_release()`. Aligned allocation supports direct-I/O callers that need sector/page-aligned storage. |
| Signals | `llam_signal_set_create_ex`, `llam_signal_wait`, `llam_signal_wait_until`, `llam_signal_set_destroy` | Linux-only opt-in signal wait sets backed by `sigtimedwait`. Other platforms return `ENOTSUP`. Signal sets are process-level subscriptions and should be created before worker threads start when possible so signal masks are inherited consistently. |
| Stats/debug | `llam_runtime_collect_stats_ex`, `llam_runtime_collect_stats_ex_handle`, `llam_runtime_collect_stats`, `llam_runtime_write_stats_json`, `llam_dump_runtime_state`, task name/class helpers | Observability surface. FFI bindings should prefer `_ex` or handle-scoped `_ex_handle` stats snapshots and JSON export for machine logs. |
