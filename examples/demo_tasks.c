/**
 * @file examples/demo_tasks.c
 * @brief Concrete demo tasks that exercise scheduling, sync, blocking, I/O, and cancellation APIs.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "demo_internal.h"

llam_task_t *g_worker_task;

unsigned demo_env_flag_default(const char *name, unsigned default_value) {
    return llam_example_env_flag_default(name, default_value);
}

int demo_env_i32(const char *name, int default_value, int min_value, int max_value) {
    const char *value = llam_example_env_get(name);
    char *end = NULL;
    long parsed = 0L;

    if (value == NULL || *value == '\0') {
        return default_value;
    }
    if (value[0] == ' ' || value[0] == '\t' || value[0] == '\n' ||
        value[0] == '\r' || value[0] == '\f' || value[0] == '\v') {
        return default_value;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    if (parsed < (long)min_value) {
        parsed = (long)min_value;
    }
    if (parsed > (long)max_value) {
        parsed = (long)max_value;
    }
    return (int)parsed;
}

static uint32_t demo_current_mxcsr(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
#else
    switch (fegetround()) {
    case FE_DOWNWARD:
        return 1U << 13;
    case FE_UPWARD:
        return 2U << 13;
    case FE_TOWARDZERO:
        return 3U << 13;
    case FE_TONEAREST:
    default:
        return 0U;
    }
#endif
}

static uint16_t demo_current_x87_cw(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint16_t value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
#else
    return (uint16_t)(demo_current_mxcsr() >> 3);
#endif
}

static void demo_set_fp_round(unsigned mode) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr = demo_current_mxcsr();
    uint16_t x87_cw = demo_current_x87_cw();

    mxcsr &= ~(3U << 13);
    mxcsr |= (mode & 3U) << 13;
    x87_cw &= (uint16_t)~(3U << 10);
    x87_cw |= (uint16_t)((mode & 3U) << 10);

    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(x87_cw));
#else
    int fe_round = FE_TONEAREST;

    switch (mode & 3U) {
    case 1U:
        fe_round = FE_DOWNWARD;
        break;
    case 2U:
        fe_round = FE_UPWARD;
        break;
    case 3U:
        fe_round = FE_TOWARDZERO;
        break;
    default:
        break;
    }
    (void)fesetround(fe_round);
#endif
}

static unsigned demo_fp_round_mode(void) {
    unsigned mxcsr_mode = (demo_current_mxcsr() >> 13) & 3U;
    unsigned x87_mode = (unsigned)((demo_current_x87_cw() >> 10) & 3U);

    return mxcsr_mode == x87_mode ? mxcsr_mode : 0xFFU;
}

static const char *demo_fp_round_name(unsigned mode) {
    switch (mode) {
    case 0U:
        return "nearest";
    case 1U:
        return "down";
    case 2U:
        return "up";
    case 3U:
        return "zero";
    default:
        return "mixed";
    }
}

static void demo_runtime_sleep_ms(unsigned duration_ms) {
    if (duration_ms == 0U) {
        return;
    }
    (void)llam_sleep_ns((uint64_t)duration_ms * 1000ULL * 1000ULL);
}

static void demo_opaque_block_ms(unsigned duration_ms) {
    uint64_t deadline_ns;

    if (duration_ms == 0U) {
        return;
    }
    /*
     * Keep the opaque-blocking example independent of libc feature-test macros:
     * the point here is to pin the current worker inside foreign-looking code
     * after llam_enter_blocking(), not to demonstrate a particular POSIX sleep
     * primitive.
     */
    deadline_ns = llam_now_ns() + (uint64_t)duration_ms * 1000ULL * 1000ULL;
    while (llam_now_ns() < deadline_ns) {
    }
}

static void *slow_square(void *arg) {
    struct slow_job *job = arg;

    demo_runtime_sleep_ms(job->pause_ms);
    job->output = job->input * job->input;
    return job;
}

static void *blocking_pause(void *arg) {
    (void)arg;
    demo_runtime_sleep_ms(20U);
    return arg;
}

void counter_task(void *arg) {
    const char *name = arg;

    for (int i = 0; i < 4; ++i) {
        printf("[%s] iteration=%d task=%llu flags=0x%x\n",
               name,
               i,
               (unsigned long long)llam_task_id(llam_current_task()),
               llam_task_flags(llam_current_task()));
        llam_yield();
    }
}

