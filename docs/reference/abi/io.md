# I/O semantics

The exact behavior of I/O functions, including partial transfers, deadlines, and
fallbacks.

## Runtime I/O helpers

Runtime I/O helpers provide scheduler-safe wrappers for POSIX-style descriptors
or platform socket handles:

```c
ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);
ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);
ssize_t llam_read_handle(llam_handle_t handle, void *buf, size_t count);
ssize_t llam_write_handle(llam_handle_t handle, const void *buf, size_t count);
ssize_t llam_recvfrom(llam_fd_t fd, void *buf, size_t count, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t llam_sendto(llam_fd_t fd, const void *buf, size_t count, int flags,
                    const struct sockaddr *dst_addr, socklen_t addrlen);
ssize_t llam_recvfrom_owned(llam_fd_t fd, size_t max_count, int flags,
                            struct sockaddr *src_addr, socklen_t *addrlen,
                            llam_io_buffer_t **out);
int llam_close(llam_fd_t fd);
llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);
int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);
int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);
int llam_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);
```

## Inside and outside a managed task

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

## Close

`llam_close()` is the fd/socket close boundary. It returns `0` on success and
`-1` with `errno` set on failure; `LLAM_INVALID_FD` fails with `EBADF`. Managed
callers invalidate runtime-local descriptor caches before the platform close.
Raw platform close remains source-compatible, but it can hide fd reuse from
runtime diagnostics until a later backend completion observes the stale handle.

## Connect

`llam_connect()` follows `connect(2)` result semantics:

- Returns `0` when the socket is connected.
- Returns `-1` and sets `errno` to the connection failure.
- Treats `EINPROGRESS`, `EALREADY`, and would-block states as pending work.
- Resolves writable readiness with `SO_ERROR`.
- Never reports success from readiness alone.

## Accept

`llam_accept()` returns `LLAM_INVALID_FD` on failure with `errno` set. On
success, it transfers ownership of the accepted descriptor to the caller. The
caller is responsible for closing it, preferably with `llam_close()` after the
descriptor has been used with LLAM I/O. Use `LLAM_FD_IS_INVALID(fd)` when
testing returned descriptors, especially from Windows bindings where
`llam_fd_t` maps to `SOCKET`.

## Owned buffers

Owned-buffer read APIs transfer buffer ownership to the caller on success. The
caller must release the buffer with `llam_io_buffer_release()`. The returned
wrapper and payload remain valid until release even if the runtime that produced
the buffer is shut down first. The data pointer returned by
`llam_io_buffer_data()` is borrowed from the owned buffer; serialize
`llam_io_buffer_release()` against any raw pointer use if a buffer is shared
between host threads.

Owned-buffer result rules are fixed for FFI bindings:

- Positive byte count: `*out` is non-NULL and owned by the caller.
- EOF or zero-byte read/receive: returns `0` and stores `NULL` in `*out`.
- Failure: returns `-1`, stores `NULL` in `*out`, and sets `errno`.

## Datagrams

Datagram APIs follow syscall-style `recvfrom(2)` and `sendto(2)` semantics.
Managed tasks park on descriptor readiness before retrying, so a blocking UDP
socket does not pin a scheduler worker. `llam_recvfrom_owned()` follows the
same owned-buffer result rules as `llam_read_owned()` and `llam_recv_owned()`;
a zero-length datagram returns `0` with `*out == NULL` by contract.

## Blocking syscall wrappers

Blocking syscall wrappers cover libc/FS operations that are not reliably native
async syscalls:

```c
int llam_getaddrinfo_result(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **out, int *gai_error);
void llam_freeaddrinfo_result(struct addrinfo *result);
int llam_open_async(const char *path, int flags, uint32_t mode,
                    llam_handle_t *out);
int llam_stat_path_ex(const char *path, llam_file_stat_t *out,
                      size_t out_size);
```

Managed tasks route these calls through the blocking-worker path; unmanaged
host threads execute the same wrapper synchronously. `llam_getaddrinfo_result`
distinguishes LLAM submission/cancellation failure (`-1` + `errno`) from normal
resolver failure (`0`, `*gai_error = EAI_*`, `*out = NULL`).

## Windows

On Windows, one-shot socket `read`, `write`, `accept`, and `connect` use IOCP
request completions. TCP `POLLOUT` readiness uses a zero-byte overlapped send.
TCP and UDP `POLLIN` remain on the cooperative/direct fallback path by default.
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` enables the experimental IOCP stream readiness
probe, and `LLAM_WINDOWS_IOCP_UDP_POLLIN=1` enables an overlapped peek receive
that does not consume datagrams, both for controlled validation. Generic HANDLE read/write uses
overlapped `ReadFile`/`WriteFile` through IOCP when the handle supports it;
otherwise LLAM falls back to blocking offload. `llam_poll_handle()` waits on
waitable HANDLE state and does not replace socket readiness polling.
Owned-buffer and unsupported poll paths keep the documented direct/blocking
fallback behavior.
