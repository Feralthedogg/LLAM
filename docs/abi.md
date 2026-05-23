# LLAM ABI And Semantic Contract

This document defines the public ABI and runtime semantics that language
implementations may rely on when loading LLAM dynamically.

## ABI Stability

LLAM exposes one stable public ABI family: symbols declared in
`include/llam/runtime.h`.

All other non-static implementation symbols are private even if a platform
linker happens to export them. Language runtimes must bind only documented
public symbols.

The current public ABI version is:

```text
LLAM_VERSION         = 2.0.0
LLAM_ABI_VERSION_MAJOR = 2
LLAM_ABI_VERSION_MINOR = 0
LLAM_ABI_VERSION       = (major << 16) | minor
```

ABI rules:

- Major version changes are binary-incompatible.
- Minor version changes are additive and binary-compatible.
- Opaque handles remain opaque; callers must not inspect or allocate them.
- Public structs can grow only at the tail during the same ABI major version.
- Callers must pass their compiled struct size to `llam_abi_get_info()`.
- The library copies only the overlapping struct prefix for ABI metadata.
- Pointers returned as static strings are owned by the library and remain valid
  until process exit.

Dynamic loaders should resolve and check these symbols first:

```c
uint32_t llam_abi_version(void);
const char *llam_version_string(void);
int llam_abi_get_info(llam_abi_info_t *info, size_t info_size);
```

A loader should reject the library when `info.abi_major` differs from the
header it was compiled against.

## Public API Contract Matrix

These symbols are the minimum stable surface that language runtimes may bind
directly. Unless noted otherwise, integer APIs return `0` on success and `-1`
with `errno` set on failure.

| Group | Symbols | Stable contract |
| --- | --- | --- |
| ABI/version | `llam_abi_version`, `llam_version_string`, `llam_abi_get_info` | Safe to call before runtime initialization. Returned strings are library-owned static storage. |
| Lifecycle | `llam_runtime_opts_init`, `llam_runtime_init_ex`, `llam_runtime_init`, `llam_runtime_create`, `llam_runtime_default`, `llam_runtime_run_handle`, `llam_runtime_request_stop`, `llam_run`, `llam_runtime_destroy`, `llam_runtime_shutdown` | Explicit runtime handles are the canonical embedding boundary. Legacy host-thread lifecycle calls operate on `llam_runtime_default()`; managed task spawn/stop/shutdown wrappers target the current owner runtime and cannot cross-stop peer runtimes. Shutdown releases backend resources while public cleanup handles keep their documented post-shutdown behavior. FFI bindings should prefer `_ex` and option initializers. |
| Tasks | `llam_spawn_opts_init`, `llam_spawn_ex`, `llam_spawn`, `llam_runtime_spawn_ex`, `llam_join`, `llam_join_until`, `llam_detach`, `llam_yield`, `llam_task_safepoint`, `LLAM_PREEMPT_POLL`, `LLAM_PREEMPT_POLL_EVERY`, `llam_current_task` | Task handles are opaque and runtime-owned. One successful join or detach consumes the task handle. Embedders with explicit runtimes should spawn with `llam_runtime_spawn_ex()`. FFI bindings should prefer `_ex` and option initializers. |
| Time | `llam_now_ns`, `llam_sleep_ns`, `llam_sleep_until` | Monotonic nanosecond clock. Deadline APIs use absolute `llam_now_ns()` units. |
| Cancellation | `llam_cancel_token_create`, `llam_cancel_token_destroy`, `llam_cancel_token_cancel`, `llam_cancel_token_is_cancelled` | Cancellation tokens are explicit handles. Destroy fails with `EBUSY` while live waiters or task/I/O observers still hold references. |
| Mutex/cond | `llam_mutex_*`, `llam_cond_*` | Runtime-aware waits park managed tasks; external-thread use is limited to nonblocking or explicitly documented calls. Destroy returns `EBUSY` while owners, waiters, in-flight wake returns, or public operations remain. |
| Channels | `llam_channel_*` | Pointer-valued bounded channel. Capacity must be at least `1`; `llam_channel_create(0)` fails with `EINVAL`. Destroy returns `EBUSY` while buffered values, waiters, close-woken returns, or public operations remain. Use result-style receive APIs when `NULL` is a valid payload. |
| Blocking | `llam_call_blocking_result`, `llam_call_blocking`, `llam_enter_blocking`, `llam_leave_blocking` | Prevents long blocking work from pinning scheduler workers. `llam_call_blocking_result` is the unambiguous FFI-safe form. Blocking callback return value is user-owned. |
| I/O | `llam_read`, `llam_write`, `llam_read_handle`, `llam_write_handle`, `llam_close`, `llam_close_handle`, `llam_read_owned`, `llam_recv_owned`, `llam_accept`, `llam_connect`, `llam_poll_fd`, `llam_poll_handle` | Scheduler-safe descriptor/socket/HANDLE operations. File descriptor and handle ownership follows platform convention unless owned-buffer APIs say otherwise. Descriptors used with LLAM I/O should be closed with `llam_close()` or `llam_close_handle()` so runtime-local descriptor state observes the close boundary. |
| Positional I/O | `llam_pread`, `llam_pwrite`, `llam_preadv`, `llam_pwritev`, `llam_pread_handle`, `llam_pwrite_handle`, `llam_preadv_handle`, `llam_pwritev_handle` | Explicit-offset file I/O that does not modify the current file offset. Windows fd/socket positional APIs fail with `ENOTSUP`; Windows file users should use the HANDLE APIs. |
| Owned buffers | `llam_io_buffer_opts_init`, `llam_io_buffer_alloc_ex`, `llam_io_buffer_alloc`, `llam_io_buffer_alloc_aligned`, `llam_io_buffer_alignment`, `llam_io_buffer_data`, `llam_io_buffer_size`, `llam_io_buffer_capacity`, `llam_io_buffer_release`, `llam_pread_owned_aligned`, `llam_pread_handle_owned_aligned` | Buffers returned by owned-read APIs are runtime-allocated and caller-released through `llam_io_buffer_release()`. Aligned allocation supports direct-I/O callers that need sector/page-aligned storage. |
| Stats/debug | `llam_runtime_collect_stats_ex`, `llam_runtime_collect_stats_ex_handle`, `llam_runtime_collect_stats`, `llam_runtime_write_stats_json`, `llam_dump_runtime_state`, task name/class helpers | Observability surface. FFI bindings should prefer `_ex` or handle-scoped `_ex_handle` stats snapshots and JSON export for machine logs. |