static void fp_round_task(void *arg) {
    struct fp_round_job *job = arg;
    unsigned before = demo_fp_round_mode();
    unsigned after;

    demo_set_fp_round(job->mode);
    printf("[fp-%s] before=%s set=%s\n",
           job->label,
           demo_fp_round_name(before),
           demo_fp_round_name(job->mode));
    llam_yield();
    after = demo_fp_round_mode();
    printf("[fp-%s] after=%s\n", job->label, demo_fp_round_name(after));
}

void fp_isolation_task(void *arg) {
    struct fp_round_job down = {
        .label = "down",
        .mode = 1U,
    };
    struct fp_round_job up = {
        .label = "up",
        .mode = 2U,
    };
    llam_task_t *down_task;
    llam_task_t *up_task;

    (void)arg;
    down_task = llam_spawn(fp_round_task,
                         &down,
                         &(llam_spawn_opts_t){
                             .task_class = LLAM_TASK_CLASS_DEFAULT,
                             .stack_class = LLAM_STACK_CLASS_DEFAULT,
                             .flags = LLAM_SPAWN_F_PINNED,
                         });
    up_task = llam_spawn(fp_round_task,
                       &up,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                           .flags = LLAM_SPAWN_F_PINNED,
                       });
    if (down_task != NULL) {
        (void)llam_join(down_task);
    }
    if (up_task != NULL) {
        (void)llam_join(up_task);
    }
    printf("[fp-parent] final=%s\n", demo_fp_round_name(demo_fp_round_mode()));
}

void sleeper_task(void *arg) {
    (void)arg;
    printf("[sleep] before sleep\n");
    llam_sleep_ns(20ULL * 1000ULL * 1000ULL);
    printf("[sleep] after sleep\n");
}

void blocking_task(void *arg) {
    struct slow_job *job = arg;
    void *result = NULL;

    printf("[block] offloading slow square for %d\n", job->input);
    (void)llam_call_blocking_result(slow_square, job, &result);
    (void)result;
    printf("[block] result=%d\n", job->output);
}

void joiner_task(void *arg) {
    (void)arg;
    printf("[joiner] waiting for worker\n");
    llam_join(g_worker_task);
    printf("[joiner] worker completed\n");
}

void timed_join_target_task(void *arg) {
    (void)arg;
    llam_sleep_ns(15ULL * 1000ULL * 1000ULL);
    printf("[join-timeout-target] finished\n");
}

void timed_join_waiter_task(void *arg) {
    struct timed_join_state *state = arg;

    if (llam_join_until(state->target, llam_now_ns() + 2ULL * 1000ULL * 1000ULL) != 0) {
        printf("[join-timeout] errno=%d\n", errno);
    } else {
        printf("[join-timeout] unexpected success\n");
    }
}

void io_reader_task(void *arg) {
    struct io_pair *pair = arg;
    char buffer[32] = {0};
    ssize_t rc = llam_read(pair->reader_fd, buffer, 4U);

    printf("[io-read] rc=%zd payload=%.*s\n", rc, rc > 0 ? (int)rc : 0, buffer);
}

void io_owned_reader_task(void *arg) {
    struct io_pair *pair = arg;
    llam_io_buffer_t *buffer = NULL;
    ssize_t rc = llam_read_owned(pair->reader_fd, 16U, &buffer);

    printf("[io-owned-read] rc=%zd payload=%.*s cap=%zu\n",
           rc,
           rc > 0 && buffer != NULL ? (int)llam_io_buffer_size(buffer) : 0,
           rc > 0 && buffer != NULL ? (const char *)llam_io_buffer_data(buffer) : "",
           buffer != NULL ? llam_io_buffer_capacity(buffer) : 0U);
    llam_io_buffer_release(buffer);
}

void io_owned_peek_task(void *arg) {
    struct io_pair *pair = arg;
    llam_io_buffer_t *buffer = NULL;
    ssize_t rc = llam_recv_owned(pair->reader_fd, 16U, MSG_PEEK, &buffer);

    printf("[io-owned-peek] rc=%zd payload=%.*s cap=%zu\n",
           rc,
           rc > 0 && buffer != NULL ? (int)llam_io_buffer_size(buffer) : 0,
           rc > 0 && buffer != NULL ? (const char *)llam_io_buffer_data(buffer) : "",
           buffer != NULL ? llam_io_buffer_capacity(buffer) : 0U);
    llam_io_buffer_release(buffer);
    buffer = NULL;

    rc = llam_read_owned(pair->reader_fd, 16U, &buffer);
    printf("[io-owned-after-peek] rc=%zd payload=%.*s cap=%zu\n",
           rc,
           rc > 0 && buffer != NULL ? (int)llam_io_buffer_size(buffer) : 0,
           rc > 0 && buffer != NULL ? (const char *)llam_io_buffer_data(buffer) : "",
           buffer != NULL ? llam_io_buffer_capacity(buffer) : 0U);
    llam_io_buffer_release(buffer);
}

