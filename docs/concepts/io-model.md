# I/O Model

LLAM I/O calls look blocking from task code, but managed tasks park
cooperatively while the platform backend waits.

## Backend Selection

| Platform | Backend |
| --- | --- |
| Linux | io_uring/liburing |
| macOS | kqueue |
| FreeBSD, OpenBSD, NetBSD, DragonFly BSD | kqueue |
| Windows | IOCP for Winsock and overlapped HANDLE operations |

Unsupported descriptors or policies fall back to the blocking-helper path so a
scheduler worker is not pinned.

## Request Flow

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

## Owned Buffers

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

Borrowed pointers from `llam_io_buffer_data()` are valid only until release.

## Windows HANDLE I/O

On Windows, file I/O should use HANDLE variants:

```c
ssize_t n = llam_pread_handle(handle, buf, sizeof(buf), offset);
```

The POSIX-style `llam_pread()` and `llam_pwrite()` fd/socket variants return
`ENOTSUP` on Windows.
