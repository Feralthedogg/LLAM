# Public API Reference

This page maps the stable public surface in `include/llam/runtime.h`,
`include/llam/io.h`, and `include/llam/platform.h`. Use it to find the right API
group quickly, then check [Options](options.md) and [ABI Contract](../abi.md)
when you need field-level or binary-compatibility rules.

## API Contracts

| Contract | Rule |
| --- | --- |
| Deadlines | Absolute deadlines use `llam_now_ns()` units. `0` means already expired unless the function documents another meaning. |
| Errors | Functions returning `-1` set `errno`; common values include `EINVAL`, `EAGAIN`, `ETIMEDOUT`, `ECANCELED`, `EPIPE`, `EBUSY`, `EXDEV`, `ENOTSUP`, and `ENOMEM`. |
| Runtime ownership | Runtime-aware objects are bound to the runtime that created them. Cross-runtime managed use fails with `EXDEV`. |
| ABI-sized structs | Use a matching `*_opts_init()` helper where provided and pass `*_CURRENT_SIZE`. Windows async opt-in deliberately has no initializer that could silently acknowledge process-wide fail-fast. |
| Nullable payloads | Channel and blocking APIs with `_result` suffix distinguish a successful `NULL` payload from failure. |
| Windows files | `llam_fd_t` is a socket on Windows. Use HANDLE APIs for file and pipe I/O. |

## ABI And Versioning

| API | Purpose |
| --- | --- |
| `llam_abi_version` | Return packed public ABI version. Dynamic loaders should resolve this first. |
| `llam_version_string` | Return the static source/API version string. |
| `llam_abi_get_info` | Fill version, platform, and struct-size metadata for loaders and bindings. |
| `LLAM_ABI_INFO_CURRENT_SIZE` | Size token for `llam_abi_get_info`. |
| `LLAM_VERSION_*` / `LLAM_ABI_VERSION_*` | Compile-time source and ABI version macros. |

## Lifecycle

Embedding code should use the handle-scoped lifecycle:
`llam_runtime_create()`, `llam_runtime_spawn_ex()`,
`llam_runtime_run_handle()`, and `llam_runtime_destroy()`. The process-default
APIs are compatibility wrappers for simple single-runtime programs.

| API | Purpose |
| --- | --- |
| `llam_runtime_opts_init` | Fill runtime options with ABI-safe defaults. |
| `llam_runtime_init_ex` | Initialize the process-default compatibility runtime with an explicit option size. |
| `llam_runtime_init` | Convenience default-runtime initializer. |
| `llam_runtime_default` | Return the default runtime handle. |
| `llam_runtime_create` | Canonical embedding entry point for an explicit runtime handle. |
| `llam_runtime_run_handle` | Canonical embedding scheduler driver for one runtime handle. |
| `llam_runtime_request_stop_handle` | Stop one explicit runtime; peer managed tasks get `EXDEV`. |
| `llam_runtime_get_resources_ex` | Caller-sized snapshot of pool sizes, live/retained work and closing state. |
| `llam_runtime_drain_until` | Host bounded stop/drain; timeout preserves runtime and resources. |
| `llam_run` | Drive the process-default compatibility runtime. |
| `llam_runtime_request_stop` | Request cooperative stop and wake workers. |
| `llam_runtime_destroy` | Canonical embedding teardown. Passing `NULL` aliases default shutdown. |
| `llam_runtime_shutdown` | Stop and release default-runtime resources. |

## Managed runtimes

For compiler-qualified C, use the [managed runtime API](managed-runtime.md):
`llam_preempt_opts_init`, `llam_runtime_create_managed`,
`llam_runtime_preempt_info_ex`, `llam_runtime_module_register_ex`,
`llam_runtime_module_unregister`, and `llam_call_native_affine_result`.
`<llam/managed.h>` defines caller-sized policy, resource and module structures.
The constructor defaults to `ASYNC_AUTO`; `ASYNC_REQUIRED` rejects an
unqualified entry or native profile before user entry. Existing constructors
and the expert Windows interface below retain their compatibility contracts.

## Experimental Windows asynchronous preemption