## Inbound Option Structs

Public structs returned by LLAM use explicit size handshakes. Public structs
passed into LLAM use `_ex` entry points:

```c
int llam_runtime_init_ex(const llam_runtime_opts_t *opts, size_t opts_size);
llam_task_t *llam_spawn_ex(llam_task_fn fn, void *arg,
                           const llam_spawn_opts_t *opts,
                           size_t opts_size);
int llam_runtime_opts_init(llam_runtime_opts_t *opts, size_t opts_size);
int llam_spawn_opts_init(llam_spawn_opts_t *opts, size_t opts_size);
```

When `opts` is not `NULL`, `opts_size` must be nonzero. LLAM copies only the
overlapping prefix of the caller's struct and leaves fields beyond that prefix
at the same defaults used by the convenience wrappers. This lets older FFI
bindings pass smaller option structs to newer libraries when fields are
appended at the tail. Passing an option pointer with `opts_size == 0` fails
with `errno = EINVAL`.

The non-`_ex` functions are C convenience wrappers that pass
`sizeof(llam_runtime_opts_t)` or `sizeof(llam_spawn_opts_t)` when options are
provided. New dynamic bindings should resolve and call the `_ex` symbols.

Use the public size macros rather than spelling `sizeof(...)` in generated
bindings:

```c
LLAM_RUNTIME_OPTS_CURRENT_SIZE
LLAM_SPAWN_OPTS_CURRENT_SIZE
LLAM_RUNTIME_STATS_CURRENT_SIZE
LLAM_IO_BUFFER_OPTS_CURRENT_SIZE
```

