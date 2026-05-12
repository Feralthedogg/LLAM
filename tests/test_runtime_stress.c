/**
 * @file tests/test_runtime_stress.c
 * @brief Runtime stress tests for task storms, cancellation, wakeups, and I/O waits.
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

#include "llam/runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if LLAM_PLATFORM_POSIX
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TASK_STORM_COUNT 20000U
#define NESTED_PARENT_COUNT 96U
#define NESTED_CHILDREN_PER_PARENT 32U
#define CANCEL_WAITER_COUNT 768U
#define CHANNEL_ROUNDS 50000U
#define COND_ROUNDS 12000U

typedef struct stress_state {
    atomic_uint failures;
    atomic_uint task_count;
    atomic_uint parent_count;
    atomic_uint child_count;
    atomic_uint waiting_count;
    atomic_uint canceled_count;
    atomic_uint sent_count;
    atomic_uint recv_count;
    atomic_uint cond_waiting_round;
    atomic_uint cond_generation;
    atomic_uint cond_completed;
    atomic_uint io_waiting;
    atomic_uint io_canceled;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *cancel_token;
    int io_fd;
    int first_errno;
    char first_case[128];
} stress_state_t;

static int fail_msg(const char *message) {
    fprintf(stderr, "[test_runtime_stress] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_stress] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(stress_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return llam_runtime_init(&opts);
}

static int spawn_detached(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts) {
    llam_task_t *task = llam_spawn(fn, arg, opts);

    if (task == NULL) {
        return -1;
    }
    if (llam_detach(task) != 0) {
        return -1;
    }
    return 0;
}

static int check_task_failures(stress_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_stress] task failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

static void task_storm_task(void *arg) {
    stress_state_t *state = arg;

    atomic_fetch_add_explicit(&state->task_count, 1U, memory_order_relaxed);
}

static int test_task_storm(void) {
    stress_state_t state;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.task_count, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for task storm failed");
    }
    for (unsigned i = 0U; i < TASK_STORM_COUNT; ++i) {
        if (spawn_detached(task_storm_task, &state, NULL) != 0) {
            int saved_errno = errno;

            llam_runtime_shutdown();
            errno = saved_errno;
            return fail_errno("spawn task storm task failed");
        }
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run task storm failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.task_count, memory_order_relaxed) != TASK_STORM_COUNT) {
        llam_runtime_shutdown();
        return fail_msg("task storm counter mismatch");
    }
    llam_runtime_shutdown();
    return 0;
}

static void nested_child_task(void *arg) {
    stress_state_t *state = arg;

    atomic_fetch_add_explicit(&state->child_count, 1U, memory_order_relaxed);
}

static void nested_parent_task(void *arg) {
    stress_state_t *state = arg;

    for (unsigned i = 0U; i < NESTED_CHILDREN_PER_PARENT; ++i) {
        if (spawn_detached(nested_child_task, state, NULL) != 0) {
            task_fail(state, "nested child spawn", errno);
            return;
        }
        if ((i & 7U) == 0U) {
            llam_yield();
        }
    }
    atomic_fetch_add_explicit(&state->parent_count, 1U, memory_order_relaxed);
}

static int test_nested_spawn_storm(void) {
    stress_state_t state;
    unsigned expected_children = NESTED_PARENT_COUNT * NESTED_CHILDREN_PER_PARENT;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.parent_count, 0U);
    atomic_init(&state.child_count, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for nested spawn failed");
    }
    for (unsigned i = 0U; i < NESTED_PARENT_COUNT; ++i) {
        if (spawn_detached(nested_parent_task, &state, NULL) != 0) {
            int saved_errno = errno;

            llam_runtime_shutdown();
            errno = saved_errno;
            return fail_errno("spawn nested parent failed");
        }
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run nested spawn failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.parent_count, memory_order_relaxed) != NESTED_PARENT_COUNT ||
        atomic_load_explicit(&state.child_count, memory_order_relaxed) != expected_children) {
        llam_runtime_shutdown();
        return fail_msg("nested spawn count mismatch");
    }
    llam_runtime_shutdown();
    return 0;
}

static void cancel_waiter_task(void *arg) {
    stress_state_t *state = arg;
    int cancelled;

    atomic_fetch_add_explicit(&state->waiting_count, 1U, memory_order_release);
    for (;;) {
        errno = 0;
        cancelled = llam_cancel_token_is_cancelled(state->cancel_token);
        if (cancelled < 0) {
            task_fail(state, "cancel waiter token query", errno);
            return;
        }
        if (cancelled != 0) {
            break;
        }
        if (llam_sleep_ns(1000ULL) != 0 && errno != ECANCELED) {
            task_fail(state, "cancel waiter settle sleep", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->canceled_count, 1U, memory_order_relaxed);
}

static void cancel_storm_task(void *arg) {
    stress_state_t *state = arg;

    while (atomic_load_explicit(&state->waiting_count, memory_order_acquire) < CANCEL_WAITER_COUNT) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->cancel_token) != 0) {
        task_fail(state, "cancel storm token cancel", errno);
    }
}

static int test_cancel_storm(void) {
    stress_state_t state;
    llam_spawn_opts_t opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.waiting_count, 0U);
    atomic_init(&state.canceled_count, 0U);
    state.cancel_token = llam_cancel_token_create();
    if (state.cancel_token == NULL) {
        return fail_errno("cancel token create failed");
    }
    if (llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("spawn opts init for cancel storm failed");
    }
    opts.cancel_token = state.cancel_token;
    if (init_runtime() != 0) {
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("runtime init for cancel storm failed");
    }
    for (unsigned i = 0U; i < CANCEL_WAITER_COUNT; ++i) {
        if (spawn_detached(cancel_waiter_task, &state, &opts) != 0) {
            int saved_errno = errno;

            llam_runtime_shutdown();
            (void)llam_cancel_token_destroy(state.cancel_token);
            errno = saved_errno;
            return fail_errno("spawn cancel waiter failed");
        }
    }
    if (spawn_detached(cancel_storm_task, &state, NULL) != 0) {
        int saved_errno = errno;

        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        errno = saved_errno;
        return fail_errno("spawn cancel storm task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("llam_run cancel storm failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        return 1;
    }
    if (atomic_load_explicit(&state.canceled_count, memory_order_relaxed) != CANCEL_WAITER_COUNT) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_msg("cancel storm count mismatch");
    }
    if (llam_cancel_token_destroy(state.cancel_token) != 0) {
        llam_runtime_shutdown();
        return fail_errno("cancel token destroy after storm failed");
    }
    llam_runtime_shutdown();
    return 0;
}

static void channel_receiver_task(void *arg) {
    stress_state_t *state = arg;

    atomic_store_explicit(&state->waiting_count, 1U, memory_order_release);
    for (uintptr_t i = 1U; i <= CHANNEL_ROUNDS; ++i) {
        void *value = llam_channel_recv(state->channel);

        if (value != (void *)i) {
            task_fail(state, "channel wake payload mismatch", errno);
            return;
        }
        atomic_fetch_add_explicit(&state->recv_count, 1U, memory_order_relaxed);
    }
}

static void channel_sender_task(void *arg) {
    stress_state_t *state = arg;

    while (atomic_load_explicit(&state->waiting_count, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (uintptr_t i = 1U; i <= CHANNEL_ROUNDS; ++i) {
        if (llam_channel_send(state->channel, (void *)i) != 0) {
            task_fail(state, "channel wake send", errno);
            return;
        }
        atomic_fetch_add_explicit(&state->sent_count, 1U, memory_order_relaxed);
    }
}

static int test_channel_wakeup_storm(void) {
    stress_state_t state;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.waiting_count, 0U);
    atomic_init(&state.sent_count, 0U);
    atomic_init(&state.recv_count, 0U);
    state.channel = llam_channel_create(1U);
    if (state.channel == NULL) {
        return fail_errno("channel create for wake storm failed");
    }
    if (init_runtime() != 0) {
        (void)llam_channel_destroy(state.channel);
        return fail_errno("runtime init for channel wake storm failed");
    }
    if (spawn_detached(channel_receiver_task, &state, NULL) != 0 ||
        spawn_detached(channel_sender_task, &state, NULL) != 0) {
        int saved_errno = errno;

        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.channel);
        errno = saved_errno;
        return fail_errno("spawn channel wake tasks failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.channel);
        return fail_errno("llam_run channel wake storm failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.channel);
        return 1;
    }
    if (atomic_load_explicit(&state.sent_count, memory_order_relaxed) != CHANNEL_ROUNDS ||
        atomic_load_explicit(&state.recv_count, memory_order_relaxed) != CHANNEL_ROUNDS) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.channel);
        return fail_msg("channel wake storm count mismatch");
    }
    if (llam_channel_destroy(state.channel) != 0) {
        llam_runtime_shutdown();
        return fail_errno("channel destroy after wake storm failed");
    }
    llam_runtime_shutdown();
    return 0;
}

static void cond_waiter_task(void *arg) {
    stress_state_t *state = arg;

    for (unsigned round = 0U; round < COND_ROUNDS; ++round) {
        if (llam_mutex_lock(state->mutex) != 0) {
            task_fail(state, "cond waiter lock", errno);
            return;
        }
        atomic_store_explicit(&state->cond_waiting_round, round + 1U, memory_order_release);
        while (atomic_load_explicit(&state->cond_generation, memory_order_acquire) <= round) {
            if (llam_cond_wait(state->cond, state->mutex) != 0) {
                task_fail(state, "cond waiter wait", errno);
                (void)llam_mutex_unlock(state->mutex);
                return;
            }
        }
        if (llam_mutex_unlock(state->mutex) != 0) {
            task_fail(state, "cond waiter unlock", errno);
            return;
        }
        atomic_fetch_add_explicit(&state->cond_completed, 1U, memory_order_relaxed);
    }
}

static void cond_signaler_task(void *arg) {
    stress_state_t *state = arg;

    for (unsigned round = 0U; round < COND_ROUNDS; ++round) {
        while (atomic_load_explicit(&state->cond_waiting_round, memory_order_acquire) <= round) {
            llam_yield();
        }
        if (llam_mutex_lock(state->mutex) != 0) {
            task_fail(state, "cond signaler lock", errno);
            return;
        }
        atomic_store_explicit(&state->cond_generation, round + 1U, memory_order_release);
        if (llam_cond_signal(state->cond) != 0) {
            task_fail(state, "cond signal", errno);
            (void)llam_mutex_unlock(state->mutex);
            return;
        }
        if (llam_mutex_unlock(state->mutex) != 0) {
            task_fail(state, "cond signaler unlock", errno);
            return;
        }
    }
}

static int test_cond_wakeup_storm(void) {
    stress_state_t state;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.cond_waiting_round, 0U);
    atomic_init(&state.cond_generation, 0U);
    atomic_init(&state.cond_completed, 0U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    if (state.mutex == NULL || state.cond == NULL) {
        if (state.cond != NULL) {
            (void)llam_cond_destroy(state.cond);
        }
        if (state.mutex != NULL) {
            (void)llam_mutex_destroy(state.mutex);
        }
        return fail_errno("cond wake fixture create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_cond_destroy(state.cond);
        (void)llam_mutex_destroy(state.mutex);
        return fail_errno("runtime init for cond wake storm failed");
    }
    if (spawn_detached(cond_waiter_task, &state, NULL) != 0 ||
        spawn_detached(cond_signaler_task, &state, NULL) != 0) {
        int saved_errno = errno;

        llam_runtime_shutdown();
        (void)llam_cond_destroy(state.cond);
        (void)llam_mutex_destroy(state.mutex);
        errno = saved_errno;
        return fail_errno("spawn cond wake tasks failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        (void)llam_cond_destroy(state.cond);
        (void)llam_mutex_destroy(state.mutex);
        return fail_errno("llam_run cond wake storm failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_cond_destroy(state.cond);
        (void)llam_mutex_destroy(state.mutex);
        return 1;
    }
    if (atomic_load_explicit(&state.cond_completed, memory_order_relaxed) != COND_ROUNDS) {
        llam_runtime_shutdown();
        (void)llam_cond_destroy(state.cond);
        (void)llam_mutex_destroy(state.mutex);
        return fail_msg("cond wake storm count mismatch");
    }
    if (llam_cond_destroy(state.cond) != 0 ||
        llam_mutex_destroy(state.mutex) != 0) {
        llam_runtime_shutdown();
        return fail_errno("destroy cond wake fixture failed");
    }
    llam_runtime_shutdown();
    return 0;
}

#if LLAM_PLATFORM_POSIX
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void io_cancel_reader_task(void *arg) {
    stress_state_t *state = arg;
    char buf[16];

    atomic_store_explicit(&state->io_waiting, 1U, memory_order_release);
    errno = 0;
    if (llam_read_when_ready(state->io_fd, buf, sizeof(buf), -1) != -1 || errno != ECANCELED) {
        task_fail(state, "io cancel read wait", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->io_canceled, 1U, memory_order_relaxed);
}

static void io_cancel_task(void *arg) {
    stress_state_t *state = arg;

    while (atomic_load_explicit(&state->io_waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->cancel_token) != 0) {
        task_fail(state, "io cancel token cancel", errno);
    }
}

static int test_posix_io_cancel_wait(void) {
    stress_state_t state;
    llam_spawn_opts_t opts;
    int fds[2] = {-1, -1};

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.io_waiting, 0U);
    atomic_init(&state.io_canceled, 0U);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return fail_errno("socketpair for io cancel failed");
    }
    if (set_nonblocking(fds[0]) != 0 ||
        set_nonblocking(fds[1]) != 0) {
        close(fds[0]);
        close(fds[1]);
        return fail_errno("set_nonblocking for io cancel failed");
    }
    state.io_fd = fds[0];
    state.cancel_token = llam_cancel_token_create();
    if (state.cancel_token == NULL) {
        close(fds[0]);
        close(fds[1]);
        return fail_errno("cancel token create for io cancel failed");
    }
    if (llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        return fail_errno("spawn opts init for io cancel failed");
    }
    opts.cancel_token = state.cancel_token;
    if (init_runtime() != 0) {
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        return fail_errno("runtime init for io cancel failed");
    }
    if (spawn_detached(io_cancel_reader_task, &state, &opts) != 0 ||
        spawn_detached(io_cancel_task, &state, NULL) != 0) {
        int saved_errno = errno;

        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        errno = saved_errno;
        return fail_errno("spawn io cancel tasks failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        return fail_errno("llam_run io cancel failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    if (atomic_load_explicit(&state.io_canceled, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(state.cancel_token);
        close(fds[0]);
        close(fds[1]);
        return fail_msg("io cancel count mismatch");
    }
    if (llam_cancel_token_destroy(state.cancel_token) != 0) {
        llam_runtime_shutdown();
        close(fds[0]);
        close(fds[1]);
        return fail_errno("cancel token destroy after io cancel failed");
    }
    llam_runtime_shutdown();
    close(fds[0]);
    close(fds[1]);
    return 0;
}
#endif

int main(void) {
    if (test_task_storm() != 0) {
        return 1;
    }
    if (test_nested_spawn_storm() != 0) {
        return 1;
    }
    if (test_cancel_storm() != 0) {
        return 1;
    }
    if (test_channel_wakeup_storm() != 0) {
        return 1;
    }
    if (test_cond_wakeup_storm() != 0) {
        return 1;
    }
#if LLAM_PLATFORM_POSIX
    if (test_posix_io_cancel_wait() != 0) {
        return 1;
    }
#endif
    puts("test_runtime_stress ok");
    return 0;
}
