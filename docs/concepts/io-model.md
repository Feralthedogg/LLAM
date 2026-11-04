# I/O model

LLAM I/O calls look blocking from task code, but managed tasks park
cooperatively while the platform backend waits.

## Backend selection

| Platform | Backend |
| --- | --- |
| Linux | io_uring/liburing |
| macOS | kqueue |
| Windows | IOCP for Winsock and overlapped HANDLE operations |

Unsupported descriptors or policies fall back to the blocking-helper path so a
scheduler worker is not pinned.

## Request flow

```text
direct nonblocking probe
  -> cooperative handoff when useful
  -> backend submit or readiness watch
  -> blocking-helper fallback when unsupported
  -> owner task wake
```

Use `llam_close()` or `llam_close_handle()` for descriptors and handles that
have been used with LLAM I/O. This lets runtime-local descriptor state observe
the close boundary.

## Stream I/O

```c
ssize_t n = llam_read(fd, buf, sizeof(buf));
ssize_t m = llam_write(fd, data, len);
```

`llam_read_when_ready()` combines readiness waiting and the read attempt for
common producer/consumer cases.

A stream read or write may transfer only a prefix. Use `llam_read_exact()` or
`llam_write_all()` when the application needs the whole buffer, and retain the
reported progress if a later transfer fails. The `*_ex` forms accept common
absolute-deadline and cancellation options; bounded byte I/O requires a managed task
and a supported socket, nonblocking, or overlapped I/O path.

Use `LLAM_INVALID_FD` or `LLAM_FD_IS_INVALID(fd)` for descriptor-returning failures
such as `llam_accept()`, and `LLAM_INVALID_HANDLE` or `LLAM_HANDLE_IS_INVALID(handle)`
for HANDLE-returning integrations.

## Datagram I/O

Use datagram APIs when peer address metadata matters:

```c
struct sockaddr_storage peer;
socklen_t peer_len = sizeof(peer);

ssize_t n = llam_recvfrom(fd,
                          buf,
                          sizeof(buf),
                          0,
                          (struct sockaddr *)&peer,
                          &peer_len);
```

`llam_recvfrom_owned()` receives into runtime-owned storage and returns peer
address information.

## Owned buffers

Owned buffers are allocated by the runtime and released by the caller:

```c
llam_io_buffer_t *buffer = NULL;
ssize_t n = llam_read_owned(fd, 4096, &buffer);

if (n > 0) {
    void *data = llam_io_buffer_data(buffer);
    size_t size = llam_io_buffer_size(buffer);
    (void)data;
    (void)size;
}

llam_io_buffer_release(buffer);
```

EOF or a zero-byte read returns `0` with `buffer == NULL`. Failures return `-1`, set
`errno`, and also leave `buffer == NULL`.

Borrowed pointers from `llam_io_buffer_data()` are valid only until release. If
another host thread can release the buffer, copy or serialize access before using the
raw pointer.

## Positional and direct I/O

For database and storage workloads, use positional I/O so the current file offset is
not shared between tasks. Direct-I/O callers can request sector- or page-aligned owned
buffers and pass externally opened `O_DIRECT` files or Windows overlapped file
HANDLEs.

```c
llam_io_buffer_t *buffer = NULL;
ssize_t n = llam_pread_owned_aligned(fd, 4096, 8192, 4096, &buffer);

if (n > 0) {
    /* llam_io_buffer_data(buffer) is aligned to at least 4096 bytes. */
}
llam_io_buffer_release(buffer);
```

## Windows HANDLE I/O

On Windows, file I/O should use HANDLE variants:

```c
ssize_t n = llam_pread_handle(handle, buf, sizeof(buf), offset);
```

The POSIX-style `llam_pread()` and `llam_pwrite()` fd/socket variants return
`ENOTSUP` on Windows.
