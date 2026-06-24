# Runtime I/O

Runtime I/O lets task code look blocking while LLAM parks the task on the
platform backend.

## POSIX Socketpair Echo

This example is POSIX-only because it uses `socketpair()`.

```c
#include <llam/runtime.h>

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

typedef struct echo_state {
    llam_fd_t reader;
    llam_fd_t writer;
} echo_state_t;

static void reader_task(void *arg) {
    echo_state_t *state = arg;
    char buf[64];

    ssize_t n = llam_read(state->reader, buf, sizeof(buf));
    if (n > 0) {
        printf("read=%.*s\n", (int)n, buf);
    }
}

static void writer_task(void *arg) {
    echo_state_t *state = arg;
    const char *msg = "hello";

    (void)llam_write(state->writer, msg, strlen(msg));
}

static void root(void *arg) {
    (void)arg;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return;
    }

    echo_state_t state = {
        .reader = sv[0],
        .writer = sv[1],
    };

    llam_task_t *reader = llam_spawn(reader_task, &state, NULL);
    llam_task_t *writer = llam_spawn(writer_task, &state, NULL);

    if (reader != NULL) {
        (void)llam_join(reader);
    }
    if (writer != NULL) {
        (void)llam_join(writer);
    }

    (void)llam_close(sv[0]);
    (void)llam_close(sv[1]);
}
```

## Owned Buffer Read

Use owned buffers when the runtime should allocate storage:

```c
llam_io_buffer_t *buffer = NULL;
ssize_t n = llam_read_owned(fd, 4096, &buffer);

if (n > 0 && buffer != NULL) {
    void *data = llam_io_buffer_data(buffer);
    size_t size = llam_io_buffer_size(buffer);
    consume_bytes(data, size);
}

llam_io_buffer_release(buffer);
```

`llam_io_buffer_data()` returns a borrowed pointer. It is valid only until
`llam_io_buffer_release()`.

## Datagram Receive

```c
struct sockaddr_storage peer;
socklen_t peer_len = sizeof(peer);
char packet[1500];

ssize_t n = llam_recvfrom(fd,
                          packet,
                          sizeof(packet),
                          0,
                          (struct sockaddr *)&peer,
                          &peer_len);
```

Use `llam_recvfrom_owned()` when you also want runtime-owned packet storage.

## Windows HANDLE File I/O

On Windows, use HANDLE variants for file I/O:

```c
ssize_t n = llam_pread_handle(handle, buf, sizeof(buf), offset);
```

The POSIX-style `llam_pread()` and `llam_pwrite()` fd/socket variants return
`ENOTSUP` on Windows.