`llam_runtime_enable_experimental_windows_async_preempt_ex()` atomically opts
an idle, fixed-primary Win64 runtime into the experimental backend. The caller passes
nonoverlapping, half-open, **caller-certified** PE-image PC ranges and the
`LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST` flag. LLAM copies the ranges,
checks PE executable backing and machine support, and pins their modules; it
cannot prove that arbitrary code is safe to interrupt. A committed remote
context failure can terminate the entire hosting process. Other builds retain
the public symbol but return `ENOTSUP`. See [Options](options.md) and the
[Windows build contract](../build.md#native-windows-build) before using it.
A successful call racing destroy reports that enablement committed first, not
that the runtime remains usable after the call returns.
Each actual primary worker must register before user dispatch starts. Captured
full native contexts stay on their capture worker; only a completed eligible
explicit cooperative yield with all old asynchronous ownership retired can
release a G for ordinary migration. Internal yields and park/offload wakes
remain origin-bound. Do not retain native-TLS addresses across migration.
Dynamic scaling, async helper/relief execution and inline foreign detach are
not admitted. Native qualification currently covers two and three primary Ms
with the pinned scalar-leaf fixture, not every workload or compiler profile.

## Tasks

| API | Purpose |
| --- | --- |
| `llam_spawn_opts_init` | Fill spawn options with ABI-safe defaults. |
| `llam_runtime_spawn_ex` | Spawn into an explicit runtime handle. |
| `llam_spawn_ex` | Spawn into the selected current/default runtime with explicit option size. |
| `llam_spawn` | Convenience spawn wrapper. |
| `llam_join` | Wait indefinitely and consume a task handle on success. |
| `llam_join_until` | Wait until an absolute deadline. A timeout leaves the handle joinable. |
| `llam_detach` | Consume a task handle without joining. |
| `llam_yield` | Cooperatively yield the current managed task. |
| `llam_task_safepoint` | Mark progress in CPU-bound loops. |
| `LLAM_PREEMPT_POLL` | Public hot-loop safepoint macro. |
| `LLAM_PREEMPT_POLL_EVERY` | Safepoint macro for counted loops. Arguments are evaluated once. |
| `llam_task_set_class` | Change the current task scheduler class. |

`llam_task_set_result` publishes an application value/error at completion.
`llam_join_result` and `llam_join_result_until` copy it before consuming a normal
handle; `llam_task_get_result` peeks after completion, including a borrowed group
child. An unsuccessful join leaves output untouched.

## Task Groups

| API | Purpose |
| --- | --- |
| `llam_task_group_create` | Legacy group: explicit child tokens override group cancellation. |
| `llam_task_group_create_ex` / `llam_runtime_task_group_create_ex` | Opt in to `LLAM_TASK_GROUP_F_LINK_CANCEL`: group OR explicit token. |
| `llam_task_group_spawn_ex` | Spawn a grouped child with explicit option size. |
| `llam_task_group_spawn` | Convenience grouped spawn wrapper. |
| `llam_task_group_cancel` | Cancel the group source; external tokens themselves are never cancelled. |
| `llam_task_group_join` | Join all grouped children. |
| `llam_task_group_join_until` | Join grouped children until an absolute deadline. |
| `llam_task_group_destroy` | Destroy a group after child work is drained or cancelled. |

## Time, Timers, And Signals

| API | Purpose |
| --- | --- |
| `llam_now_ns` | Monotonic nanosecond clock used by deadlines. |
| `llam_sleep_ns` | Relative sleep for the current managed task. |
| `llam_sleep_until` | Sleep until an absolute deadline. |
| `llam_timer_create_ex` | Create a waitable interval timer from `llam_timer_opts_t`. |
| `llam_timer_create` | Create a timer with only an interval. |
| `llam_timer_wait` | Wait for timer ticks. |
| `llam_timer_wait_until` | Wait for timer ticks until a deadline. |
| `llam_timer_reset` | Reset first deadline and interval. |
| `llam_timer_cancel` | Cancel pending timer waits. |
| `llam_timer_destroy` | Destroy a timer. |
| `LLAM_TIMER_OPTS_CURRENT_SIZE` | Size token for `llam_timer_create_ex`. |
| `llam_signal_set_create_ex` | Create a signal wait set. Linux-only; other platforms return `ENOTSUP`. |
| `llam_signal_wait` | Wait for a signal event. |
| `llam_signal_wait_until` | Wait for a signal event until a deadline. |
| `llam_signal_set_destroy` | Destroy a signal wait set. |
| `LLAM_SIGNAL_OPTS_CURRENT_SIZE` | Size token for `llam_signal_set_create_ex`. |

## Cancellation

| API | Purpose |
| --- | --- |
| `llam_cancel_token_create` | Create a cancellation token. |
| `llam_cancel_token_cancel` | Request cancellation for current and future observers. |
| `llam_cancel_token_is_cancelled` | Query cancellation state. |
| `llam_cancel_token_destroy` | Destroy a token when it has no live waiters or observers. |

## Synchronization

| API | Purpose |
| --- | --- |
| `llam_mutex_create` / `llam_mutex_destroy` | Create and destroy runtime-aware non-recursive mutexes. |
| `llam_mutex_lock` | Lock and park the task when contended. |
| `llam_mutex_lock_until` | Timed lock with an absolute deadline. |
| `llam_mutex_trylock` | Non-parking lock attempt. |
| `llam_mutex_unlock` | Unlock and wake one waiter when present. |
| `llam_cond_create` / `llam_cond_destroy` | Create and destroy runtime-aware condition variables. |
| `llam_cond_wait` | Wait while atomically releasing and reacquiring a LLAM mutex. |
| `llam_cond_wait_until` | Timed condition wait. |
| `llam_cond_signal` | Wake one condition waiter. |
| `llam_cond_broadcast` | Wake all condition waiters. |

## Channels And Select

| API | Purpose |
| --- | --- |
| `llam_channel_create` / `llam_channel_destroy` | Create and destroy bounded pointer channels. |
| `llam_channel_send` | Send and park when the channel is full. |
| `llam_channel_try_send` | Non-parking send attempt. |
| `llam_channel_send_until` | Timed send. |
| `llam_channel_recv_result` | Receive with explicit out parameter. Use when `NULL` is a valid payload. |
| `llam_channel_try_recv_result` | Non-parking receive attempt with explicit out parameter. |
| `llam_channel_recv_until_result` | Timed receive with explicit out parameter. |
| `llam_channel_recv` | Convenience receive wrapper. |
| `llam_channel_recv_until` | Convenience timed receive wrapper. |
| `llam_channel_close` | Close the channel and wake blocked senders/receivers. |
| `llam_channel_select` | Select one ready send/receive operation. |
| `LLAM_CHANNEL_SELECT_MAX_OPS` | Maximum operation count accepted by `llam_channel_select`. |

## Stream And Handle I/O

| API | Purpose |
| --- | --- |
| `llam_read` / `llam_write` | Stream fd/socket I/O through the runtime path where possible. |
| `llam_read_handle` / `llam_write_handle` | Generic platform handle I/O. On Windows, use this for HANDLEs. |
| `llam_read_when_ready` | Poll and read in one runtime operation. |
| `llam_writev` | Scatter/gather write. |
| `llam_pread` / `llam_pwrite` | Positional I/O for POSIX-style descriptors. |
| `llam_preadv` / `llam_pwritev` | Scatter/gather positional I/O for POSIX-style descriptors. |
| `llam_pread_handle` / `llam_pwrite_handle` | Positional I/O for generic handles. |
| `llam_preadv_handle` / `llam_pwritev_handle` | Scatter/gather positional I/O for generic handles. |
| `llam_close` / `llam_close_handle` | Close while invalidating runtime-local descriptor or handle state. |

## Datagram, Socket, And DNS

| API | Purpose |
| --- | --- |
| `llam_recvfrom` / `llam_sendto` | Datagram I/O with peer addresses. |
| `llam_recv_owned` / `llam_recvfrom_owned` | Datagram receive into runtime-owned buffers. |
| `llam_accept` | Accept a socket connection with scheduler-safe waiting. |
| `llam_connect` | Connect a socket with scheduler-safe waiting. |
| `llam_poll_fd` | Wait for descriptor/socket readiness. |
| `llam_poll_handle` | Wait for generic handle readiness where supported. |
| `llam_getaddrinfo_result` | DNS through the runtime blocking path. |
| `llam_freeaddrinfo_result` | Release DNS results returned by `llam_getaddrinfo_result`. |

## Owned I/O Buffers

| API | Purpose |
| --- | --- |
| `llam_io_buffer_opts_init` | Fill owned-buffer options with ABI-safe defaults. |
| `llam_io_buffer_alloc_ex` | Allocate a runtime-owned buffer from explicit options. |
| `llam_io_buffer_alloc` | Allocate a buffer by capacity. |
| `llam_io_buffer_alloc_aligned` | Allocate a buffer with explicit alignment. |
| `llam_io_buffer_alignment` | Return buffer alignment. |
| `llam_read_owned` | Read into a runtime-owned buffer. |
| `llam_pread_owned_aligned` | Positional read into an aligned owned buffer. |
| `llam_pread_handle_owned_aligned` | Handle-based positional read into an aligned owned buffer. |
| `llam_io_buffer_data` | Return mutable buffer storage. |
| `llam_io_buffer_size` | Return the number of valid bytes. |
| `llam_io_buffer_capacity` | Return usable capacity. |
| `llam_io_buffer_release` | Release a runtime-owned buffer. |

## Filesystem Wrappers

| API | Purpose |
| --- | --- |
| `llam_open_async` | Open a filesystem path through the blocking path. |
| `llam_stat_path_ex` | Fill portable file metadata into `llam_file_stat_t`. |
| `LLAM_FILE_TYPE_*` | Portable file type values used by `llam_file_stat_t`. |

## Blocking Integration

| API | Purpose |
| --- | --- |
| `llam_call_blocking_result` | Run blocking work and return through an explicit out parameter. |
| `llam_call_blocking` | Convenience blocking callback API. |
| `llam_enter_blocking` | Mark the current task as entering an opaque blocking region. |
| `llam_leave_blocking` | Mark the current task as leaving an opaque blocking region. |

## Diagnostics And Introspection

| API | Purpose |
| --- | --- |
| `llam_runtime_collect_stats_ex` | Size-aware stats snapshot for the current/default runtime. |
| `llam_runtime_collect_stats_ex_handle` | Size-aware stats snapshot for an explicit runtime handle. |
| `llam_runtime_collect_stats` | Convenience stats snapshot. |
| `llam_runtime_write_stats_json` / `llam_runtime_write_stats_json_handle` | Write JSON statistics for selected/default or explicit runtime. |
| `llam_dump_runtime_state` / `llam_dump_runtime_state_handle` | Best-effort dump for selected/default or explicit runtime. |
| `LLAM_RUNTIME_STATS_CURRENT_SIZE` | Size token for `llam_runtime_collect_stats_ex` and handle-scoped stats. |
| `llam_task_flags` | Return task spawn/runtime flags. |
| `llam_task_id` | Return the runtime-assigned task id. |
| `llam_task_state_name` | Return a stable task state string. |
| `llam_task_class` | Return the scheduler class for a task. |
| `llam_current_task` | Return the current managed task, or `NULL` outside LLAM. |

## Task Local Storage

| API | Purpose |
| --- | --- |
| `llam_task_local_key_create` | Allocate a task-local storage key. |
| `llam_task_local_key_delete` | Delete a task-local storage key. |
| `llam_task_local_get` | Read the current task's value for a key. |
| `llam_task_local_set` | Set or clear the current task's value for a key. |
| `LLAM_TASK_LOCAL_INVALID_KEY` | Invalid key sentinel. |

## Platform Types

| Type or macro | Purpose |
| --- | --- |
| `llam_fd_t` | POSIX fd or Windows `SOCKET`. |
| `llam_handle_t` | POSIX fd alias or Windows HANDLE-compatible type. |
| `llam_iovec_t` / `llam_mut_iovec_t` | Portable immutable and mutable scatter/gather slices. |
| `LLAM_INVALID_FD` / `LLAM_FD_IS_INVALID` | Portable fd/socket invalid sentinel and predicate. |
| `LLAM_INVALID_HANDLE` / `LLAM_HANDLE_IS_INVALID` | Portable handle invalid sentinel and predicate. |
| `LLAM_PLATFORM_WINDOWS` / `LLAM_PLATFORM_POSIX` | Major platform-family feature macros. |
| `LLAM_PLATFORM_LINUX` / `LLAM_PLATFORM_DARWIN` / `LLAM_PLATFORM_BSD` | Platform detection macros; BSD detection now leads to a build error. |
| `LLAM_PLATFORM_FREEBSD` / `LLAM_PLATFORM_OPENBSD` / `LLAM_PLATFORM_NETBSD` / `LLAM_PLATFORM_DRAGONFLY` | Legacy BSD detection macros; unsupported in 3.0.0. |
| `LLAM_PLATFORM_KQUEUE` | Non-zero on macOS targets. |
| `LLAM_ARCH_X86_64` / `LLAM_ARCH_AARCH64` | Public architecture feature macros. |
| `LLAM_PLATFORM_NAME` | Human-readable platform name used by diagnostics. |
| `LLAM_API` | Public symbol visibility/import/export macro. |

## Explicit Ownership And I/O Budgets (ABI 2.3)

Host-side constructors are `llam_runtime_channel_create`,
`llam_runtime_mutex_create`, `llam_runtime_cond_create`,
`llam_runtime_cancel_token_create`, `llam_runtime_task_group_create[_ex]`,
`llam_runtime_timer_create_ex` and `llam_runtime_signal_set_create_ex`. They
retain the runtime through publication and preserve existing object lifetimes.
Old constructors retain current-task/default-runtime selection.

`llam_wait_opts_init` initializes the caller-sized common wait policy.
`llam_read_ex`, `llam_write_ex`, `llam_connect_ex`, `llam_poll_fd_ex`,
`llam_read_handle_ex` and `llam_write_handle_ex` accept it. Stream fd operations
also have absolute `llam_read_until`, `llam_write_until`, `llam_connect_until`
and `llam_poll_fd_until` forms. New poll forms take an `int` mask in the short-mask range; legacy poll ABI
is unchanged. Poll timeout returns 0; byte/connect timeout returns -1/`ETIMEDOUT`.

`llam_read_exact`, `llam_read_exact_ex`, `llam_write_all` and
`llam_write_all_ex` report confirmed progress through
a required `size_t *`, including on failure. Early EOF is `EPIPE`; premature
zero write is `EIO`. `llam_task_local_set_owned` adds nonblocking same-M exit
cleanup and `llam_task_local_take_result` transfers the value without cleanup.
See [Embedding LLAM](../guides/embedding.md) for cancellation composition,
endpoint restrictions, drain timeout recovery and complete failure cleanup.
