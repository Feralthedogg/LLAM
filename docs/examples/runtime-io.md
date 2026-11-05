# Runtime I/O

Runtime I/O lets task code look blocking while LLAM parks the task on the
platform backend.

## POSIX socketpair echo

**Complete program, POSIX only.** It uses `socketpair()`, so it does not build on
Windows.

```c title="main.c"
#include <llam/runtime.h>

#include <errno.h>
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
    (void)llam_task_set_result(NULL, n < 0 ? errno : 0);
}

static void writer_task(void *arg) {
    echo_state_t *state = arg;
    const char *msg = "hello";

    ssize_t n = llam_write(state->writer, msg, strlen(msg));
    (void)llam_task_set_result(NULL, n < 0 ? errno : 0);
}

static int join_error(llam_task_t *task) {
    llam_task_result_t result;

    return llam_join_result(task, &result, sizeof(result)) == 0 ? result.error : errno;
}

static void root(void *arg) {
    echo_state_t *state = arg;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        (void)llam_task_set_result(NULL, errno);
        return;
    }
    state->reader = sv[0];
    state->writer = sv[1];

    int error = 0;
    llam_task_t *reader = llam_spawn(reader_task, state, NULL);
    llam_task_t *writer = reader != NULL ? llam_spawn(writer_task, state, NULL) : NULL;

    if (writer != NULL) {
        error = join_error(writer);
    } else {
        error = errno;
        /* With no writer, the reader needs EOF or it waits forever. */
        (void)shutdown(sv[1], SHUT_WR);
    }
    if (reader != NULL) {
        int reader_error = join_error(reader);
        if (error == 0) error = reader_error;
    }

    (void)llam_close(sv[0]);
    (void)llam_close(sv[1]);
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    /* Declared here because a failed join can leave a child running; the
     * run call returns only after every task has finished. */
    echo_state_t state;
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create_compat(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, root, &state, NULL, 0);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return 1;
    }
    llam_task_result_t result;
    int failed = llam_join_result(task, &result, sizeof(result)) != 0;
    if (!failed) failed = result.flags != LLAM_TASK_RESULT_F_SET || result.error != 0;
    llam_runtime_destroy(runtime);
    return failed;
}
```

```text title="Output"
read=hello
```

## Owned buffer read

**Focused snippet.** Use owned buffers when the runtime should allocate storage:

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

## Datagram receive

**Focused snippet.** Receive a datagram and its sender address:

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

## Windows HANDLE file I/O

**Platform-specific, Windows.** Use HANDLE variants for file I/O:

```c
ssize_t n = llam_pread_handle(handle, buf, sizeof(buf), offset);
```

The POSIX-style `llam_pread()` and `llam_pwrite()` fd/socket variants return
`ENOTSUP` on Windows.
