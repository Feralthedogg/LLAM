# Channels and select

Use channels to move pointer values between tasks without blocking worker
threads.

## Producer and consumer

**Complete program.** One task sends two values and closes the channel. Another
task receives them until the channel closes. If either task fails to start or
reports an error, the program exits with a failure status.

```c title="main.c"
#include <llam/runtime.h>

#include <errno.h>
#include <stdio.h>

static void producer(void *arg) {
    llam_channel_t *channel = arg;
    int error = 0;

    if (llam_channel_send(channel, "ping") != 0 ||
        llam_channel_send(channel, "pong") != 0) {
        error = errno;
    }
    (void)llam_channel_close(channel);
    (void)llam_task_set_result(NULL, error);
}

static void consumer(void *arg) {
    llam_channel_t *channel = arg;
    void *value = NULL;

    while (llam_channel_recv_result(channel, &value) == 0) {
        printf("recv=%s\n", (const char *)value);
    }
    /* EPIPE means the channel is closed and drained. */
    (void)llam_task_set_result(NULL, errno == EPIPE ? 0 : errno);
}

static int join_error(llam_task_t *task) {
    llam_task_result_t result;

    return llam_join_result(task, &result, sizeof(result)) == 0 ? result.error : errno;
}

static void root(void *arg) {
    (void)arg;

    llam_channel_t *channel = llam_channel_create(2);
    if (channel == NULL) {
        (void)llam_task_set_result(NULL, errno);
        return;
    }

    int error = 0;
    llam_task_t *b = llam_spawn(consumer, channel, NULL);
    llam_task_t *a = b != NULL ? llam_spawn(producer, channel, NULL) : NULL;

    if (a != NULL) {
        error = join_error(a);
    } else {
        error = errno;
        /* With no producer to close it, the consumer would wait forever. */
        (void)llam_channel_close(channel);
    }
    if (b != NULL) {
        int consumer_error = join_error(b);
        if (error == 0) error = consumer_error;
    }

    if (llam_channel_destroy(channel) != 0 && error == 0) error = errno;
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create_compat(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, root, NULL, NULL, 0);
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
recv=ping
recv=pong
```

Use result-style receive APIs when `NULL` is a valid payload. Convenience
`llam_channel_recv()` returns `NULL` for both payload and failure.

## Select between channels

**Focused snippet.** This block belongs inside a task that owns both channels.

```c
void *left_value = NULL;
void *right_value = NULL;
llam_select_op_t ops[2] = {
    {
        .kind = LLAM_SELECT_OP_RECV,
        .channel = left,
        .recv_out = &left_value,
    },
    {
        .kind = LLAM_SELECT_OP_RECV,
        .channel = right,
        .recv_out = &right_value,
    },
};

size_t selected = 0;
int rc = llam_channel_select(ops, 2, UINT64_MAX, &selected);

if (rc == 0 && ops[selected].result_errno == 0) {
    void *value = selected == 0 ? left_value : right_value;
    handle_value(value);
}
```

Use `deadline_ns = 0` for a single nonblocking scan, or `UINT64_MAX` for no
deadline.
