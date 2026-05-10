/**
 * @file tests/test_sync_primitives.c
 * @brief Runtime mutex, condition variable, channel, timeout, and close tests.
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

typedef struct sync_state {
    llam_mutex_t *counter_mutex;
    llam_mutex_t *cond_mutex;
    llam_mutex_t *destroy_mutex;
    llam_mutex_t *busy_cond_mutex;
    llam_cond_t *cond;
    llam_cond_t *destroy_cond;
    llam_cond_t *busy_cond;
    llam_channel_t *channel;
    llam_channel_t *empty_channel;
    llam_channel_t *full_channel;
    llam_channel_t *null_channel;
    llam_channel_t *busy_channel;
    llam_channel_t *errno_request_channel;
    llam_channel_t *errno_response_channel;
    llam_channel_t *select_recv_channel;
    llam_channel_t *select_send_channel;
    llam_channel_t *select_full_channel;
    llam_channel_t *select_park_channel;
    llam_channel_t *select_empty_a;
    llam_channel_t *select_empty_b;
    llam_channel_t *bounded3_channel;
    llam_task_local_key_t tls_key;
    atomic_uint failures;
    atomic_uint counter;
    atomic_uint cond_waiting;
    atomic_uint cond_ready;
    atomic_uint cond_signaled;
    atomic_uint busy_cond_waiting;
    atomic_uint busy_cond_release;
    atomic_uint busy_cond_destroy_checked;
    atomic_uint errno_peer_waiting;
    atomic_uint channel_sum;
    atomic_uint timeout_checks;
    atomic_uint null_checks;
    atomic_uint errno_checks;
    atomic_uint destroy_checks;
    atomic_uint select_checks;
    atomic_uint select_park_waiting;
    atomic_uint select_park_checks;
    atomic_uint tls_checks;
    atomic_uint group_checks;
    atomic_uint bounded_capacity_checks;
    int first_errno;
    char first_case[128];
} sync_state_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_sync_primitives] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_sync_primitives] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(sync_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void counter_task(void *arg) {
    sync_state_t *state = arg;
    unsigned i;

    for (i = 0U; i < 100U; ++i) {
        if (llam_mutex_lock(state->counter_mutex) != 0) {
            task_fail(state, "counter mutex lock", errno);
            return;
        }
        atomic_fetch_add_explicit(&state->counter, 1U, memory_order_relaxed);
        if (llam_mutex_unlock(state->counter_mutex) != 0) {
            task_fail(state, "counter mutex unlock", errno);
            return;
        }
        llam_yield();
    }
}

static void cond_waiter_task(void *arg) {
    sync_state_t *state = arg;

    if (llam_mutex_lock(state->cond_mutex) != 0) {
        task_fail(state, "cond waiter mutex lock", errno);
        return;
    }
    atomic_store_explicit(&state->cond_waiting, 1U, memory_order_release);
    while (atomic_load_explicit(&state->cond_ready, memory_order_acquire) == 0U) {
        if (llam_cond_wait(state->cond, state->cond_mutex) != 0) {
            task_fail(state, "llam_cond_wait", errno);
            (void)llam_mutex_unlock(state->cond_mutex);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->cond_signaled, 1U, memory_order_relaxed);
    if (llam_mutex_unlock(state->cond_mutex) != 0) {
        task_fail(state, "cond waiter mutex unlock", errno);
    }
}

static void cond_signaler_task(void *arg) {
    sync_state_t *state = arg;

    while (atomic_load_explicit(&state->cond_waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_mutex_lock(state->cond_mutex) != 0) {
        task_fail(state, "cond signaler mutex lock", errno);
        return;
    }
    atomic_store_explicit(&state->cond_ready, 1U, memory_order_release);
    if (llam_cond_signal(state->cond) != 0) {
        task_fail(state, "llam_cond_signal", errno);
        (void)llam_mutex_unlock(state->cond_mutex);
        return;
    }
    if (llam_mutex_unlock(state->cond_mutex) != 0) {
        task_fail(state, "cond signaler mutex unlock", errno);
    }
}

static void channel_sender_task(void *arg) {
    sync_state_t *state = arg;
    uintptr_t value;

    for (value = 1U; value <= 5U; ++value) {
        if (llam_channel_send(state->channel, (void *)value) != 0) {
            task_fail(state, "llam_channel_send", errno);
            return;
        }
    }
}

static void channel_receiver_task(void *arg) {
    sync_state_t *state = arg;
    uintptr_t sum = 0U;
    unsigned i;

    for (i = 0U; i < 5U; ++i) {
        void *value = llam_channel_recv(state->channel);

        if (value == NULL) {
            task_fail(state, "llam_channel_recv", errno);
            return;
        }
        sum += (uintptr_t)value;
    }
    atomic_store_explicit(&state->channel_sum, (unsigned)sum, memory_order_relaxed);
}

static void timeout_and_close_task(void *arg) {
    sync_state_t *state = arg;
    void *value;

    errno = 0;
    value = llam_channel_recv_until(state->empty_channel, llam_now_ns() + 1000000ULL);
    if (value != NULL || errno != ETIMEDOUT) {
        task_fail(state, "empty channel recv timeout", errno);
        return;
    }
    errno = 0;
    if (llam_channel_try_recv_result(state->empty_channel, &value) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "empty channel try recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->timeout_checks, 1U, memory_order_relaxed);

    if (llam_channel_send(state->full_channel, (void *)(uintptr_t)1U) != 0) {
        task_fail(state, "fill channel send", errno);
        return;
    }
    errno = 0;
    if (llam_channel_try_send(state->full_channel, (void *)(uintptr_t)2U) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "full channel try send", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send_until(state->full_channel, (void *)(uintptr_t)2U, llam_now_ns() + 1000000ULL) != -1 ||
        errno != ETIMEDOUT) {
        task_fail(state, "full channel send timeout", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->timeout_checks, 1U, memory_order_relaxed);

    if (llam_channel_close(state->full_channel) != 0) {
        task_fail(state, "llam_channel_close", errno);
        return;
    }
    value = llam_channel_recv(state->full_channel);
    if (value != (void *)(uintptr_t)1U) {
        task_fail(state, "closed channel buffered recv", EPROTO);
        return;
    }
    errno = 0;
    value = llam_channel_recv(state->full_channel);
    if (value != NULL || errno != EPIPE) {
        task_fail(state, "closed channel empty recv", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send(state->full_channel, (void *)(uintptr_t)3U) != -1 || errno != EPIPE) {
        task_fail(state, "closed channel send", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->timeout_checks, 1U, memory_order_relaxed);
}

static void null_channel_task(void *arg) {
    sync_state_t *state = arg;
    void *value = (void *)state;

    if (llam_channel_try_send(state->null_channel, NULL) != 0) {
        task_fail(state, "null channel try send", errno);
        return;
    }
    if (llam_channel_try_recv_result(state->null_channel, &value) != 0) {
        task_fail(state, "null channel try recv result", errno);
        return;
    }
    if (value != NULL) {
        task_fail(state, "null channel recv result payload", EPROTO);
        return;
    }
    errno = 0;
    if (llam_channel_recv_result(state->null_channel, NULL) != -1 || errno != EINVAL) {
        task_fail(state, "null channel recv result NULL out", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->null_checks, 1U, memory_order_relaxed);
}

static void channel_errno_peer_task(void *arg) {
    sync_state_t *state = arg;
    void *value;

    atomic_store_explicit(&state->errno_peer_waiting, 1U, memory_order_release);
    errno = EAGAIN;
    value = llam_channel_recv(state->errno_request_channel);
    if (value != (void *)(uintptr_t)17U || errno != EAGAIN) {
        task_fail(state, "direct handoff receiver errno", errno);
        return;
    }

    errno = EADDRINUSE;
    if (llam_channel_send(state->errno_response_channel, value) != 0 || errno != EADDRINUSE) {
        task_fail(state, "direct handoff response errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->errno_checks, 1U, memory_order_relaxed);
}

static void channel_errno_sender_task(void *arg) {
    sync_state_t *state = arg;
    void *value;

    while (atomic_load_explicit(&state->errno_peer_waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    errno = ECHILD;
    if (llam_channel_send(state->errno_request_channel, (void *)(uintptr_t)17U) != 0 || errno != ECHILD) {
        task_fail(state, "direct handoff sender errno", errno);
        return;
    }

    errno = ENAMETOOLONG;
    value = llam_channel_recv(state->errno_response_channel);
    if (value != (void *)(uintptr_t)17U || errno != ENAMETOOLONG) {
        task_fail(state, "direct handoff sender recv errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->errno_checks, 1U, memory_order_relaxed);
}

static void destroy_contract_task(void *arg) {
    sync_state_t *state = arg;
    void *value = NULL;

    if (llam_mutex_lock(state->destroy_mutex) != 0) {
        task_fail(state, "destroy mutex lock", errno);
        return;
    }
    errno = 0;
    if (llam_mutex_lock(state->destroy_mutex) != -1 || errno != EDEADLK) {
        task_fail(state, "recursive mutex lock", errno);
        (void)llam_mutex_unlock(state->destroy_mutex);
        return;
    }
    errno = 0;
    if (llam_mutex_lock_until(state->destroy_mutex, llam_now_ns() + 1000000ULL) != -1 || errno != EDEADLK) {
        task_fail(state, "recursive timed mutex lock", errno);
        (void)llam_mutex_unlock(state->destroy_mutex);
        return;
    }
    errno = 0;
    if (llam_mutex_trylock(state->destroy_mutex) != -1 || errno != EBUSY) {
        task_fail(state, "recursive mutex trylock", errno);
        (void)llam_mutex_unlock(state->destroy_mutex);
        return;
    }
    errno = 0;
    if (llam_mutex_destroy(state->destroy_mutex) != -1 || errno != EBUSY) {
        task_fail(state, "busy mutex destroy", errno);
        (void)llam_mutex_unlock(state->destroy_mutex);
        return;
    }
    if (llam_mutex_unlock(state->destroy_mutex) != 0) {
        task_fail(state, "destroy mutex unlock", errno);
        return;
    }
    if (llam_mutex_destroy(state->destroy_mutex) != 0) {
        task_fail(state, "idle mutex destroy", errno);
        return;
    }
    state->destroy_mutex = NULL;

    if (llam_channel_send(state->busy_channel, (void *)(uintptr_t)7U) != 0) {
        task_fail(state, "busy channel send", errno);
        return;
    }
    errno = 0;
    if (llam_channel_destroy(state->busy_channel) != -1 || errno != EBUSY) {
        task_fail(state, "buffered channel destroy", errno);
        return;
    }
    if (llam_channel_recv_result(state->busy_channel, &value) != 0 ||
        value != (void *)(uintptr_t)7U) {
        task_fail(state, "busy channel drain", errno);
        return;
    }
    if (llam_channel_destroy(state->busy_channel) != 0) {
        task_fail(state, "idle channel destroy", errno);
        return;
    }
    state->busy_channel = NULL;

    if (llam_cond_destroy(state->destroy_cond) != 0) {
        task_fail(state, "idle cond destroy", errno);
        return;
    }
    state->destroy_cond = NULL;
    atomic_fetch_add_explicit(&state->destroy_checks, 1U, memory_order_relaxed);
}

static void busy_cond_waiter_task(void *arg) {
    sync_state_t *state = arg;

    if (llam_mutex_lock(state->busy_cond_mutex) != 0) {
        task_fail(state, "busy cond waiter mutex lock", errno);
        return;
    }
    atomic_store_explicit(&state->busy_cond_waiting, 1U, memory_order_release);
    while (atomic_load_explicit(&state->busy_cond_release, memory_order_acquire) == 0U) {
        if (llam_cond_wait(state->busy_cond, state->busy_cond_mutex) != 0) {
            task_fail(state, "busy cond wait", errno);
            (void)llam_mutex_unlock(state->busy_cond_mutex);
            return;
        }
    }
    if (llam_mutex_unlock(state->busy_cond_mutex) != 0) {
        task_fail(state, "busy cond waiter mutex unlock", errno);
    }
}

static void busy_cond_destroyer_task(void *arg) {
    sync_state_t *state = arg;
    unsigned i;

    while (atomic_load_explicit(&state->busy_cond_waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (i = 0U; i < 4U; ++i) {
        llam_yield();
    }
    errno = 0;
    if (llam_cond_destroy(state->busy_cond) != -1 || errno != EBUSY) {
        task_fail(state, "busy cond destroy", errno);
        return;
    }
    atomic_store_explicit(&state->busy_cond_destroy_checked, 1U, memory_order_release);
    if (llam_mutex_lock(state->busy_cond_mutex) != 0) {
        task_fail(state, "busy cond destroyer mutex lock", errno);
        return;
    }
    atomic_store_explicit(&state->busy_cond_release, 1U, memory_order_release);
    if (llam_cond_signal(state->busy_cond) != 0) {
        task_fail(state, "busy cond signal", errno);
        (void)llam_mutex_unlock(state->busy_cond_mutex);
        return;
    }
    if (llam_mutex_unlock(state->busy_cond_mutex) != 0) {
        task_fail(state, "busy cond destroyer mutex unlock", errno);
    }
}

static void select_task(void *arg) {
    sync_state_t *state = arg;
    void *received = NULL;
    size_t selected = SIZE_MAX;
    llam_select_op_t recv_ops[2];
    llam_select_op_t send_ops[2];

    if (llam_channel_send(state->select_recv_channel, (void *)(uintptr_t)23U) != 0) {
        task_fail(state, "select fixture send", errno);
        return;
    }

    memset(recv_ops, 0, sizeof(recv_ops));
    recv_ops[0].kind = LLAM_SELECT_OP_RECV;
    recv_ops[0].channel = state->select_recv_channel;
    recv_ops[0].recv_out = &received;
    recv_ops[1].kind = LLAM_SELECT_OP_RECV;
    recv_ops[1].channel = state->empty_channel;
    recv_ops[1].recv_out = &received;
    if (llam_channel_select(recv_ops, 2U, llam_now_ns() + 10000000ULL, &selected) != 0 ||
        selected != 0U ||
        received != (void *)(uintptr_t)23U ||
        recv_ops[0].result_errno != 0) {
        task_fail(state, "channel select recv", errno);
        return;
    }

    memset(send_ops, 0, sizeof(send_ops));
    send_ops[0].kind = LLAM_SELECT_OP_SEND;
    if (llam_channel_send(state->select_full_channel, (void *)(uintptr_t)55U) != 0) {
        task_fail(state, "select full fixture send", errno);
        return;
    }

    send_ops[0].channel = state->select_full_channel;
    send_ops[0].send_value = (void *)(uintptr_t)77U;
    send_ops[1].kind = LLAM_SELECT_OP_SEND;
    send_ops[1].channel = state->select_send_channel;
    send_ops[1].send_value = (void *)(uintptr_t)31U;
    selected = SIZE_MAX;
    if (llam_channel_select(send_ops, 2U, llam_now_ns() + 10000000ULL, &selected) != 0 ||
        selected != 1U ||
        send_ops[1].result_errno != 0) {
        task_fail(state, "channel select send", errno);
        return;
    }
    received = llam_channel_recv(state->select_send_channel);
    if (received != (void *)(uintptr_t)31U) {
        task_fail(state, "channel select sent payload", errno);
        return;
    }
    received = llam_channel_recv(state->select_full_channel);
    if (received != (void *)(uintptr_t)55U) {
        task_fail(state, "channel select full drain", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->select_checks, 1U, memory_order_relaxed);
}

static void select_park_waiter_task(void *arg) {
    sync_state_t *state = arg;
    void *received = NULL;
    size_t selected = SIZE_MAX;
    llam_select_op_t recv_ops[2];

    memset(recv_ops, 0, sizeof(recv_ops));
    recv_ops[0].kind = LLAM_SELECT_OP_RECV;
    recv_ops[0].channel = state->select_park_channel;
    recv_ops[0].recv_out = &received;
    recv_ops[1].kind = LLAM_SELECT_OP_RECV;
    recv_ops[1].channel = state->select_empty_a;
    recv_ops[1].recv_out = &received;

    atomic_store_explicit(&state->select_park_waiting, 1U, memory_order_release);
    if (llam_channel_select(recv_ops, 2U, llam_now_ns() + 50000000ULL, &selected) != 0 ||
        selected != 0U ||
        received != (void *)(uintptr_t)99U ||
        recv_ops[0].result_errno != 0) {
        task_fail(state, "channel select parked recv", errno);
        return;
    }

    memset(recv_ops, 0, sizeof(recv_ops));
    recv_ops[0].kind = LLAM_SELECT_OP_RECV;
    recv_ops[0].channel = state->select_empty_a;
    recv_ops[0].recv_out = &received;
    recv_ops[1].kind = LLAM_SELECT_OP_RECV;
    recv_ops[1].channel = state->select_empty_b;
    recv_ops[1].recv_out = &received;
    errno = 0;
    if (llam_channel_select(recv_ops, 2U, llam_now_ns() + 1000000ULL, &selected) != -1 ||
        errno != ETIMEDOUT) {
        task_fail(state, "channel select parked timeout", errno);
        return;
    }

    atomic_fetch_add_explicit(&state->select_park_checks, 1U, memory_order_relaxed);
}

static void select_park_sender_task(void *arg) {
    sync_state_t *state = arg;
    unsigned i;

    while (atomic_load_explicit(&state->select_park_waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (i = 0U; i < 4U; ++i) {
        llam_yield();
    }
    if (llam_channel_send(state->select_park_channel, (void *)(uintptr_t)99U) != 0) {
        task_fail(state, "channel select parked sender", errno);
    }
}

static void task_local_task(void *arg) {
    sync_state_t *state = arg;
    uintptr_t value = 91U;

    if (llam_task_local_set(state->tls_key, (void *)value) != 0) {
        task_fail(state, "task local set", errno);
        return;
    }
    if (llam_task_local_get(state->tls_key) != (void *)value) {
        task_fail(state, "task local get", errno);
        return;
    }
    if (llam_task_local_set(state->tls_key, NULL) != 0) {
        task_fail(state, "task local clear", errno);
        return;
    }
    if (llam_task_local_get(state->tls_key) != NULL) {
        task_fail(state, "task local cleared get", EPROTO);
        return;
    }
    atomic_fetch_add_explicit(&state->tls_checks, 1U, memory_order_relaxed);
}

static void bounded_capacity_task(void *arg) {
    sync_state_t *state = arg;
    void *value;

    if (llam_channel_send(state->bounded3_channel, (void *)(uintptr_t)1U) != 0 ||
        llam_channel_send(state->bounded3_channel, (void *)(uintptr_t)2U) != 0 ||
        llam_channel_send(state->bounded3_channel, (void *)(uintptr_t)3U) != 0) {
        task_fail(state, "bounded capacity fill", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send_until(state->bounded3_channel, (void *)(uintptr_t)4U, 0U) != -1 ||
        errno != ETIMEDOUT) {
        task_fail(state, "bounded capacity exact full", errno);
        return;
    }
    value = llam_channel_recv(state->bounded3_channel);
    if (value != (void *)(uintptr_t)1U) {
        task_fail(state, "bounded capacity recv 1", errno);
        return;
    }
    value = llam_channel_recv(state->bounded3_channel);
    if (value != (void *)(uintptr_t)2U) {
        task_fail(state, "bounded capacity recv 2", errno);
        return;
    }
    value = llam_channel_recv(state->bounded3_channel);
    if (value != (void *)(uintptr_t)3U) {
        task_fail(state, "bounded capacity recv 3", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->bounded_capacity_checks, 1U, memory_order_relaxed);
}

static void group_task(void *arg) {
    sync_state_t *state = arg;

    atomic_fetch_add_explicit(&state->group_checks, 1U, memory_order_relaxed);
}

static void destroy_sync_state(sync_state_t *state) {
    if (state->tls_key != 0U && state->tls_key != LLAM_TASK_LOCAL_INVALID_KEY) {
        (void)llam_task_local_key_delete(state->tls_key);
        state->tls_key = LLAM_TASK_LOCAL_INVALID_KEY;
    }
    if (state->select_send_channel != NULL) {
        (void)llam_channel_destroy(state->select_send_channel);
    }
    if (state->select_full_channel != NULL) {
        (void)llam_channel_destroy(state->select_full_channel);
    }
    if (state->bounded3_channel != NULL) {
        (void)llam_channel_destroy(state->bounded3_channel);
    }
    if (state->select_recv_channel != NULL) {
        (void)llam_channel_destroy(state->select_recv_channel);
    }
    if (state->select_empty_b != NULL) {
        (void)llam_channel_destroy(state->select_empty_b);
    }
    if (state->select_empty_a != NULL) {
        (void)llam_channel_destroy(state->select_empty_a);
    }
    if (state->select_park_channel != NULL) {
        (void)llam_channel_destroy(state->select_park_channel);
    }
    if (state->busy_channel != NULL) {
        (void)llam_channel_destroy(state->busy_channel);
    }
    if (state->errno_response_channel != NULL) {
        (void)llam_channel_destroy(state->errno_response_channel);
    }
    if (state->errno_request_channel != NULL) {
        (void)llam_channel_destroy(state->errno_request_channel);
    }
    if (state->null_channel != NULL) {
        (void)llam_channel_destroy(state->null_channel);
    }
    if (state->full_channel != NULL) {
        (void)llam_channel_destroy(state->full_channel);
    }
    if (state->empty_channel != NULL) {
        (void)llam_channel_destroy(state->empty_channel);
    }
    if (state->channel != NULL) {
        (void)llam_channel_destroy(state->channel);
    }
    if (state->destroy_cond != NULL) {
        (void)llam_cond_destroy(state->destroy_cond);
    }
    if (state->busy_cond != NULL) {
        (void)llam_cond_destroy(state->busy_cond);
    }
    if (state->cond != NULL) {
        (void)llam_cond_destroy(state->cond);
    }
    if (state->destroy_mutex != NULL) {
        (void)llam_mutex_destroy(state->destroy_mutex);
    }
    if (state->busy_cond_mutex != NULL) {
        (void)llam_mutex_destroy(state->busy_cond_mutex);
    }
    if (state->cond_mutex != NULL) {
        (void)llam_mutex_destroy(state->cond_mutex);
    }
    if (state->counter_mutex != NULL) {
        (void)llam_mutex_destroy(state->counter_mutex);
    }
}

static int init_sync_state(sync_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->failures, 0U);
    atomic_init(&state->counter, 0U);
    atomic_init(&state->cond_waiting, 0U);
    atomic_init(&state->cond_ready, 0U);
    atomic_init(&state->cond_signaled, 0U);
    atomic_init(&state->busy_cond_waiting, 0U);
    atomic_init(&state->busy_cond_release, 0U);
    atomic_init(&state->busy_cond_destroy_checked, 0U);
    atomic_init(&state->errno_peer_waiting, 0U);
    atomic_init(&state->channel_sum, 0U);
    atomic_init(&state->timeout_checks, 0U);
    atomic_init(&state->null_checks, 0U);
    atomic_init(&state->errno_checks, 0U);
    atomic_init(&state->destroy_checks, 0U);
    atomic_init(&state->select_checks, 0U);
    atomic_init(&state->select_park_waiting, 0U);
    atomic_init(&state->select_park_checks, 0U);
    atomic_init(&state->tls_checks, 0U);
    atomic_init(&state->group_checks, 0U);
    atomic_init(&state->bounded_capacity_checks, 0U);
    state->tls_key = LLAM_TASK_LOCAL_INVALID_KEY;

    state->counter_mutex = llam_mutex_create();
    state->cond_mutex = llam_mutex_create();
    state->destroy_mutex = llam_mutex_create();
    state->busy_cond_mutex = llam_mutex_create();
    state->cond = llam_cond_create();
    state->destroy_cond = llam_cond_create();
    state->busy_cond = llam_cond_create();
    state->channel = llam_channel_create(2U);
    state->empty_channel = llam_channel_create(1U);
    state->full_channel = llam_channel_create(1U);
    state->null_channel = llam_channel_create(1U);
    state->busy_channel = llam_channel_create(1U);
    state->errno_request_channel = llam_channel_create(1U);
    state->errno_response_channel = llam_channel_create(1U);
    state->select_recv_channel = llam_channel_create(3U);
    state->select_send_channel = llam_channel_create(3U);
    state->select_full_channel = llam_channel_create(1U);
    state->select_park_channel = llam_channel_create(1U);
    state->select_empty_a = llam_channel_create(1U);
    state->select_empty_b = llam_channel_create(1U);
    state->bounded3_channel = llam_channel_create(3U);
    if (llam_task_local_key_create(&state->tls_key) != 0) {
        destroy_sync_state(state);
        return -1;
    }
    if (state->counter_mutex == NULL ||
        state->cond_mutex == NULL ||
        state->destroy_mutex == NULL ||
        state->busy_cond_mutex == NULL ||
        state->cond == NULL ||
        state->destroy_cond == NULL ||
        state->busy_cond == NULL ||
        state->channel == NULL ||
        state->empty_channel == NULL ||
        state->full_channel == NULL ||
        state->null_channel == NULL ||
        state->busy_channel == NULL ||
        state->errno_request_channel == NULL ||
        state->errno_response_channel == NULL ||
        state->select_recv_channel == NULL ||
        state->select_send_channel == NULL ||
        state->select_full_channel == NULL ||
        state->select_park_channel == NULL ||
        state->select_empty_a == NULL ||
        state->select_empty_b == NULL ||
        state->bounded3_channel == NULL) {
        destroy_sync_state(state);
        return -1;
    }
    return 0;
}

int main(void) {
    sync_state_t state;
    llam_runtime_opts_t opts;
    llam_mutex_t *idle_mutex;
    llam_cond_t *idle_cond;
    llam_channel_t *idle_channel;
    llam_task_group_t *group = NULL;

    errno = 0;
    if (llam_channel_create(0U) != NULL || errno != EINVAL) {
        return test_fail("llam_channel_create(0) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_mutex_destroy(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_mutex_destroy(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_cond_destroy(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_cond_destroy(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_channel_destroy(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_channel_destroy(NULL) did not fail with EINVAL");
    }
    idle_mutex = llam_mutex_create();
    idle_cond = llam_cond_create();
    idle_channel = llam_channel_create(1U);
    if (idle_mutex == NULL || idle_cond == NULL || idle_channel == NULL) {
        (void)llam_mutex_destroy(idle_mutex);
        (void)llam_cond_destroy(idle_cond);
        (void)llam_channel_destroy(idle_channel);
        return test_fail_errno("idle destroy fixture allocation failed");
    }
    if (llam_mutex_destroy(idle_mutex) != 0 ||
        llam_cond_destroy(idle_cond) != 0 ||
        llam_channel_destroy(idle_channel) != 0) {
        return test_fail_errno("idle destroy failed");
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("llam_runtime_init failed");
    }
    if (init_sync_state(&state) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("sync state initialization failed");
    }
    group = llam_task_group_create();
    if (group == NULL) {
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_task_group_create failed");
    }
    if (llam_task_group_spawn(group, group_task, &state, NULL) == NULL ||
        llam_task_group_spawn(group, group_task, &state, NULL) == NULL) {
        (void)llam_task_group_destroy(group);
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_task_group_spawn failed");
    }
    errno = 0;
    if (llam_task_group_join_until(group, llam_now_ns()) != -1 || errno != ETIMEDOUT) {
        (void)llam_task_group_destroy(group);
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail("llam_task_group_join_until did not preserve pending children on timeout");
    }
    errno = 0;
    if (llam_task_group_destroy(group) != -1 || errno != EBUSY) {
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail("busy task group destroy did not fail with EBUSY");
    }

    if (llam_spawn(counter_task, &state, NULL) == NULL ||
        llam_spawn(counter_task, &state, NULL) == NULL ||
        llam_spawn(cond_waiter_task, &state, NULL) == NULL ||
        llam_spawn(cond_signaler_task, &state, NULL) == NULL ||
        llam_spawn(channel_sender_task, &state, NULL) == NULL ||
        llam_spawn(channel_receiver_task, &state, NULL) == NULL ||
        llam_spawn(timeout_and_close_task, &state, NULL) == NULL ||
        llam_spawn(null_channel_task, &state, NULL) == NULL ||
        llam_spawn(channel_errno_peer_task, &state, NULL) == NULL ||
        llam_spawn(channel_errno_sender_task, &state, NULL) == NULL ||
        llam_spawn(destroy_contract_task, &state, NULL) == NULL ||
        llam_spawn(busy_cond_waiter_task, &state, NULL) == NULL ||
        llam_spawn(busy_cond_destroyer_task, &state, NULL) == NULL ||
        llam_spawn(select_task, &state, NULL) == NULL ||
        llam_spawn(select_park_waiter_task, &state, NULL) == NULL ||
        llam_spawn(select_park_sender_task, &state, NULL) == NULL ||
        llam_spawn(task_local_task, &state, NULL) == NULL ||
        llam_spawn(bounded_capacity_task, &state, NULL) == NULL) {
        (void)llam_task_group_destroy(group);
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn failed");
    }
    if (llam_run() != 0) {
        (void)llam_task_group_destroy(group);
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_run failed");
    }
    if (llam_task_group_join(group) != 0) {
        (void)llam_task_group_destroy(group);
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_task_group_join failed");
    }
    if (llam_task_group_destroy(group) != 0) {
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail_errno("llam_task_group_destroy failed");
    }

    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_sync_primitives] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.counter, memory_order_relaxed) != 200U ||
        atomic_load_explicit(&state.cond_signaled, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.busy_cond_destroy_checked, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.channel_sum, memory_order_relaxed) != 15U ||
        atomic_load_explicit(&state.timeout_checks, memory_order_relaxed) != 3U ||
        atomic_load_explicit(&state.null_checks, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.errno_checks, memory_order_relaxed) != 2U ||
        atomic_load_explicit(&state.destroy_checks, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.select_checks, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.select_park_checks, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.tls_checks, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.group_checks, memory_order_relaxed) != 2U ||
        atomic_load_explicit(&state.bounded_capacity_checks, memory_order_relaxed) != 1U) {
        destroy_sync_state(&state);
        llam_runtime_shutdown();
        return test_fail("sync primitive result counters were unexpected");
    }

    destroy_sync_state(&state);
    llam_runtime_shutdown();
    printf("[test_sync_primitives] ok\n");
    return 0;
}