`llam_runtime_opts_init()` and `llam_spawn_opts_init()` write only the
caller-provided prefix, so bindings can ask the loaded library for its current
defaults without assuming that all-zero options equal the default policy.
Current defaults are deterministic balanced runtime startup and default-class,
default-stack task spawn.

Public option structs store enum-valued fields and flag words as `uint32_t`,
not C enum or `unsigned` objects. The enum types and constants remain available
for C source readability, but the struct layout does not depend on
compiler-specific enum width choices or non-fixed integer aliases.
`llam_spawn_opts_t::reserved0` explicitly occupies the former padding between
`flags` and `deadline_ns`. Initializers zero it, the current library ignores it,
and callers should leave it at zero so future tail-compatible bindings do not
inherit indeterminate padding bits as semantic input.

Experimental runtime toggles are grouped under
`llam_runtime_opts_t::experimental_flags`, a `uint64_t` bitset of
`LLAM_RUNTIME_EXPERIMENTAL_F_*` macros. The constants are defined with
`UINT64_C(...)` rather than C enum storage so future high-bit flags do not
depend on compiler enum width. This keeps unstable tuning switches inside one
ABI-stable field instead of freezing one public struct member per experiment.

## Outbound Stats Structs

Runtime statistics use an explicit output size:

```c
int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats,
                                  size_t stats_size);
```

When `stats` is not `NULL`, `stats_size` must be nonzero. LLAM zeroes the
caller-provided prefix and copies only the overlapping prefix of the current
library's `llam_runtime_stats_t`. This lets older FFI bindings pass smaller
stats structs to newer libraries without buffer overflow when fields are
appended at the tail. `llam_runtime_collect_stats()` is a C convenience wrapper
that passes `sizeof(llam_runtime_stats_t)`.

`llam_runtime_write_stats_json(fd)` writes the same canonical LLAM counter names
as a newline-terminated JSON object. The schema is additive: consumers must
ignore unknown fields and should not require a fixed field order.

All scalar control fields in public option structs and all 32-bit counter/state
fields in public stats structs use fixed-width integer storage. Language
bindings should model these fields as unsigned 32-bit integers, except
`sqpoll_cpu`, which is a signed 32-bit integer where `-1` means automatic CPU
selection.

## Enum-Valued Function ABI

Enum-valued public functions use `uint32_t` at the ABI boundary:

```c
int llam_task_set_class(uint32_t task_class);
uint32_t llam_task_class(const llam_task_t *task);
uint32_t llam_task_flags(const llam_task_t *task);
```

Callers should pass one of the `llam_task_class_t` constants to
`llam_task_set_class()` and interpret `llam_task_class()` as a
`llam_task_class_t` value. The fixed-width signature avoids depending on the C
compiler's enum representation. `llam_task_set_class()` returns `-1/EINVAL`
for unknown class values and `-1/ENOTSUP` outside a managed task.

## Threading And Ownership Rules

- `llam_runtime_init_ex()`, `llam_runtime_init()`, and
  `llam_runtime_shutdown()` are not reentrant.
- `llam_runtime_create()`, `llam_runtime_spawn_ex()`,
  `llam_runtime_run_handle()`, and `llam_runtime_destroy()` are the canonical
  embedding-facing lifecycle calls. They operate on independent runtime handles;
  legacy host-thread lifecycle wrappers operate on `llam_runtime_default()`.
  Managed task calls to legacy spawn/stop/shutdown wrappers target the current
  task's owner runtime so explicit-runtime tasks do not accidentally poke the
  default runtime.
- Calling `llam_runtime_shutdown()` from a managed LLAM task or scheduler frame
  does not tear down the owning runtime underneath the active scheduler. It is
  treated as a cooperative stop request; the host thread that owns the run call
  remains responsible for final shutdown.
- Calling `llam_runtime_destroy(runtime)` from a managed task only affects that
  task's owner runtime. A foreign runtime handle is ignored so one scheduler
  cannot accidentally become a stop signal for a peer scheduler.
- A runtime handle must not be destroyed while another host thread is
  concurrently creating tasks or runtime-owned synchronization objects for that
  same handle.
- User pointers passed to tasks, channels, blocking callbacks, or I/O buffers
  remain owned by the caller unless a specific API states otherwise.