void io_writer_task(void *arg) {
    struct io_pair *pair = arg;
    static const char payload[] = "pong";
    ssize_t rc;

    llam_sleep_ns(5ULL * 1000ULL * 1000ULL);
    rc = llam_write(pair->writer_fd, payload, 4U);
    printf("[io-write] rc=%zd\n", rc);
}

void io_owned_writer_task(void *arg) {
    struct io_pair *pair = arg;
    static const char payload[] = "owned";
    ssize_t rc;

    llam_sleep_ns(5ULL * 1000ULL * 1000ULL);
    rc = llam_write(pair->writer_fd, payload, sizeof(payload) - 1U);
    printf("[io-owned-write] rc=%zd\n", rc);
}

void io_peek_writer_task(void *arg) {
    struct io_pair *pair = arg;
    static const char payload[] = "peek";
    ssize_t rc;

    llam_sleep_ns(5ULL * 1000ULL * 1000ULL);
    rc = llam_write(pair->writer_fd, payload, sizeof(payload) - 1U);
    printf("[io-peek-write] rc=%zd\n", rc);
}

void poll_waiter_task(void *arg) {
    struct poll_pair *pair = arg;
    short revents = 0;
    int rc = llam_poll_fd(pair->reader_fd, POLLIN, -1, &revents);

    printf("[poll-wait] rc=%d revents=0x%x\n", rc, (unsigned)revents);
}

void poll_writer_task(void *arg) {
    struct poll_pair *pair = arg;
    static const char payload[] = "tick";
    ssize_t rc;

    llam_sleep_ns(6ULL * 1000ULL * 1000ULL);
    rc = llam_write(pair->writer_fd, payload, 4U);
    printf("[poll-write] rc=%zd\n", rc);
}

void poll_timeout_task(void *arg) {
    int fd = *(int *)arg;
    short revents = 0;
    int rc = llam_poll_fd(fd, POLLIN, 3, &revents);

    printf("[poll-timeout] rc=%d revents=0x%x\n", rc, (unsigned)revents);
}

void cond_waiter_task(void *arg) {
    struct cond_state *state = arg;

    llam_mutex_lock(state->mutex);
    while (!state->ready) {
        llam_cond_wait(state->cond, state->mutex);
    }
    printf("[cond-waiter] observed ready=1\n");
    llam_mutex_unlock(state->mutex);
}

void cond_timeout_task(void *arg) {
    struct cond_state *state = arg;

    llam_mutex_lock(state->mutex);
    if (!state->ready &&
        llam_cond_wait_until(state->cond, state->mutex, llam_now_ns() + 2ULL * 1000ULL * 1000ULL) != 0) {
        printf("[cond-timeout] errno=%d\n", errno);
    } else {
        printf("[cond-timeout] unexpected success\n");
    }
    llam_mutex_unlock(state->mutex);
}

void cond_signaler_task(void *arg) {
    struct cond_state *state = arg;

    llam_sleep_ns(8ULL * 1000ULL * 1000ULL);
    llam_mutex_lock(state->mutex);
    state->ready = 1;
    printf("[cond-signaler] set ready=1\n");
    llam_cond_signal(state->cond);
    llam_mutex_unlock(state->mutex);
}

void channel_recv_task(void *arg) {
    struct channel_state *state = arg;
    const char *message = llam_channel_recv(state->channel);

    if (message != NULL) {
        printf("[channel-recv] message=%s\n", message);
    }
}

void channel_send_task(void *arg) {
    struct channel_state *state = arg;

    llam_sleep_ns(4ULL * 1000ULL * 1000ULL);
    llam_channel_send(state->channel, "mailbox-ready");
    printf("[channel-send] sent mailbox-ready\n");
}

void channel_timeout_recv_task(void *arg) {
    struct channel_state *state = arg;
    void *value = llam_channel_recv_until(state->channel, llam_now_ns() + 2ULL * 1000ULL * 1000ULL);

    if (value == NULL) {
        printf("[channel-recv-timeout] errno=%d\n", errno);
    } else {
        printf("[channel-recv-timeout] unexpected value=%s\n", (const char *)value);
    }
}

void channel_timeout_fill_task(void *arg) {
    struct channel_state *state = arg;

    llam_channel_send(state->channel, "prefilled");
    if (state->ready != NULL) {
        atomic_store_explicit(state->ready, 1U, memory_order_release);
    }
    printf("[channel-send-fill] queued prefilled\n");
}

