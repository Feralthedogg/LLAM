# LLAM ABI And Semantic Contract

This document defines the public ABI and runtime semantics that language
implementations may rely on when loading LLAM dynamically.

## ABI Stability

LLAM exposes one stable public ABI family:

- Canonical API: symbols declared in `include/llam/runtime.h`.
- Legacy compatibility API: symbols declared in `include/llam/nm_runtime.h`.

All other non-static implementation symbols are private even if a platform
linker happens to export them. Language runtimes must bind only documented
public symbols.

The current public ABI version is:

```text
LLAM_ABI_VERSION_MAJOR = 1
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
Linux:  libllam_runtime.so -> libllam_runtime.so.1 -> libllam_runtime.so.1.0.0
macOS:  libllam_runtime.dylib -> libllam_runtime.1.dylib
```

Language runtimes should prefer the ABI-major soname/install-name when a
platform supports it.

## Runtime Lifecycle

`llam_runtime_init()` initializes one process-wide runtime instance. It must
complete successfully before managed tasks or runtime-aware synchronization
objects are used.

`llam_run()` drives scheduled work until all live tasks complete, the runtime
requests stop, or a fatal runtime error is observed.

`llam_runtime_shutdown()` stops workers and releases process-wide resources.
After shutdown, existing task, channel, mutex, condition, cancellation-token,
and I/O-buffer handles are invalid unless a function explicitly documents
otherwise.

## Task Semantics

`llam_spawn()` creates a stackful managed task and makes it runnable. The task
entry receives the user pointer exactly once. Returning from the entry function
completes the task and wakes join waiters.

`llam_join()` waits for task completion. `llam_join_until()` uses an absolute
deadline in `llam_now_ns()` units. Timeout failures return `-1` with `errno`
set to `ETIMEDOUT`.

`llam_yield()` is cooperative. It never transfers ownership of user memory and
does not imply a memory fence beyond the synchronization performed by the
runtime scheduler.

## Synchronization Semantics

LLAM mutexes, condition variables, channels, and cancellation tokens are
runtime-aware. When called from a managed task, blocking waits park the task
instead of blocking the scheduler worker.

When called outside a managed task, behavior is valid only for APIs that
explicitly document non-runtime behavior. Language runtimes should perform
blocking coordination from LLAM tasks whenever possible.

## I/O Semantics

Runtime I/O helpers provide scheduler-safe wrappers for POSIX-style descriptors
or platform socket handles:

```c
ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);
ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);
llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);
int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);
int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);
```

When called outside a managed task, these functions delegate to the platform
syscall or polling primitive directly.

When called inside a managed task, LLAM uses this order:

1. Complete immediately when a direct nonblocking fast path is safe.
2. Submit to the platform backend when supported.
3. Fall back to a blocking helper so the scheduler worker is not pinned.

`llam_connect()` follows `connect(2)` result semantics:

- Returns `0` when the socket is connected.
- Returns `-1` and sets `errno` to the connection failure.
- Treats `EINPROGRESS`, `EALREADY`, and would-block states as pending work.
- Resolves writable readiness with `SO_ERROR`.
- Never reports success from readiness alone.

`llam_accept()` transfers ownership of the accepted descriptor to the caller on
success. The caller is responsible for closing it.

Owned-buffer read APIs transfer buffer ownership to the caller on success. The
caller must release the buffer with `llam_io_buffer_release()`.

## Error Contract

Unless explicitly documented otherwise:

- `0` means success for integer status APIs.
- Positive byte counts or descriptor values follow the corresponding syscall.
- `-1` means failure and `errno` contains the reason.
- `NULL` means allocation, cancellation, or submission failure and `errno`
  disambiguates where possible.

LLAM preserves syscall-style error values for I/O operations so language
runtimes can map them into their own error domains without backend-specific
special cases.