- Task, channel, mutex, condition, cancellation, and buffer handles must not be
  copied by value or stack-allocated by callers.
- A cancellation token must outlive all tasks, waits, and I/O operations that
  observe it. Internally retained observers keep a refcount; destroy fails with
  `EBUSY` until those observers are gone.
- Mutex, condition, and channel destroy functions require external
  synchronization: no task may own, wait on, or concurrently access the object
  being destroyed.
- Host-side destroy/release calls for idle public objects may be performed after
  the owner runtime has been destroyed.  Managed tasks still cannot use or
  destroy objects owned by a different live runtime; those calls fail with
  `EXDEV`.
- LLAM may move managed tasks between workers unless the caller supplies a
  documented placement hint. Hints are not ABI guarantees.
- Language runtimes should treat all undocumented exported symbols as private
  implementation details.

## Deadline And Cancellation Rules

- Deadline values are absolute monotonic timestamps in `llam_now_ns()` units.
- All public `*_until(deadline_ns)` APIs treat `deadline_ns == 0` as an
  already-expired absolute deadline, not "disabled".
- Expired deadline waits fail with `errno = ETIMEDOUT`.
- Cancellation-aware operations fail with `errno = ECANCELED` where the public
  header documents cancellation token participation.
- Immediate fast paths are allowed to complete before observing cancellation if
  the operation had already become ready.
- LLAM preserves syscall-style `errno` values for I/O operations and does not
  translate them into backend-specific domains.

## Shared Library Names

CMake builds both a static target and a shared target:

```text
llam_runtime         static library target
llam_runtime_shared  shared library target
```

The Makefile builds the shared library through:

```bash
make shared
```

Expected dynamic artifacts:

```text
Linux:  libllam_runtime.so -> libllam_runtime.so.2 -> libllam_runtime.so.2.0.0
macOS:  libllam_runtime.dylib -> libllam_runtime.2.dylib
Windows: llam_runtime.dll plus llam_runtime.lib and llam_runtime_shared.lib in
         the native Windows x86_64 release archive.
```

Language runtimes should prefer the ABI-major soname/install-name when a
platform supports it.