void channel_timeout_send_task(void *arg) {
    struct channel_state *state = arg;

    while (state->ready != NULL && atomic_load_explicit(state->ready, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_channel_send_until(state->channel, "late", llam_now_ns() + 2ULL * 1000ULL * 1000ULL) != 0) {
        printf("[channel-send-timeout] errno=%d\n", errno);
    } else {
        printf("[channel-send-timeout] unexpected success\n");
    }
}

void cancel_waiter_task(void *arg) {
    (void)arg;
    if (llam_sleep_ns(100ULL * 1000ULL * 1000ULL) != 0) {
        printf("[cancel-waiter] sleep canceled errno=%d\n", errno);
    } else {
        printf("[cancel-waiter] unexpectedly completed sleep\n");
    }
}

void cancel_trigger_task(void *arg) {
    struct cancel_state *state = arg;

    llam_sleep_ns(7ULL * 1000ULL * 1000ULL);
    llam_cancel_token_cancel(state->token);
    printf("[cancel-trigger] requested cancellation\n");
}

void io_cancel_waiter_task(void *arg) {
    struct io_cancel_state *state = arg;
    char byte = 0;
    ssize_t rc = llam_read(state->reader_fd, &byte, 1U);

    if (rc < 0) {
        printf("[io-cancel] errno=%d\n", errno);
    } else {
        printf("[io-cancel] unexpected rc=%zd\n", rc);
    }
}

void io_cancel_trigger_task(void *arg) {
    struct io_cancel_state *state = arg;

    llam_sleep_ns(8ULL * 1000ULL * 1000ULL);
    llam_cancel_token_cancel(state->token);
    printf("[io-cancel-trigger] requested cancellation\n");
}

void block_cancel_waiter_task(void *arg) {
    void *result = NULL;

    (void)arg;
    if (llam_call_blocking_result(blocking_pause, NULL, &result) != 0 && errno == ECANCELED) {
        printf("[block-cancel] errno=%d\n", errno);
    } else {
        (void)result;
        printf("[block-cancel] unexpected completion errno=%d\n", errno);
    }
}

void block_cancel_trigger_task(void *arg) {
    struct block_cancel_state *state = arg;

    llam_sleep_ns(5ULL * 1000ULL * 1000ULL);
    llam_cancel_token_cancel(state->token);
    printf("[block-cancel-trigger] requested cancellation\n");
}

static int connect_loopback(struct connect_job *job) {
    struct sockaddr_in addr;
    int fd;
    int rc;

    if (job == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(job->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = llam_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc;
}

void accept_waiter_task(void *arg) {
    struct accept_state *state = arg;
    int fd = llam_accept(state->listener_fd, NULL, NULL);

    printf("[accept-wait] fd=%d\n", fd);
    if (fd >= 0) {
        close(fd);
    }
}

void accept_connector_task(void *arg) {
    struct connect_job *job = arg;

    llam_sleep_ns(9ULL * 1000ULL * 1000ULL);
    if (connect_loopback(job) == 0) {
        printf("[accept-connect:%s] connected\n", job->label);
    }
}

void mutex_holder_task(void *arg) {
    struct timed_mutex_state *state = arg;

    llam_mutex_lock(state->mutex);
    llam_sleep_ns(12ULL * 1000ULL * 1000ULL);
    printf("[mutex-holder] releasing lock\n");
    llam_mutex_unlock(state->mutex);
}

void mutex_timeout_task(void *arg) {
    struct timed_mutex_state *state = arg;

    llam_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (llam_mutex_lock_until(state->mutex, llam_now_ns() + 2ULL * 1000ULL * 1000ULL) != 0) {
        printf("[mutex-timeout] errno=%d\n", errno);
    } else {
        printf("[mutex-timeout] unexpected success\n");
        llam_mutex_unlock(state->mutex);
    }
}

void opaque_block_task(void *arg) {
    unsigned iteration;

    (void)arg;

    for (iteration = 0; iteration < 2U; ++iteration) {
        llam_spawn(counter_task,
                   iteration == 0U ? "opaque-companion-a" : "opaque-companion-b",
                   &(llam_spawn_opts_t){
                       .task_class = LLAM_TASK_CLASS_DEFAULT,
                       .stack_class = LLAM_STACK_CLASS_DEFAULT,
                   });
        printf("[opaque-block] entering same-thread blocking scope=%u\n", iteration);
        llam_enter_blocking();
        demo_opaque_block_ms(6U);
        llam_leave_blocking();
        printf("[opaque-block] left same-thread blocking scope=%u\n", iteration);
        llam_sleep_ns(1ULL * 1000ULL * 1000ULL);
    }
}
