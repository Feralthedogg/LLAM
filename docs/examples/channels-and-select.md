# Channels And Select

Use channels to move pointer values between tasks without blocking worker
threads.

## Producer And Consumer

```c
#include <llam/runtime.h>

#include <stdio.h>

typedef struct state {
    llam_channel_t *channel;
} state_t;

static void producer(void *arg) {
    state_t *state = arg;

    (void)llam_channel_send(state->channel, "ping");
    (void)llam_channel_send(state->channel, "pong");
    (void)llam_channel_close(state->channel);
}

static void consumer(void *arg) {
    state_t *state = arg;
    void *value = NULL;

    while (llam_channel_recv_result(state->channel, &value) == 0) {
        printf("recv=%s\n", (const char *)value);
    }
}

static void root(void *arg) {
    (void)arg;

    state_t state = {
        .channel = llam_channel_create(2),
    };
    if (state.channel == NULL) {
        return;
    }

    llam_task_t *a = llam_spawn(producer, &state, NULL);
    llam_task_t *b = llam_spawn(consumer, &state, NULL);

    if (a != NULL) {
        (void)llam_join(a);
    }
    if (b != NULL) {
        (void)llam_join(b);
    }

    (void)llam_channel_destroy(state.channel);
}
```

Use result-style receive APIs when `NULL` is a valid payload. Convenience
`llam_channel_recv()` returns `NULL` for both payload and failure.

## Select Between Channels

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