Native Windows 10/11 support covers Windows x86_64 context switching, Windows
event wake handles, IOCP policy primitives, and IOCP one-shot socket requests
for `read`, `write`, `accept`, and `connect`, plus generic HANDLE `ReadFile`
and `WriteFile` requests through `llam_read_handle()`, `llam_write_handle()`,
`llam_pread_handle()`, and `llam_pwrite_handle()`. TCP `POLLOUT` and UDP `POLLIN` readiness are also
IOCP-backed; TCP `POLLIN` remains fallback by default unless
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` is enabled for controlled smoke or benchmark
runs. The release workflow publishes a native Windows x86_64 archive after the
Windows stress, shared-library export, IOCP smoke, and HANDLE I/O gates pass.

## Runtime Lifecycle

`llam_runtime_init_ex()` initializes one process-wide runtime instance with an
explicit option size. `llam_runtime_init()` is a C convenience wrapper. One of
them must complete successfully before managed tasks or runtime-aware
synchronization objects are used.

`llam_run()` drives scheduled work until all live tasks complete, the runtime
requests stop, or a fatal runtime error is observed. A cooperative stop request
is a successful scheduler outcome: `llam_run()` returns `0` after the runtime
observes `llam_runtime_request_stop()` and all live tasks have exited or become
irrelevant to the drained runtime. Backend or fatal runtime failures return
`-1` with `errno` set.

`llam_runtime_request_stop()` requests cooperative stop and wakes scheduler,
I/O, and blocking workers. It does not forcibly kill live tasks; `llam_run()`
still waits for live tasks to exit or observe cancellation through user code.
Unmanaged callers target the process-default runtime. Managed callers target
their current task's owner runtime.

`llam_runtime_shutdown()` is idempotent and may be called after a failed or
partial initialization. It requests cooperative stop, joins runtime-owned OS
threads that were started, and releases backend resources. It is not a task-join
API: callers that need a graceful task drain should call
`llam_runtime_request_stop()`, drive `llam_run()`, and then call shutdown. After
shutdown, task handles are no longer joinable, blocking runtime-aware operations
that require a managed task fail by contract, and backend I/O ownership is gone.
Public cleanup handles keep the exceptions documented by their APIs: owned
buffers remain valid until `llam_io_buffer_release()`, and already-buffered
channel values may be drained with `llam_channel_try_recv_result()` before
destroy.

From a managed LLAM task or scheduler frame, shutdown performs only the
stop-request portion. This avoids freeing scheduler, stack, or I/O state while
it is still active on the current call stack.

## Task Semantics

`llam_spawn_ex()` creates a stackful managed task with an explicit option size
and makes it runnable. `llam_spawn()` is a C convenience wrapper. The task entry
receives the user pointer exactly once. Returning from the entry function
completes the task and wakes join waiters. If `llam_spawn_opts_t` supplies a
cancellation token, that token must belong to the runtime that will own the new
task. Cross-runtime tokens are rejected with `EXDEV` before the task is
published.

`llam_join()` waits for task completion. Managed LLAM task callers park
cooperatively. Unmanaged OS-thread callers block the calling thread and do not
drive scheduler progress by themselves; another thread must call `llam_run()` or
the target must already be able to complete. `llam_join_until()` uses an
absolute deadline in `llam_now_ns()` units. Passing deadline `0` is an already
expired absolute deadline, not "disabled". Timeout failures return `-1` with
`errno` set to `ETIMEDOUT`; after timeout, the task remains joinable by the same
handle.

A task handle returned by `llam_spawn*()` must be completed by exactly one of
`llam_join()`, `llam_join_until()` success, or `llam_detach()`. A successful
join or detach consumes the task handle. The caller must not use the handle
again, and double completion is invalid. A concurrent second join attempt fails
with `errno = EBUSY` before the handle is consumed. `llam_detach()` fails with
`errno = EBUSY` if a join waiter already owns completion. Detached tasks still
run to completion and remain counted as live runtime work until their entry
function returns.

Task groups own the child task handles spawned through the group. Group
cancellation cancels only the group's automatically attached cancellation token;
children spawned with an explicit caller-owned token remain controlled by that
token. Destroying a task group with live children, active spawn/cancel
operations, or an in-progress join fails with `EBUSY`. Calls racing with a
completed group destroy fail with `EINVAL` instead of dereferencing reclaimed
group storage. Child tasks are spawned on the group's owner runtime, including
host-thread calls that are not currently running inside a managed task.

`llam_yield()` is cooperative. It never transfers ownership of user memory and
does not imply a memory fence beyond the synchronization performed by the
runtime scheduler. `llam_task_safepoint()` is the lower-overhead progress hook
for CPU-bound loops; it is a no-op outside a managed task and may yield only
when runtime policy or preemption requests require it.

`LLAM_PREEMPT_POLL()` and `LLAM_PREEMPT_POLL_EVERY(counter, interval)` are the
public hot-loop helpers for request-based automatic preemption. They do not let
the runtime asynchronously switch stacks at arbitrary instruction boundaries;
they provide explicit LLAM boundaries where an already requested preemption can
be honored. `LLAM_PREEMPT_POLL_EVERY` evaluates its arguments once, treats
`interval` values `0` and `1` as "poll every call", and is safe to use in tight
loops where a full `llam_yield()` would be too expensive.

## Synchronization Semantics

LLAM mutexes, condition variables, channels, and cancellation tokens are
runtime-aware. When called from a managed task, blocking waits park the task
instead of blocking the scheduler worker.

Outside-runtime behavior is fixed as follows:

- `llam_yield()` outside a managed task is a no-op.
- `llam_task_safepoint()` outside a managed task is a no-op.
- `llam_sleep_ns()` and `llam_sleep_until()` block the calling OS thread after
  runtime initialization; before initialization they fail with `EINVAL`.
- `llam_call_blocking_result()` runs the callback synchronously outside a
  managed task.
- `llam_enter_blocking()` and `llam_leave_blocking()` outside a managed task
  are no-ops and return `0`.
- Runtime mutex, condition, and channel blocking operations require a managed
  task context and fail with `ENOTSUP` outside one.
- Nonblocking channel operations (`llam_channel_try_send()` and
  `llam_channel_try_recv_result()`) may be called from unmanaged OS threads
  after runtime initialization because they never park. This is the supported
  host-thread cleanup path for draining buffered channel values before destroy.
  `llam_channel_try_recv_result()` can also drain already-buffered values after
  runtime shutdown; it never wakes parked senders without a live runtime. A
  nonblocking operation that would park fails with `errno = EAGAIN`, not
  `ETIMEDOUT`.
- Runtime I/O helpers document their own direct syscall fallback below.

Language runtimes should perform blocking coordination from LLAM tasks whenever
possible.

LLAM mutexes are non-recursive. `llam_mutex_lock()` and
`llam_mutex_lock_until()` return `-1/EDEADLK` when the current managed task
already owns the mutex. Mutex try-lock returns `-1/EBUSY` when the mutex is
already locked. Mutex unlock returns `-1/EPERM` when the current managed task
does not own the mutex.

Condition waits follow condition-variable style rules. The caller must own the
associated mutex on entry. LLAM atomically releases it while waiting and
reacquires it before returning, including signal, broadcast, timeout,
cancellation, and spurious wake paths. Callers must wait in a predicate loop
because wakeups are not a predicate guarantee. Invalid-argument or wrong-owner
failures return before releasing the mutex.

`llam_cond_signal()` and `llam_cond_broadcast()` may be called with or without
the associated mutex held, and may be called outside a managed LLAM task. Invalid
condition handles fail with `-1/EINVAL`.

Channel receive APIs come in two forms:

```c
void *llam_channel_recv(llam_channel_t *channel);
int llam_channel_recv_result(llam_channel_t *channel, void **out);
```

The pointer-return convenience APIs are compact but ambiguous when `NULL` is a
valid payload. FFI bindings and code that sends `NULL` should use
`llam_channel_recv_result()` or `llam_channel_recv_until_result()`, where `0`
means a value was received and `*out` may legitimately be `NULL`.

Channel close rules are fixed:

- `llam_channel_close()` is idempotent.
- Sends after close fail with `EPIPE`.
- Buffered values sent before close remain drainable.
- Receives fail with `EPIPE` only after the buffer is empty and no sender can
  satisfy the receive.
- `NULL` payloads are valid; use `_result` receive APIs when they matter.
- Timed channel operations treat deadline `0` as an already-expired absolute
  deadline.

## I/O Semantics

Runtime I/O helpers provide scheduler-safe wrappers for POSIX-style descriptors
or platform socket handles:

```c
ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);
ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);
ssize_t llam_read_handle(llam_handle_t handle, void *buf, size_t count);
ssize_t llam_write_handle(llam_handle_t handle, const void *buf, size_t count);
int llam_close(llam_fd_t fd);
llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);
int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);
int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);
int llam_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);
```

When called outside a managed task, these functions delegate to the platform
syscall or polling primitive directly. `llam_poll_fd()` and
`llam_poll_handle()` may block the calling OS thread according to their
`timeout_ms` argument. `timeout_ms < 0` waits indefinitely, `timeout_ms == 0`
performs a non-blocking readiness check, and positive values bound the wait in
milliseconds.

When called inside a managed task, LLAM uses this order:

1. Complete immediately when a direct nonblocking fast path is safe.
2. Submit to the platform backend when supported.
3. Fall back to a blocking helper so the scheduler worker is not pinned.

`llam_close()` is the fd/socket close boundary. It returns `0` on success and
`-1` with `errno` set on failure; `LLAM_INVALID_FD` fails with `EBADF`. Managed
callers invalidate runtime-local descriptor caches before the platform close.
Raw platform close remains source-compatible, but it can hide fd reuse from
runtime diagnostics until a later backend completion observes the stale handle.

`llam_connect()` follows `connect(2)` result semantics:

- Returns `0` when the socket is connected.
- Returns `-1` and sets `errno` to the connection failure.
- Treats `EINPROGRESS`, `EALREADY`, and would-block states as pending work.
- Resolves writable readiness with `SO_ERROR`.
- Never reports success from readiness alone.

`llam_accept()` returns `LLAM_INVALID_FD` on failure with `errno` set. On
success, it transfers ownership of the accepted descriptor to the caller. The
caller is responsible for closing it, preferably with `llam_close()` after the
descriptor has been used with LLAM I/O. Use `LLAM_FD_IS_INVALID(fd)` when
testing returned descriptors, especially from Windows bindings where
`llam_fd_t` maps to `SOCKET`.

Owned-buffer read APIs transfer buffer ownership to the caller on success. The
caller must release the buffer with `llam_io_buffer_release()`. The returned
wrapper and payload remain valid until release even if the runtime that produced
the buffer is shut down first.

Owned-buffer result rules are fixed for FFI bindings:

- Positive byte count: `*out` is non-NULL and owned by the caller.
- EOF or zero-byte read/receive: returns `0` and stores `NULL` in `*out`.
- Failure: returns `-1`, stores `NULL` in `*out`, and sets `errno`.

On Windows, one-shot socket `read`, `write`, `accept`, and `connect` use IOCP
request completions. TCP `POLLOUT` readiness uses a zero-byte overlapped send,
and UDP `POLLIN` readiness uses an overlapped peek receive so datagrams are not
consumed. TCP `POLLIN` remains on the cooperative/direct fallback path by
default; `LLAM_WINDOWS_IOCP_TCP_POLLIN=1` enables the experimental IOCP stream
readiness probe for controlled validation. Generic HANDLE read/write uses
overlapped `ReadFile`/`WriteFile` through IOCP when the handle supports it;
otherwise LLAM falls back to blocking offload. `llam_poll_handle()` waits on
waitable HANDLE state and does not replace socket readiness polling.
Owned-buffer and unsupported poll paths keep the documented direct/blocking
fallback behavior.

## Error Contract

Unless explicitly documented otherwise:

- `0` means success for integer status APIs.
- Positive byte counts or descriptor values follow the corresponding syscall.
- `-1` means failure and `errno` contains the reason.
- `NULL` means allocation, cancellation, or submission failure and `errno`
  disambiguates where possible.
- Managed tasks have task-local `errno` at LLAM fiber context-switch
  boundaries. When a task yields, parks, sleeps, joins, waits on I/O, or exits,
  LLAM snapshots its current `errno`; when that task resumes, LLAM restores the
  snapshot before user code continues. This prevents another task or scheduler
  worker thread from leaking its TLS `errno` into the resumed task.
- External threads that are not running inside a LLAM task continue to use the
  platform thread-local `errno` directly.
- Callers should still inspect `errno` only after APIs that signal failure,
  except for APIs that explicitly forward callback/syscall errno state.
Common public failures are fixed as follows:

| errno | Meaning |
| --- | --- |
| `EINVAL` | Invalid argument, zero-sized inbound option struct, invalid enum/policy value, or invalid consumed handle use. |
| `EAGAIN` | Nonblocking operation would need to park or wait. |
| `EBUSY` | Runtime already initialized/running, concurrent second task join before handle consumption, detach while a join waiter owns completion, or destroy while observers remain. |
| `ETIMEDOUT` | Absolute deadline or relative timeout expired. |
| `ECANCELED` | Cancellation token or cooperative runtime stop was observed by a cancellable operation. |
| `EPIPE` | Channel is closed or an operation reached an equivalent peer-closed state. |
| `EXDEV` | Runtime-aware object was used from a different runtime owner. Explicit runtimes are isolated owner domains; cross-runtime managed use fails deterministically. |
| `ENOMEM` | Allocation failure. |
| `ENOTSUP` | Platform/backend feature is unsupported, or a runtime-aware operation requiring a managed task was called outside one. |

Default 2.x source and release builds keep runtime-owner checks enabled so
`EXDEV` remains observable. Projects that build LLAM from source can define
`LLAM_RUNTIME_DISABLE_OWNER_CHECKS=1` only for explicit singleton-only profiling
builds; those binaries intentionally give up cross-owner diagnostics and should
not be used as ABI-conformance references.

LLAM preserves syscall-style error values for I/O operations so language
runtimes can map them into their own error domains without backend-specific
special cases.
