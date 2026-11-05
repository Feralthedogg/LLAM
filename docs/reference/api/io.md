# I/O

Read and write streams and HANDLEs, work with sockets and DNS, use owned buffers,
and call filesystem wrappers. These calls look blocking from task code, but
managed tasks park while the backend waits.

## Stream and handle I/O

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

## Deadlines and wait options

`llam_wait_opts_t` bounds one operation with an absolute deadline and an extra
cancellation token. The `*_until` forms take only the deadline, and `0` there means
already expired. New poll forms take an `int` mask in the short-mask range; the older
poll ABI is unchanged. Poll timeout returns 0; byte and connect timeout returns
-1/`ETIMEDOUT`. See the [wait option fields](../options/runtime.md#operation-wait-options)
and [Embed LLAM](../../guides/embedding.md#compose-cancellation-and-budgets) for
endpoint restrictions.

| API | Purpose |
| --- | --- |
| `llam_wait_opts_init` | Fill wait options with ABI-safe defaults. |
| `llam_read_ex` / `llam_write_ex` | Stream I/O with a wait policy. |
| `llam_read_handle_ex` / `llam_write_handle_ex` | Handle I/O with a wait policy. |
| `llam_connect_ex` | Connect with a wait policy. |
| `llam_poll_fd_ex` | Wait for readiness with a wait policy. |
| `llam_read_until` / `llam_write_until` | Stream I/O until an absolute deadline. |
| `llam_connect_until` | Connect until an absolute deadline. |
| `llam_poll_fd_until` | Wait for readiness until an absolute deadline. |

## Exact transfers

These calls loop until the whole count has moved and report confirmed progress
through a required `size_t *`, including on failure. Early EOF is `EPIPE`; a premature
zero-byte write is `EIO`.

| API | Purpose |
| --- | --- |
| `llam_read_exact` / `llam_write_all` | Transfer exactly the requested bytes. |
| `llam_read_exact_ex` / `llam_write_all_ex` | Transfer exactly the requested bytes with a wait policy. |

## Datagram, socket, and DNS

| API | Purpose |
| --- | --- |
| `llam_recvfrom` / `llam_sendto` | Datagram I/O with peer addresses. |
| `llam_recv_owned` | Socket receive into a runtime-owned buffer, with `recv` flags such as `MSG_PEEK`. Works on stream and datagram sockets. |
| `llam_recvfrom_owned` | Datagram receive into a runtime-owned buffer, also returning the peer address. |
| `llam_accept` | Accept a socket connection with scheduler-safe waiting. |
| `llam_connect` | Connect a socket with scheduler-safe waiting. |
| `llam_poll_fd` | Wait for descriptor/socket readiness. |
| `llam_poll_handle` | Wait for generic handle readiness where supported. |
| `llam_getaddrinfo_result` | DNS through the runtime blocking path. |
| `llam_freeaddrinfo_result` | Release DNS results returned by `llam_getaddrinfo_result`. |

## Owned I/O buffers

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

## Filesystem wrappers

| API | Purpose |
| --- | --- |
| `llam_open_async` | Open a filesystem path through the blocking path. |
| `llam_stat_path_ex` | Fill portable file metadata into `llam_file_stat_t`. |
| `LLAM_FILE_TYPE_*` | Portable file type values used by `llam_file_stat_t`. |
