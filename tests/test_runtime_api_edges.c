/**
 * @file tests/test_runtime_api_edges.c
 * @brief Focused public API edge tests for lifecycle, ownership, cancellation, select, and boundaries.
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
#include "runtime_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !LLAM_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct edge_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint blocking_calls;
    atomic_uint blocking_started;
    atomic_uint blocking_can_finish;
    atomic_uint blocking_finished;
    atomic_uint blocking_returned_before_finish;
    atomic_uint canceled_waits;
    atomic_uint external_waiter_started;
    atomic_uint external_waiter_done;
    atomic_uint channel_destroy_waiter_started;
    atomic_uint channel_destroy_busy_checked;
    int first_errno;
    char first_case[160];
    llam_channel_t *primary;
    llam_channel_t *secondary;
    llam_channel_t *drain;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
#if !LLAM_PLATFORM_WINDOWS
    int io_fds[2];
#endif
} edge_state_t;

typedef struct task_handle_race_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint target_release;
    atomic_uint join_successes;
    atomic_uint join_busy;
    atomic_uint join_unexpected;
    atomic_uint detach_busy;
    atomic_uint detach_successes;
    int first_errno;
    char first_case[160];
    llam_task_t *target;
    llam_task_t *raw_target;
} task_handle_race_state_t;

typedef struct cancel_destroy_race_state {
    llam_cancel_token_t *token;
    atomic_uint ready;
    atomic_uint start;
    int cancel_rc;
    int cancel_errno;
    int destroy_rc;
    int destroy_errno;
} cancel_destroy_race_state_t;

typedef struct cond_owner_recheck_state {
    edge_state_t edge;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_mutex_t *raw_mutex;
    llam_cond_t *raw_cond;
    atomic_uint mutex_owned;
    atomic_uint cond_locked;
    atomic_uint stop;
    int wait_rc;
    int wait_errno;
} cond_owner_recheck_state_t;

typedef struct timer_heap_overflow_state {
    edge_state_t edge;
    size_t saved_len;
    size_t saved_cap;
    llam_timer_node_t **saved_heap;
    llam_timer_node_t *saved_root;
    unsigned saved_timer_count;
    int sleep_rc;
    int sleep_errno;
} timer_heap_overflow_state_t;

static void ownership_task(void *arg);

static void host_thread_yield(void) {
#if LLAM_PLATFORM_WINDOWS
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };

    (void)nanosleep(&ts, NULL);
#else
    (void)sched_yield();
#endif
}

#if !LLAM_PLATFORM_WINDOWS
typedef struct io_reorder_state {
    atomic_uint failures;
    atomic_uint reader_ready;
    atomic_uint read_successes;
    atomic_uint read_cancellations;
    llam_cancel_token_t *token;
    unsigned writer_delay;
    unsigned cancel_delay;
    int fds[2];
    int first_errno;
    char first_case[160];
} io_reorder_state_t;
#endif

static void cancel_destroy_race_wait_start(cancel_destroy_race_state_t *state) {
    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
        /*
         * These are host pthreads, not managed LLAM tasks. Yield to the host
         * scheduler so slow BSD VMs do not spend the race budget starving the
         * peer thread before the actual cancel/destroy interleaving begins.
         */
        host_thread_yield();
    }
}

static void *cancel_token_race_cancel_thread(void *arg) {
    cancel_destroy_race_state_t *state = arg;

    cancel_destroy_race_wait_start(state);
    state->cancel_rc = llam_cancel_token_cancel(state->token);
    state->cancel_errno = errno;
    return NULL;
}

static void *cancel_token_race_destroy_thread(void *arg) {
    cancel_destroy_race_state_t *state = arg;

    cancel_destroy_race_wait_start(state);
    state->destroy_rc = llam_cancel_token_destroy(state->token);
    state->destroy_errno = errno;
    return NULL;
}

static int fail_msg(const char *message) {
    fprintf(stderr, "[test_runtime_api_edges] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_api_edges] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(edge_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static uintptr_t public_handle_slot(const void *handle) {
    uintptr_t raw = (uintptr_t)handle;

    return (raw >> 32U) != 0U ? (raw >> 32U) - 1U : UINTPTR_MAX;
}

static uint32_t public_handle_generation(const void *handle) {
    uintptr_t raw = (uintptr_t)handle;

    return (uint32_t)raw;
}

static int check_task_failures(edge_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_api_edges] task failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

#if !LLAM_PLATFORM_WINDOWS
static void io_reorder_fail(io_reorder_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_io_reorder_failures(io_reorder_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_api_edges] io reorder failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

static void io_reorder_wait_ready(io_reorder_state_t *state) {
    while (atomic_load_explicit(&state->reader_ready, memory_order_acquire) == 0U) {
        llam_yield();
    }
}

static void io_reorder_delay(unsigned yields) {
    for (unsigned i = 0U; i < yields; ++i) {
        llam_yield();
    }
}
#endif

static int wait_until_atomic_nonzero(atomic_uint *flag, uint64_t timeout_ns) {
    uint64_t deadline = llam_now_ns() + timeout_ns;

    while (llam_now_ns() < deadline) {
        if (atomic_load_explicit(flag, memory_order_acquire) != 0U) {
            return 0;
        }
        {
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = 1000000L,
            };

            nanosleep(&ts, NULL);
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return llam_runtime_init(&opts);
}

static unsigned cancel_token_destroy_race_rounds(void) {
    const char *env = getenv("LLAM_API_EDGE_CANCEL_RACE_ROUNDS");
    char *end = NULL;
    unsigned long value;

    if (env == NULL || env[0] == '\0') {
        return 4000U;
    }
    errno = 0;
    value = strtoul(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' || value == 0UL || value > 4000UL) {
        return 4000U;
    }
    return (unsigned)value;
}

static int test_cancel_token_destroy_race(void) {
    const unsigned rounds = cancel_token_destroy_race_rounds();

    for (unsigned i = 0U; i < rounds; ++i) {
        cancel_destroy_race_state_t state;
        pthread_t canceler;
        pthread_t destroyer;

        memset(&state, 0, sizeof(state));
        atomic_init(&state.ready, 0U);
        atomic_init(&state.start, 0U);
        state.cancel_rc = -2;
        state.destroy_rc = -2;
        state.token = llam_cancel_token_create();
        if (state.token == NULL) {
            return fail_errno("cancel token race create failed");
        }

        if (pthread_create(&canceler, NULL, cancel_token_race_cancel_thread, &state) != 0 ||
            pthread_create(&destroyer, NULL, cancel_token_race_destroy_thread, &state) != 0) {
            return fail_errno("cancel token race pthread_create failed");
        }

        while (atomic_load_explicit(&state.ready, memory_order_acquire) != 2U) {
            host_thread_yield();
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        (void)pthread_join(canceler, NULL);
        (void)pthread_join(destroyer, NULL);

        if (!(state.cancel_rc == 0 ||
              (state.cancel_rc == -1 && state.cancel_errno == EINVAL))) {
            return fail_msg("cancel token race cancel returned unexpected result");
        }
        if (!(state.destroy_rc == 0 ||
              (state.destroy_rc == -1 && state.destroy_errno == EBUSY))) {
            return fail_msg("cancel token race destroy returned unexpected result");
        }

        /*
         * If cancellation retained the token long enough to make destroy return
         * EBUSY, the caller still owns the handle and must be able to destroy it
         * after the racing operation exits.
         */
        if (state.destroy_rc == -1 && llam_cancel_token_destroy(state.token) != 0) {
            return fail_errno("cancel token race cleanup destroy failed");
        }
    }
    return 0;
}

static int test_consumed_cancel_token_handle_reuse_guard(void) {
    llam_cancel_token_t *old_token;
    uintptr_t old_slot;
    unsigned attempt;

    old_token = llam_cancel_token_create();
    if (old_token == NULL) {
        return fail_errno("stale cancel token create old failed");
    }
    old_slot = public_handle_slot(old_token);
    if (llam_cancel_token_destroy(old_token) != 0) {
        return fail_errno("stale cancel token destroy old failed");
    }

    for (attempt = 0U; attempt < 64U; ++attempt) {
        llam_cancel_token_t *new_token = llam_cancel_token_create();

        if (new_token == NULL) {
            return fail_errno("stale cancel token create new failed");
        }
        if (public_handle_slot(new_token) == old_slot) {
            int cancelled;

            /*
             * Public cancellation-token handles are consumed by destroy.  A
             * stale handle must stay invalid across many slot generations so it
             * cannot cancel unrelated child tasks after allocator reuse.
             */
            errno = 0;
            if (llam_cancel_token_cancel(old_token) != -1 || errno != EINVAL) {
                (void)llam_cancel_token_destroy(new_token);
                return fail_msg("consumed cancel token handle cancelled a reused token");
            }
            errno = 0;
            cancelled = llam_cancel_token_is_cancelled(new_token);
            if (cancelled != 0 || errno != 0) {
                (void)llam_cancel_token_destroy(new_token);
                return fail_msg("reused cancel token observed stale cancellation");
            }
            if (llam_cancel_token_destroy(new_token) != 0) {
                return fail_errno("stale cancel token destroy new failed");
            }
            continue;
        }
        if (llam_cancel_token_destroy(new_token) != 0) {
            return fail_errno("stale cancel token destroy unused new failed");
        }
    }

    return 0;
}

static int test_cancel_token_task_refcount_overflow_guard(void) {
    llam_cancel_token_t *token;
    llam_cancel_token_t *raw_token = NULL;
    llam_cancel_token_t *extra_ref = NULL;
    unsigned observed_refcount;

    token = llam_cancel_token_create();
    if (token == NULL) {
        return fail_errno("cancel token refcount overflow create failed");
    }
    if (llam_cancel_token_retain_task_ref(token, &raw_token) != 0 || raw_token == NULL) {
        (void)llam_cancel_token_destroy(token);
        return fail_errno("cancel token refcount overflow initial retain failed");
    }

    pthread_mutex_lock(&raw_token->lock);
    raw_token->refcount = UINT_MAX;
    pthread_mutex_unlock(&raw_token->lock);

    /*
     * A saturated task-reference count is a corrupted/exhausted lifetime
     * state.  Retain must fail closed instead of wrapping to zero, otherwise a
     * concurrent destroy could free the token while task state still holds a
     * retained raw pointer.
     */
    errno = 0;
    if (llam_cancel_token_retain_task_ref(token, &extra_ref) != -1 || errno != EBUSY || extra_ref != NULL) {
        pthread_mutex_lock(&raw_token->lock);
        observed_refcount = raw_token->refcount;
        raw_token->refcount = 0U;
        pthread_mutex_unlock(&raw_token->lock);
        (void)llam_cancel_token_destroy(token);
        fprintf(stderr,
                "[test_runtime_api_edges] saturated cancel token retain returned success/errno=%d refcount=%u\n",
                errno,
                observed_refcount);
        return 1;
    }

    pthread_mutex_lock(&raw_token->lock);
    observed_refcount = raw_token->refcount;
    raw_token->refcount = 0U;
    pthread_mutex_unlock(&raw_token->lock);
    if (observed_refcount != UINT_MAX) {
        (void)llam_cancel_token_destroy(token);
        return fail_msg("cancel token saturated refcount was mutated after failed retain");
    }
    if (llam_cancel_token_destroy(token) != 0) {
        return fail_errno("cancel token refcount overflow cleanup destroy failed");
    }
    return 0;
}

static int test_consumed_task_group_handle_reuse_guard(void) {
    llam_task_group_t *old_group;
    uintptr_t old_slot;
    unsigned attempt;

    old_group = llam_task_group_create();
    if (old_group == NULL) {
        return fail_errno("stale task group create old failed");
    }
    old_slot = public_handle_slot(old_group);
    if (llam_task_group_destroy(old_group) != 0) {
        return fail_errno("stale task group destroy old failed");
    }

    for (attempt = 0U; attempt < 64U; ++attempt) {
        llam_task_group_t *new_group = llam_task_group_create();

        if (new_group == NULL) {
            return fail_errno("stale task group create new failed");
        }
        if (public_handle_slot(new_group) == old_slot) {
            /*
             * Public task-group handles are consumed by destroy.  A stale
             * handle must stay invalid across repeated slot reuse so it cannot
             * free an unrelated group still owned by the caller.
             */
            errno = 0;
            if (llam_task_group_destroy(old_group) != -1 || errno != EINVAL) {
                return fail_msg("consumed task group handle destroyed a reused group");
            }
            if (llam_task_group_destroy(new_group) != 0) {
                return fail_errno("stale task group destroy new failed");
            }
            continue;
        }
        if (llam_task_group_destroy(new_group) != 0) {
            return fail_errno("stale task group destroy unused new failed");
        }
    }

    return 0;
}

static int test_task_group_capacity_overflow_guard(void) {
    edge_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_task_group_t *group;
    llam_task_group_t *raw_group;
    size_t saved_count;
    size_t saved_capacity;
    llam_task_t **saved_tasks;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("task group overflow runtime opts init failed");
    }
    runtime_opts.deterministic = 1U;
    if (llam_runtime_init(&runtime_opts) != 0) {
        return fail_errno("task group overflow runtime init failed");
    }

    group = llam_task_group_create();
    if (group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group overflow create failed");
    }

    raw_group = llam_task_group_resolve_public_handle(group);
    if (raw_group == NULL) {
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return fail_errno("task group overflow resolve failed");
    }

    saved_count = raw_group->count;
    saved_capacity = raw_group->capacity;
    saved_tasks = raw_group->tasks;
    /*
     * The public API cannot practically allocate SIZE_MAX child handles, but a
     * corrupted or future-imported group state must still fail closed. Before the
     * overflow guard, group->count + 1 wrapped to zero, reserve was skipped, and
     * spawn completion wrote through group->tasks[SIZE_MAX].
     */
    raw_group->count = SIZE_MAX;
    raw_group->capacity = 0U;
    raw_group->tasks = NULL;
    llam_task_group_end_public_op(raw_group);

    errno = 0;
    if (llam_task_group_spawn(group, ownership_task, &state, NULL) != NULL || errno != ENOMEM) {
        raw_group = llam_task_group_resolve_public_handle(group);
        if (raw_group != NULL) {
            raw_group->count = saved_count;
            raw_group->capacity = saved_capacity;
            raw_group->tasks = saved_tasks;
            llam_task_group_end_public_op(raw_group);
        }
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return fail_msg("task group count overflow did not fail with ENOMEM");
    }

    raw_group = llam_task_group_resolve_public_handle(group);
    if (raw_group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group overflow restore resolve failed");
    }
    raw_group->count = saved_count;
    raw_group->capacity = saved_capacity;
    raw_group->tasks = saved_tasks;
    llam_task_group_end_public_op(raw_group);

    if (llam_task_group_destroy(group) != 0) {
        llam_runtime_shutdown();
        return fail_errno("task group overflow destroy failed");
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 0U) {
        return fail_msg("task group overflow unexpectedly spawned task");
    }
    return 0;
}

static int test_task_group_active_spawn_overflow_guard(void) {
    edge_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_task_group_t *group;
    llam_task_group_t *raw_group;
    size_t saved_active_spawns;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("task group active-spawn overflow runtime opts init failed");
    }
    runtime_opts.deterministic = 1U;
    if (llam_runtime_init(&runtime_opts) != 0) {
        return fail_errno("task group active-spawn overflow runtime init failed");
    }

    group = llam_task_group_create();
    if (group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group active-spawn overflow create failed");
    }

    raw_group = llam_task_group_resolve_public_handle(group);
    if (raw_group == NULL) {
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return fail_errno("task group active-spawn overflow resolve failed");
    }
    saved_active_spawns = raw_group->active_spawns;
    /*
     * active_spawns blocks group destruction while a spawn is outside
     * group->lock. If SIZE_MAX wraps to zero, a racing destroy can consume the
     * group while the spawn path still plans to relock and append the task.
     */
    raw_group->active_spawns = SIZE_MAX;
    llam_task_group_end_public_op(raw_group);

    errno = 0;
    {
        llam_task_t *spawned = llam_task_group_spawn(group, ownership_task, &state, NULL);

        if (spawned != NULL || errno != ENOMEM) {
            raw_group = llam_task_group_resolve_public_handle(group);
            if (raw_group != NULL) {
                raw_group->active_spawns = saved_active_spawns;
                /*
                 * A broken implementation may have appended @p spawned before
                 * returning. Remove the test artifact so failure reporting does
                 * not cascade through group destroy cleanup.
                 */
                if (spawned != NULL && raw_group->count > 0U && raw_group->tasks[raw_group->count - 1U] == spawned) {
                    raw_group->count -= 1U;
                }
                llam_task_group_end_public_op(raw_group);
            }
            if (spawned != NULL) {
                (void)llam_detach(spawned);
            }
            (void)llam_task_group_destroy(group);
            llam_runtime_shutdown();
            return fail_msg("task group active-spawn overflow did not fail with ENOMEM");
        }
    }

    raw_group = llam_task_group_resolve_public_handle(group);
    if (raw_group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group active-spawn overflow restore resolve failed");
    }
    raw_group->active_spawns = saved_active_spawns;
    llam_task_group_end_public_op(raw_group);

    if (llam_task_group_destroy(group) != 0) {
        llam_runtime_shutdown();
        return fail_errno("task group active-spawn overflow destroy failed");
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 0U) {
        return fail_msg("task group active-spawn overflow unexpectedly ran task");
    }
    return 0;
}

static int test_fault_boundary_contracts(void) {
    edge_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_stats_t stats;
    llam_task_group_t *group;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    errno = 0;
    if (llam_runtime_opts_init(NULL, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("runtime opts init NULL did not fail with EINVAL");
    }
    errno = 0;
    if (llam_spawn_opts_init(NULL, LLAM_SPAWN_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("spawn opts init NULL did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex(NULL, LLAM_RUNTIME_STATS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("stats NULL did not fail with EINVAL");
    }
    memset(&stats, 0xA5, sizeof(stats));
    errno = 0;
    if (llam_runtime_collect_stats_ex_handle(NULL,
                                             &stats,
                                             LLAM_RUNTIME_STATS_CURRENT_SIZE) != -1 ||
        errno != EINVAL ||
        stats.active_workers != 0U ||
        stats.online_workers != 0U ||
        stats.ctx_switches != 0U) {
        return fail_msg("NULL stats handle did not fail closed");
    }
    memset(&stats, 0xA5, sizeof(stats));
    errno = 0;
    if (llam_runtime_collect_stats_ex_handle((llam_runtime_t *)(uintptr_t)0x1234U,
                                             &stats,
                                             LLAM_RUNTIME_STATS_CURRENT_SIZE) != -1 ||
        errno != EINVAL ||
        stats.active_workers != 0U ||
        stats.online_workers != 0U ||
        stats.ctx_switches != 0U) {
        return fail_msg("invalid stats handle did not fail closed");
    }
    errno = 0;
    if (llam_channel_create(0U) != NULL || errno != EINVAL) {
        return fail_msg("channel create zero capacity did not fail with EINVAL");
    }
    errno = 0;
    if (llam_channel_create((SIZE_MAX / 2U) + 1U) != NULL || errno != ENOMEM) {
        return fail_msg("channel allocation-overflow boundary did not fail with ENOMEM");
    }
    errno = 0;
    if (llam_channel_create((SIZE_MAX / 2U) + 2U) != NULL || errno != ENOMEM) {
        return fail_msg("channel huge capacity did not fail with ENOMEM");
    }

    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("runtime opts init failed");
    }
    runtime_opts.profile = UINT32_MAX;
    errno = 0;
    if (llam_runtime_init_ex(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("runtime invalid profile did not fail with EINVAL");
    }

    if (init_runtime() != 0) {
        return fail_errno("runtime init after invalid profile failed");
    }
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0 ||
        stats.active_workers == 0U) {
        llam_runtime_shutdown();
        return fail_errno("stats after invalid-profile recovery failed");
    }

    if (llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return fail_errno("spawn opts init for invalid task class failed");
    }
    spawn_opts.task_class = UINT32_MAX;
    errno = 0;
    task = llam_spawn_ex(ownership_task, &state, &spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task != NULL || errno != EINVAL) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        llam_runtime_shutdown();
        return fail_msg("spawn invalid task class did not fail with EINVAL");
    }

    if (llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return fail_errno("spawn opts init for invalid stack class failed");
    }
    spawn_opts.stack_class = UINT32_MAX;
    errno = 0;
    task = llam_spawn_ex(ownership_task, &state, &spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task != NULL || errno != EINVAL) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        llam_runtime_shutdown();
        return fail_msg("spawn invalid stack class did not fail with EINVAL");
    }

    group = llam_task_group_create();
    if (group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group create for fault boundary failed");
    }
    errno = 0;
    if (llam_task_group_spawn_ex(group, ownership_task, &state, &spawn_opts, 0U) != NULL ||
        errno != EINVAL) {
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return fail_msg("group spawn non-NULL opts with zero size did not fail with EINVAL");
    }
    if (llam_task_group_destroy(group) != 0) {
        llam_runtime_shutdown();
        return fail_errno("task group destroy after fault boundary failed");
    }
    llam_runtime_shutdown();
    return 0;
}

#if !LLAM_PLATFORM_WINDOWS
static int expect_child_sigabrt(void (*child_fn)(void), const char *label) {
    pid_t pid;
    pid_t waited;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        return fail_errno(label);
    }
    if (pid == 0) {
        child_fn();
        _exit(0);
    }

    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) {
        return fail_errno(label);
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        return fail_msg(label);
    }
    return 0;
}

static void bootstrap_null_task_child(void) {
    /*
     * This internal entry point is normally reached only from a prepared fiber
     * context.  Corrupt cursors must fail closed with SIGABRT rather than
     * falling through to a NULL task callback and SIGSEGV.
     */
    llam_task_bootstrap(NULL);
}

static void bootstrap_null_entry_child(void) {
    llam_shard_t fake_shard;
    llam_task_t fake_task;

    memset(&fake_shard, 0, sizeof(fake_shard));
    memset(&fake_task, 0, sizeof(fake_task));
    fake_shard.runtime = llam_runtime_default_storage();
    fake_task.owner_runtime = fake_shard.runtime;
    g_llam_tls_shard = &fake_shard;
    llam_task_bootstrap(&fake_task);
}

static void task_exit_null_owner_child(void) {
    llam_shard_t fake_shard;
    llam_task_t fake_task;

    memset(&fake_shard, 0, sizeof(fake_shard));
    memset(&fake_task, 0, sizeof(fake_task));
    fake_shard.runtime = llam_runtime_default_storage();
    g_llam_tls_shard = &fake_shard;
    g_llam_tls_task = &fake_task;
    llam_task_exit_internal();
}

static void task_exit_owner_mismatch_child(void) {
    llam_runtime_t foreign_runtime;
    llam_shard_t fake_shard;
    llam_task_t fake_task;

    memset(&foreign_runtime, 0, sizeof(foreign_runtime));
    memset(&fake_shard, 0, sizeof(fake_shard));
    memset(&fake_task, 0, sizeof(fake_task));
    fake_shard.runtime = llam_runtime_default_storage();
    fake_task.owner_runtime = &foreign_runtime;
    g_llam_tls_shard = &fake_shard;
    g_llam_tls_task = &fake_task;
    llam_task_exit_internal();
}

static int test_task_bootstrap_invariant_fails_closed(void) {
    if (expect_child_sigabrt(bootstrap_null_task_child,
                             "task bootstrap NULL task did not fail closed with SIGABRT") != 0) {
        return -1;
    }
    if (expect_child_sigabrt(bootstrap_null_entry_child,
                             "task bootstrap NULL entry did not fail closed with SIGABRT") != 0) {
        return -1;
    }
    if (expect_child_sigabrt(task_exit_null_owner_child,
                             "task exit NULL owner did not fail closed with SIGABRT") != 0) {
        return -1;
    }
    if (expect_child_sigabrt(task_exit_owner_mismatch_child,
                             "task exit owner mismatch did not fail closed with SIGABRT") != 0) {
        return -1;
    }
    return 0;
}
#endif

static void *blocking_echo(void *arg) {
    edge_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void *blocking_null(void *arg) {
    edge_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return NULL;
}

static void *blocking_delayed(void *arg) {
    edge_state_t *state = arg;

    atomic_store_explicit(&state->blocking_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->blocking_can_finish, memory_order_acquire) == 0U) {
        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };

        nanosleep(&ts, NULL);
    }
    atomic_store_explicit(&state->blocking_finished, 1U, memory_order_release);
    return state;
}

static void ownership_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_sleep_ns(1000000ULL) != 0) {
        task_fail(state, "ownership task sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void task_handle_race_fail(task_handle_race_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_task_handle_race_failures(task_handle_race_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_api_edges] task handle race failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

static void task_handle_race_target_task(void *arg) {
    task_handle_race_state_t *state = arg;

    while (atomic_load_explicit(&state->target_release, memory_order_acquire) == 0U) {
        llam_yield();
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void task_handle_race_joiner_task(void *arg) {
    task_handle_race_state_t *state = arg;

    errno = 0;
    if (llam_join(state->target) == 0) {
        atomic_fetch_add_explicit(&state->join_successes, 1U, memory_order_relaxed);
        return;
    }
    if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->join_busy, 1U, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&state->join_unexpected, 1U, memory_order_relaxed);
    task_handle_race_fail(state, "duplicate join returned unexpected errno", errno);
}

static void task_handle_race_detacher_task(void *arg) {
    task_handle_race_state_t *state = arg;

    /*
     * Wait until the joiner is parked on the raw target's join waiter list.
     * That makes this a deterministic join-vs-detach ownership test rather
     * than a scheduler-order lottery where detach may legitimately win first.
     */
    while (state->raw_target != NULL) {
        bool waiter_ready;

        pthread_mutex_lock(&state->raw_target->lock);
        waiter_ready = state->raw_target->join_waiter_count > 0U;
        pthread_mutex_unlock(&state->raw_target->lock);
        if (waiter_ready) {
            break;
        }
        llam_yield();
    }
    errno = 0;
    if (llam_detach(state->target) == 0) {
        atomic_fetch_add_explicit(&state->detach_successes, 1U, memory_order_relaxed);
        return;
    }
    if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->detach_busy, 1U, memory_order_relaxed);
        return;
    }
    task_handle_race_fail(state, "detach versus join returned unexpected errno", errno);
}

static void task_handle_race_release_after_busy_task(void *arg) {
    task_handle_race_state_t *state = arg;

    while (atomic_load_explicit(&state->join_busy, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->detach_busy, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->detach_successes, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->failures, memory_order_acquire) == 0U) {
        llam_yield();
    }
    atomic_store_explicit(&state->target_release, 1U, memory_order_release);
}

static void stale_channel_handle_task(void *arg) {
    edge_state_t *state = arg;
    int value = 7;
    void *out = NULL;
    uintptr_t old_slot;
    unsigned attempt;

    state->primary = llam_channel_create(1U);
    if (state->primary == NULL) {
        task_fail(state, "stale channel create old", errno);
        return;
    }
    if (llam_channel_destroy(state->primary) != 0) {
        task_fail(state, "stale channel destroy old", errno);
        return;
    }
    old_slot = public_handle_slot(state->primary);
    state->secondary = llam_channel_create(1U);
    if (state->secondary == NULL) {
        task_fail(state, "stale channel create new", errno);
        return;
    }

    /*
     * Capacity-one channels are cache-reused inside managed tasks.  The old
     * consumed handle must not send into the newly acquired channel object.
     */
    errno = 0;
    if (llam_channel_try_send(state->primary, &value) != -1 || errno != EINVAL) {
        task_fail(state, "stale channel send accepted", errno);
        return;
    }
    errno = 0;
    if (llam_channel_try_recv_result(state->secondary, &out) != -1 || errno != EAGAIN || out != NULL) {
        task_fail(state, "stale channel polluted new channel", errno);
        return;
    }
    {
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0x51E7EC7U;
        llam_select_op_t ops[2] = {
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->secondary,
                .recv_out = &out,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->primary,
                .recv_out = &out,
            },
        };

        /*
         * The select fast path validates every public handle before consuming
         * any ready operation. A stale later operand must reject the whole call,
         * leave a ready earlier operand buffered, and release any valid handle
         * pin before returning. Otherwise forged/stale handles could influence
         * select results or make the valid channel below fail destroy with a
         * false EBUSY.
         */
        if (llam_channel_try_send(state->secondary, ready) != 0) {
            task_fail(state, "stale channel select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != EINVAL) {
            task_fail(state, "stale channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            task_fail(state, "stale channel select consumed ready operand", errno);
            return;
        }
    }
    {
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0x51E7EC8U;
        llam_select_op_t ops[2] = {
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->primary,
                .recv_out = &out,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->secondary,
                .recv_out = &out,
            },
        };

        /*
         * Invalid-first ordering must also fail before later ready operations
         * are examined. This guards future fast-path rewrites that might scan
         * for readiness before validating every public handle in the batch.
         */
        if (llam_channel_try_send(state->secondary, ready) != 0) {
            task_fail(state, "stale-first select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != EINVAL) {
            task_fail(state, "stale-first channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            task_fail(state, "stale-first channel select consumed ready operand", errno);
            return;
        }
    }
    {
        size_t selected = SIZE_MAX;
        void *sentinel = (void *)(uintptr_t)0x5E11DADU;
        llam_select_op_t ops[2] = {
            {
                .kind = LLAM_SELECT_OP_SEND,
                .channel = state->secondary,
                .send_value = sentinel,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->primary,
                .recv_out = &out,
            },
        };

        /*
         * The same validation-before-effect rule applies to send-ready ops.
         * A later stale handle must not let select enqueue a payload into an
         * earlier valid channel before the call fails.
         */
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != EINVAL) {
            task_fail(state, "stale channel select send-ready accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != -1 || errno != EAGAIN || out != NULL) {
            task_fail(state, "stale channel select inserted send-ready payload", errno);
            return;
        }
    }
    {
        llam_mutex_t *wrong_family = llam_mutex_create();
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0xFA117A6U;
        llam_select_op_t ops[2] = {
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->secondary,
                .recv_out = &out,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = (llam_channel_t *)(void *)wrong_family,
                .recv_out = &out,
            },
        };

        if (wrong_family == NULL) {
            task_fail(state, "wrong-family select mutex fixture", errno);
            return;
        }
        /*
         * Wrong-family opaque handles must fail before any ready operation has
         * effects. This pins the real channel/mutex family tags to the select
         * fast path instead of only exercising the standalone slot helper.
         */
        if (llam_channel_try_send(state->secondary, ready) != 0) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != EINVAL) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family channel select consumed ready operand", errno);
            return;
        }
        if (llam_mutex_destroy(wrong_family) != 0) {
            task_fail(state, "wrong-family select mutex destroy", errno);
            return;
        }
    }
    {
        llam_mutex_t *wrong_family = llam_mutex_create();
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0xFA117A7U;
        llam_select_op_t ops[2] = {
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = (llam_channel_t *)(void *)wrong_family,
                .recv_out = &out,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->secondary,
                .recv_out = &out,
            },
        };

        if (wrong_family == NULL) {
            task_fail(state, "wrong-family-first select mutex fixture", errno);
            return;
        }
        if (llam_channel_try_send(state->secondary, ready) != 0) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family-first select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != EINVAL) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family-first channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            (void)llam_mutex_destroy(wrong_family);
            task_fail(state, "wrong-family-first channel select consumed ready operand", errno);
            return;
        }
        if (llam_mutex_destroy(wrong_family) != 0) {
            task_fail(state, "wrong-family-first select mutex destroy", errno);
            return;
        }
    }
    {
        llam_channel_t *tertiary = llam_channel_create(1U);
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0xBA7C4EDU;
        void *sentinel = (void *)(uintptr_t)0xBA7C5EADU;
        llam_select_op_t ops[3] = {
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->secondary,
                .recv_out = &out,
            },
            {
                .kind = LLAM_SELECT_OP_SEND,
                .channel = tertiary,
                .send_value = sentinel,
            },
            {
                .kind = LLAM_SELECT_OP_RECV,
                .channel = state->primary,
                .recv_out = &out,
            },
        };

        if (tertiary == NULL) {
            task_fail(state, "three-op select tertiary fixture", errno);
            return;
        }
        /*
         * op_count > 2 uses the generic batch resolver instead of the two-op
         * registry fast path. Keep the same validation-before-effect invariant
         * covered there: a later stale handle must not consume an earlier ready
         * receive or enqueue an earlier ready send, and any temporary active-op
         * pins must be released before destroy.
         */
        if (llam_channel_try_send(state->secondary, ready) != 0) {
            (void)llam_channel_destroy(tertiary);
            task_fail(state, "three-op stale select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, 3U, 0U, &selected) != -1 || errno != EINVAL) {
            (void)llam_channel_destroy(tertiary);
            task_fail(state, "three-op stale channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            (void)llam_channel_destroy(tertiary);
            task_fail(state, "three-op stale channel select consumed ready operand", errno);
            return;
        }
        out = NULL;
        errno = 0;
        if (llam_channel_try_recv_result(tertiary, &out) != -1 || errno != EAGAIN || out != NULL) {
            (void)llam_channel_destroy(tertiary);
            task_fail(state, "three-op stale channel select inserted send-ready payload", errno);
            return;
        }
        if (llam_channel_destroy(tertiary) != 0) {
            task_fail(state, "three-op select tertiary destroy", errno);
            return;
        }
    }
    {
        size_t selected = SIZE_MAX;
        void *ready = (void *)(uintptr_t)0x5E1EC75U;
        llam_select_op_t ops[LLAM_CHANNEL_SELECT_INLINE_OPS + 1U];
        size_t i;

        /*
         * Large selects skip the inline batch fast path. They must still
         * validate every public handle before probing readiness; otherwise a
         * forged/stale tail operand can be hidden behind an earlier ready op.
         */
        for (i = 0U; i < LLAM_CHANNEL_SELECT_INLINE_OPS; ++i) {
            ops[i].kind = LLAM_SELECT_OP_RECV;
            ops[i].channel = state->secondary;
            ops[i].recv_out = &out;
            ops[i].send_value = NULL;
            ops[i].result_errno = 0;
        }
        ops[LLAM_CHANNEL_SELECT_INLINE_OPS].kind = LLAM_SELECT_OP_RECV;
        ops[LLAM_CHANNEL_SELECT_INLINE_OPS].channel = state->primary;
        ops[LLAM_CHANNEL_SELECT_INLINE_OPS].recv_out = &out;
        ops[LLAM_CHANNEL_SELECT_INLINE_OPS].send_value = NULL;
        ops[LLAM_CHANNEL_SELECT_INLINE_OPS].result_errno = 0;

        if (llam_channel_try_send(state->secondary, ready) != 0) {
            task_fail(state, "large stale select ready fixture send", errno);
            return;
        }
        errno = 0;
        out = NULL;
        if (llam_channel_select(ops, LLAM_CHANNEL_SELECT_INLINE_OPS + 1U, 0U, &selected) != -1 ||
            errno != EINVAL) {
            task_fail(state, "large stale channel select accepted", errno);
            return;
        }
        if (llam_channel_try_recv_result(state->secondary, &out) != 0 || out != ready) {
            task_fail(state, "large stale channel select consumed ready operand", errno);
            return;
        }
    }
    if (llam_channel_destroy(state->secondary) != 0) {
        task_fail(state, "stale channel destroy first new", errno);
        return;
    }

    /*
     * The previous low-bit channel tag wrapped after a small number of cache
     * reuses. Recycle the same public slot repeatedly and verify the original
     * consumed handle stays invalid across many generations.
     */
    state->secondary = NULL;
    for (attempt = 0U; attempt < 64U; ++attempt) {
        llam_channel_t *next = llam_channel_create(1U);

        if (next == NULL) {
            task_fail(state, "stale channel create wrap candidate", errno);
            return;
        }
        if (public_handle_slot(next) == old_slot) {
            errno = 0;
            if (llam_channel_try_send(state->primary, &value) != -1 || errno != EINVAL) {
                task_fail(state, "stale channel wrap send accepted", errno);
                return;
            }
        }
        if (llam_channel_destroy(next) != 0) {
            task_fail(state, "stale channel destroy wrap candidate", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stale_sync_handle_task(void *arg) {
    edge_state_t *state = arg;
    llam_mutex_t *old_mutex = NULL;
    llam_cond_t *old_cond = NULL;
    uintptr_t old_slot;
    unsigned attempt;

    old_mutex = llam_mutex_create();
    if (old_mutex == NULL) {
        task_fail(state, "stale mutex create old", errno);
        return;
    }
    old_slot = public_handle_slot(old_mutex);
    if (llam_mutex_destroy(old_mutex) != 0) {
        task_fail(state, "stale mutex destroy old", errno);
        return;
    }
    for (attempt = 0U; attempt < 64U; ++attempt) {
        llam_mutex_t *new_mutex = llam_mutex_create();

        if (new_mutex == NULL) {
            task_fail(state, "stale mutex create new", errno);
            return;
        }
        if (public_handle_slot(new_mutex) == old_slot) {
            errno = 0;
            if (llam_mutex_trylock(old_mutex) != -1 || errno != EINVAL) {
                task_fail(state, "stale mutex trylock accepted", errno);
                return;
            }
        }
        if (llam_mutex_destroy(new_mutex) != 0) {
            task_fail(state, "stale mutex destroy new", errno);
            return;
        }
    }

    old_cond = llam_cond_create();
    if (old_cond == NULL) {
        task_fail(state, "stale cond create old", errno);
        return;
    }
    old_slot = public_handle_slot(old_cond);
    if (llam_cond_destroy(old_cond) != 0) {
        task_fail(state, "stale cond destroy old", errno);
        return;
    }
    for (attempt = 0U; attempt < 64U; ++attempt) {
        llam_cond_t *new_cond = llam_cond_create();

        if (new_cond == NULL) {
            task_fail(state, "stale cond create new", errno);
            return;
        }
        if (public_handle_slot(new_cond) == old_slot) {
            errno = 0;
            if (llam_cond_signal(old_cond) != -1 || errno != EINVAL) {
                task_fail(state, "stale cond signal accepted", errno);
                return;
            }
        }
        if (llam_cond_destroy(new_cond) != 0) {
            task_fail(state, "stale cond destroy new", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_task(void *arg) {
    edge_state_t *state = arg;
    void *result = NULL;

    result = state;
    errno = 0;
    if (llam_call_blocking_result(NULL, state, &result) != -1 ||
        errno != EINVAL ||
        result != NULL) {
        task_fail(state, "managed blocking NULL fn", errno);
        return;
    }
    errno = 0;
    if (llam_call_blocking_result(blocking_echo, state, NULL) != -1 || errno != EINVAL) {
        task_fail(state, "managed blocking NULL out", errno);
        return;
    }
    if (llam_call_blocking_result(blocking_echo, state, &result) != 0 || result != state) {
        task_fail(state, "managed blocking echo", errno);
        return;
    }
    result = state;
    if (llam_call_blocking_result(blocking_null, state, &result) != 0 || result != NULL) {
        task_fail(state, "managed blocking NULL result", errno);
        return;
    }

    if (llam_enter_blocking() != 0 ||
        llam_enter_blocking() != 0 ||
        llam_leave_blocking() != 0 ||
        llam_leave_blocking() != 0) {
        task_fail(state, "managed nested opaque blocking", errno);
        return;
    }
    errno = 0;
    if (llam_leave_blocking() != -1 || errno != EINVAL) {
        task_fail(state, "managed unmatched leave blocking", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_cancel_caller_task(void *arg) {
    edge_state_t *state = arg;
    void *result = NULL;
    int rc;

    rc = llam_call_blocking_result(blocking_delayed, state, &result);
    if (atomic_load_explicit(&state->blocking_finished, memory_order_acquire) == 0U) {
        task_fail(state, "blocking cancellation returned before callback finished", EBUSY);
        atomic_store_explicit(&state->blocking_returned_before_finish, 1U, memory_order_release);
        return;
    }
    if (rc != 0 && errno != ECANCELED) {
        task_fail(state, "blocking cancellation unexpected errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_cancel_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->blocking_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->token) != 0) {
        task_fail(state, "blocking cancel token cancel", errno);
    }
}

static void blocking_release_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->blocking_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_sleep_ns(20ULL * 1000ULL * 1000ULL) != 0) {
        task_fail(state, "blocking release sleep", errno);
        return;
    }
    atomic_store_explicit(&state->blocking_can_finish, 1U, memory_order_release);
}

#if !LLAM_PLATFORM_WINDOWS
static void io_reorder_reader_task(void *arg) {
    io_reorder_state_t *state = arg;
    unsigned char byte = 0U;
    ssize_t rc;

    atomic_store_explicit(&state->reader_ready, 1U, memory_order_release);
    errno = 0;
    rc = llam_read_when_ready(state->fds[0], &byte, 1U, -1);
    if (rc == 1 && byte == 0x5aU) {
        atomic_fetch_add_explicit(&state->read_successes, 1U, memory_order_relaxed);
        return;
    }
    if (rc == -1 && errno == ECANCELED) {
        atomic_fetch_add_explicit(&state->read_cancellations, 1U, memory_order_relaxed);
        return;
    }
    io_reorder_fail(state, "read completion/cancel result", errno);
}

static void io_reorder_writer_task(void *arg) {
    io_reorder_state_t *state = arg;
    unsigned char byte = 0x5aU;
    ssize_t written;

    io_reorder_wait_ready(state);
    io_reorder_delay(state->writer_delay);
    do {
        written = write(state->fds[1], &byte, 1U);
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
        io_reorder_fail(state, "write completion trigger", errno);
    }
}

static void io_reorder_cancel_task(void *arg) {
    io_reorder_state_t *state = arg;

    io_reorder_wait_ready(state);
    io_reorder_delay(state->cancel_delay);
    if (llam_cancel_token_cancel(state->token) != 0) {
        io_reorder_fail(state, "cancel completion trigger", errno);
    }
}
#endif

static void channel_close_select_task(void *arg) {
    edge_state_t *state = arg;
    void *received = (void *)(uintptr_t)0xBADU;
    size_t selected = SIZE_MAX;
    llam_select_op_t ops[2];

    if (llam_channel_send(state->drain, NULL) != 0 ||
        llam_channel_send(state->drain, (void *)(uintptr_t)0xC105EU) != 0) {
        task_fail(state, "channel close buffered send", errno);
        return;
    }
    if (llam_channel_close(state->drain) != 0 ||
        llam_channel_close(state->drain) != 0) {
        task_fail(state, "channel close idempotent", errno);
        return;
    }
    if (llam_channel_recv_result(state->drain, &received) != 0 || received != NULL) {
        task_fail(state, "channel drain NULL payload", errno);
        return;
    }
    received = NULL;
    if (llam_channel_recv_result(state->drain, &received) != 0 ||
        received != (void *)(uintptr_t)0xC105EU) {
        task_fail(state, "channel drain value payload", errno);
        return;
    }
    errno = 0;
    if (llam_channel_recv_result(state->drain, &received) != -1 || errno != EPIPE) {
        task_fail(state, "channel recv after closed drain", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send(state->drain, (void *)(uintptr_t)1U) != -1 || errno != EPIPE) {
        task_fail(state, "channel send after close", errno);
        return;
    }

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;
    errno = 0;
    if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "channel select immediate timeout", errno);
        return;
    }

    if (llam_channel_close(state->secondary) != 0) {
        task_fail(state, "channel select close fixture", errno);
        return;
    }
    received = (void *)(uintptr_t)0xBAD0BAD0U;
    selected = SIZE_MAX;
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;
    if (llam_channel_select(ops, 2U, UINT64_MAX, &selected) != 0 ||
        selected != 1U ||
        ops[1].result_errno != EPIPE ||
        received != NULL) {
        task_fail(state, "channel select closed recv", errno);
        return;
    }

    if (llam_channel_try_send(state->primary, &state) != 0) {
        task_fail(state, "channel select result errno fixture send", errno);
        return;
    }
    received = NULL;
    selected = SIZE_MAX;
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[0].result_errno = EPIPE;
    if (llam_channel_select(ops, 1U, UINT64_MAX, &selected) != 0 ||
        selected != 0U ||
        received != &state ||
        ops[0].result_errno != 0) {
        task_fail(state, "channel select ready recv kept stale result errno", errno);
        return;
    }
    {
        llam_select_op_t invalid_ops[3];

        /*
         * Reused FFI operation arrays often preserve old per-op errno fields.
         * Validation failure is not a selected terminal operation, so select
         * must clear every result_errno before returning instead of leaving a
         * stale tail status for bindings to misinterpret.
         */
        memset(invalid_ops, 0, sizeof(invalid_ops));
        invalid_ops[0].kind = LLAM_SELECT_OP_RECV;
        invalid_ops[0].channel = state->primary;
        invalid_ops[0].recv_out = &received;
        invalid_ops[0].result_errno = EPIPE;
        invalid_ops[1].kind = LLAM_SELECT_OP_RECV;
        invalid_ops[1].channel = state->primary;
        invalid_ops[1].recv_out = NULL;
        invalid_ops[1].result_errno = ECANCELED;
        invalid_ops[2].kind = LLAM_SELECT_OP_SEND;
        invalid_ops[2].channel = state->primary;
        invalid_ops[2].send_value = state;
        invalid_ops[2].result_errno = ETIMEDOUT;
        errno = 0;
        if (llam_channel_select(invalid_ops, 3U, UINT64_MAX, &selected) != -1 ||
            errno != EINVAL ||
            invalid_ops[0].result_errno != 0 ||
            invalid_ops[1].result_errno != 0 ||
            invalid_ops[2].result_errno != 0) {
            task_fail(state, "channel select validation failure kept stale result errno", errno);
            return;
        }
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void channel_destroy_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    atomic_store_explicit(&state->channel_destroy_waiter_started, 1U, memory_order_release);
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != EPIPE) {
        task_fail(state, "channel destroy waiter close result", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void channel_destroy_probe_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->channel_destroy_waiter_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (unsigned i = 0U; i < 16U; ++i) {
        llam_yield();
    }
    if (llam_channel_close(state->primary) != 0) {
        task_fail(state, "channel destroy probe close", errno);
        return;
    }
    /*
     * Close removes the waiter from the channel queue before the waiter resumes.
     * Destroy must still report EBUSY until that close wake is consumed;
     * otherwise select waiters can later clean up through a freed channel.
     */
    errno = 0;
    if (llam_channel_destroy(state->primary) != -1 || errno != EBUSY) {
        task_fail(state, "channel destroy close-woken waiter busy", errno);
        return;
    }
    atomic_store_explicit(&state->channel_destroy_busy_checked, 1U, memory_order_release);
}

static void cond_mutex_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_mutex_lock(state->mutex) != 0) {
        task_fail(state, "mutex lock", errno);
        return;
    }
    errno = 0;
    if (llam_mutex_lock(state->mutex) != -1 || errno != EDEADLK) {
        task_fail(state, "mutex non-recursive lock", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    errno = 0;
    if (llam_mutex_trylock(state->mutex) != -1 || errno != EBUSY) {
        task_fail(state, "mutex trylock owned", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    errno = 0;
    if (llam_cond_wait_until(state->cond, state->mutex, 0U) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "cond wait expired deadline", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    if (llam_mutex_unlock(state->mutex) != 0) {
        task_fail(state, "mutex unlock after cond timeout", errno);
        return;
    }
    if (llam_cond_signal(state->cond) != 0 ||
        llam_cond_broadcast(state->cond) != 0) {
        task_fail(state, "cond signal/broadcast no waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void cond_owner_recheck_task(void *arg) {
    cond_owner_recheck_state_t *state = arg;

    if (llam_mutex_lock(state->mutex) != 0) {
        task_fail(&state->edge, "cond owner recheck lock", errno);
        return;
    }
    atomic_store_explicit(&state->mutex_owned, 1U, memory_order_release);
    errno = 0;
    state->wait_rc = llam_cond_wait(state->cond, state->mutex);
    state->wait_errno = errno;
    if (state->wait_rc != -1 || state->wait_errno != EPERM) {
        task_fail(&state->edge, "cond owner recheck wait", state->wait_errno);
        return;
    }
    atomic_fetch_add_explicit(&state->edge.ran, 1U, memory_order_relaxed);
}

static void *cond_owner_recheck_corrupt_thread(void *arg) {
    cond_owner_recheck_state_t *state = arg;
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50000000L,
    };

    pthread_mutex_lock(&state->raw_cond->lock);
    atomic_store_explicit(&state->cond_locked, 1U, memory_order_release);
    while (atomic_load_explicit(&state->mutex_owned, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        struct timespec spin_delay = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };

        nanosleep(&spin_delay, NULL);
    }
    if (atomic_load_explicit(&state->stop, memory_order_acquire) != 0U) {
        pthread_mutex_unlock(&state->raw_cond->lock);
        return NULL;
    }
    /*
     * This thread owns raw_cond->lock before the task enters cond_wait().  The
     * delay gives the waiter time to pass the first mutex-owner check and block
     * on the cond lock.  Corrupting owner then exercises the second defensive
     * recheck path after the wait node has been queued.
     */
    nanosleep(&delay, NULL);
    atomic_store_explicit(&state->raw_mutex->owner, (uintptr_t)0, memory_order_release);
    pthread_mutex_unlock(&state->raw_cond->lock);
    return NULL;
}

static void timer_heap_overflow_task(void *arg) {
    timer_heap_overflow_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;
    uint64_t deadline_ns;

    if (shard == NULL) {
        task_fail(&state->edge, "timer heap overflow missing shard", EINVAL);
        return;
    }

    pthread_mutex_lock(&shard->lock);
    state->saved_len = shard->timer_heap_len;
    state->saved_cap = shard->timer_heap_cap;
    state->saved_heap = shard->timer_heap;
    state->saved_root = shard->timers;
    state->saved_timer_count = atomic_load_explicit(&shard->timer_count, memory_order_acquire);
    /*
     * Simulate a corrupted or future-imported heap at the overflow boundary.
     * Before the guard, timer_heap_len + 1 wrapped to zero, reserve succeeded,
     * and insertion wrote through timer_heap[SIZE_MAX].
     */
    shard->timer_heap_len = SIZE_MAX;
    shard->timer_heap_cap = 0U;
    shard->timer_heap = NULL;
    shard->timers = NULL;
    atomic_store_explicit(&shard->timer_count, 0U, memory_order_release);
    pthread_mutex_unlock(&shard->lock);

    deadline_ns = llam_now_ns() + 1000000000ULL;
    errno = 0;
    state->sleep_rc = llam_sleep_until(deadline_ns);
    state->sleep_errno = errno;

    pthread_mutex_lock(&shard->lock);
    shard->timer_heap_len = state->saved_len;
    shard->timer_heap_cap = state->saved_cap;
    shard->timer_heap = state->saved_heap;
    shard->timers = state->saved_root;
    atomic_store_explicit(&shard->timer_count, state->saved_timer_count, memory_order_release);
    pthread_mutex_unlock(&shard->lock);

    if (state->sleep_rc != -1 || state->sleep_errno != ENOMEM) {
        task_fail(&state->edge, "timer heap overflow sleep", state->sleep_errno);
        return;
    }
    atomic_fetch_add_explicit(&state->edge.ran, 1U, memory_order_relaxed);
}

static void precancel_wait_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;
#if !LLAM_PLATFORM_WINDOWS
    char byte = '\0';
    short revents = 0;
#endif

    errno = 0;
    if (llam_sleep_ns(1000000000ULL) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    /*
     * This duration intentionally overflows when added to the current
     * monotonic timestamp.  The runtime must saturate the absolute deadline so
     * cancellation still wins instead of treating the wrapped deadline as an
     * already-expired successful sleep.
     */
    if (llam_sleep_ns(UINT64_MAX) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel huge relative sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_channel_recv(state->primary) != NULL || errno != ECANCELED) {
        task_fail(state, "pre-cancel legacy channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    /*
     * Immediate operations may still complete with a pre-cancelled token; the
     * token is observed only once the second send has to publish a wait node.
     */
    if (llam_channel_send(state->secondary, state) != 0) {
        task_fail(state, "pre-cancel legacy channel send setup", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send(state->secondary, state) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel legacy channel send wait", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

#if !LLAM_PLATFORM_WINDOWS
    errno = 0;
    /*
     * The request is published before cancellation registration is checked.
     * A pre-cancelled token must detach it from the submit/watch owner before
     * returning, otherwise the backend can later observe a stale request.
     */
    if (llam_read(state->io_fds[0], &byte, 1U) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel io read", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    /*
     * Seed revents with a non-zero sentinel so the cancelled fast-fail path
     * proves it does not leak stale readiness bits back to FFI callers.
     */
    revents = (short)0x7fff;
    if (llam_poll_fd(state->io_fds[0], POLLIN, 1000, &revents) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel timed poll", errno);
        return;
    }
    if (revents != 0) {
        task_fail(state, "pre-cancel timed poll stale revents", 0);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    /*
     * The infinite wait path shares backend watch code on POSIX; keep the same
     * stale-output guard here so both pre-arm cancellation paths are covered.
     */
    revents = (short)0x7fff;
    /*
     * Infinite poll/read waits use shared backend watch paths on POSIX.  A
     * token that is already cancelled before the request is armed must fail
     * inline instead of relying on a cancellation wake that was never queued.
     */
    if (llam_poll_fd(state->io_fds[0], POLLIN, -1, &revents) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel infinite poll", errno);
        return;
    }
    if (revents != 0) {
        task_fail(state, "pre-cancel infinite poll stale revents", 0);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_read_when_ready(state->io_fds[0], &byte, 1U, -1) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel read when ready", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
#endif
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void tiny_deadline_sleep_task(void *arg) {
    edge_state_t *state = arg;

    /*
     * Exercise the timer setup boundary where a watchdog or scheduler can pop
     * a just-armed timer before the sleeper reaches the context switch.  That
     * path must be treated as completion, not as timer allocation failure.
     */
    for (unsigned i = 0U; i < 4096U; ++i) {
        uint64_t deadline_ns = llam_now_ns() + 1U;

        if (llam_sleep_until(deadline_ns) != 0) {
            task_fail(state, "tiny deadline sleep", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != ECANCELED) {
        task_fail(state, "runtime stop channel waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_request_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_sleep_ns(1000000ULL) != 0) {
        task_fail(state, "runtime stop trigger sleep", errno);
        return;
    }
    if (llam_runtime_request_stop() != 0) {
        task_fail(state, "runtime stop trigger request", errno);
    }
}

static void stop_late_waiter_task(void *arg) {
    edge_state_t *state = arg;

    for (unsigned i = 0U; i < 16U; ++i) {
        llam_yield();
    }
    stop_waiter_task(state);
}

#if !LLAM_PLATFORM_WINDOWS
static void stop_poll_handle_waiter_task(void *arg) {
    edge_state_t *state = arg;
    short revents = 0;

    errno = 0;
    /*
     * POSIX handles alias fds.  Infinite handle polling must therefore use the
     * fd poll path, where runtime stop can abort the parked I/O request instead
     * of waiting for an uninterruptible blocking-helper callback to return.
     */
    if (llam_poll_handle((llam_handle_t)state->io_fds[0], POLLIN, -1, &revents) != -1 ||
        errno != ECANCELED) {
        task_fail(state, "runtime stop POSIX poll_handle waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_direct_read_waiter_task(void *arg) {
    edge_state_t *state = arg;
    char byte = 0;

    errno = 0;
    /*
     * LLAM_DIRECT_BLOCKING_IO forces the compensated direct-read shortcut.  An
     * empty blocking socket must still observe runtime stop instead of pinning a
     * scheduler thread in an uninterruptible read(2)/recv(2).
     */
    if (llam_read(state->io_fds[0], &byte, 1U) != -1 || errno != ECANCELED) {
        task_fail(state, "runtime stop direct blocking read waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void token_cancel_read_waiter_task(void *arg) {
    edge_state_t *state = arg;
    char byte = 0;

    atomic_store_explicit(&state->external_waiter_started, 1U, memory_order_release);
    errno = 0;
    /*
     * Keep this descriptor in its default blocking mode.  A task cancel token
     * must still abort the async kqueue/epoll request instead of relying on the
     * runtime-stop sweep or a user-visible fd close.
     */
    if (llam_read(state->io_fds[0], &byte, 1U) != -1 || errno != ECANCELED) {
        task_fail(state, "cancel token blocking socket read waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void token_cancel_read_trigger_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->external_waiter_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    /*
     * Give the waiter a scheduler turn to publish its I/O request.  The
     * regression this protects was a Darwin node-control wake race after the
     * request had already moved into the backend.
     */
    if (llam_sleep_ns(1000000ULL) != 0) {
        task_fail(state, "cancel token read trigger sleep", errno);
        return;
    }
    if (llam_cancel_token_cancel(state->token) != 0) {
        task_fail(state, "cancel token read trigger cancel", errno);
    }
}

static void stop_accept_waiter_task(void *arg) {
    edge_state_t *state = arg;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    llam_fd_t accepted;

    errno = 0;
    /*
     * LLAM_ACCEPT_DIRECT_BLOCKING forces the accept helper fallback for address
     * reporting calls.  With no inbound clients, runtime stop must cancel the
     * helper's sliced accept loop; otherwise a running block job keeps
     * llam_run() alive indefinitely.
     */
    accepted = llam_accept(state->io_fds[0], (struct sockaddr *)&addr, &addr_len);
    if (!LLAM_FD_IS_INVALID(accepted)) {
        (void)close(accepted);
        task_fail(state, "runtime stop blocking accept unexpectedly succeeded", 0);
        return;
    }
    if (errno != ECANCELED) {
        task_fail(state, "runtime stop blocking accept waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}
#endif

static void second_run_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != 0 ||
        received != (void *)(uintptr_t)0x51EC0DULL) {
        task_fail(state, "second run channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void second_run_sender_task(void *arg) {
    edge_state_t *state = arg;

    for (unsigned i = 0U; i < 4U; ++i) {
        llam_yield();
    }
    if (llam_channel_send(state->primary, (void *)(uintptr_t)0x51EC0DULL) != 0) {
        task_fail(state, "second run channel send", errno);
    }
}

static void unmanaged_drain_sender_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_channel_send(state->primary, (void *)(uintptr_t)0xD0A1U) != 0) {
        task_fail(state, "unmanaged drain sender", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void unmanaged_external_recv_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    atomic_store_explicit(&state->external_waiter_started, 1U, memory_order_release);
    if (llam_channel_recv_result(state->primary, &received) != 0 ||
        received != (void *)(uintptr_t)0x1234U) {
        task_fail(state, "unmanaged external channel recv", errno);
        return;
    }
    atomic_store_explicit(&state->external_waiter_done, 1U, memory_order_release);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void *unmanaged_external_run_thread(void *arg) {
    edge_state_t *state = arg;

    if (llam_run() != 0) {
        task_fail(state, "unmanaged external channel run", errno);
    }
    return NULL;
}

static int test_unmanaged_boundary_contracts(void) {
    edge_state_t state;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_task_local_key_t key = LLAM_TASK_LOCAL_INVALID_KEY;
    llam_select_op_t ops[1];
    void *out = NULL;
    size_t selected = SIZE_MAX;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.blocking_calls, 0U);

    channel = llam_channel_create(1U);
    mutex = llam_mutex_create();
    cond = llam_cond_create();
    if (channel == NULL || mutex == NULL || cond == NULL) {
        if (channel != NULL) {
            (void)llam_channel_destroy(channel);
        }
        if (mutex != NULL) {
            (void)llam_mutex_destroy(mutex);
        }
        if (cond != NULL) {
            (void)llam_cond_destroy(cond);
        }
        return fail_errno("unmanaged fixture create failed");
    }
    if (llam_task_local_key_create(&key) != 0) {
        (void)llam_channel_destroy(channel);
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        return fail_errno("task local key create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_task_local_key_delete(key);
        (void)llam_channel_destroy(channel);
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        return fail_errno("runtime init for unmanaged boundary failed");
    }

    if (llam_current_task() != NULL) {
        llam_runtime_shutdown();
        return fail_msg("current task outside managed context was not NULL");
    }
    llam_yield();
    llam_task_safepoint();
    if (llam_sleep_ns(0U) != 0 ||
        llam_enter_blocking() != 0 ||
        llam_leave_blocking() != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged no-op/blocking boundary failed");
    }
    if (llam_call_blocking_result(blocking_echo, &state, &out) != 0 ||
        out != &state ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged blocking call did not run inline");
    }

    errno = 0;
    if (llam_mutex_lock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex lock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_mutex_trylock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex trylock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_mutex_unlock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex unlock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_cond_wait(cond, mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged cond wait did not fail with ENOTSUP");
    }
    if (llam_cond_signal(cond) != 0 || llam_cond_broadcast(cond) != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged cond signal/broadcast failed");
    }
    errno = 0;
    if (llam_channel_send(channel, &state) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel send did not fail with ENOTSUP");
    }
    if (llam_channel_try_send(channel, &state) != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged channel try_send failed");
    }
    errno = 0;
    if (llam_channel_try_send(channel, &state) != -1 || errno != EAGAIN) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_send full case did not fail with EAGAIN");
    }
    errno = 0;
    if (llam_channel_recv_result(channel, &out) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel recv did not fail with ENOTSUP");
    }
    out = NULL;
    if (llam_channel_try_recv_result(channel, &out) != 0 || out != &state) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged channel try_recv failed to drain host-sent value");
    }
    errno = 0;
    if (llam_channel_try_recv_result(channel, &out) != -1 || errno != EAGAIN) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_recv empty case did not fail with EAGAIN");
    }
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = channel;
    ops[0].recv_out = &out;
    ops[0].result_errno = ECANCELED;
    errno = 0;
    if (llam_channel_select(ops, 1U, 0U, &selected) != -1 ||
        errno != ENOTSUP ||
        ops[0].result_errno != 0) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel select did not clear stale result errno");
    }
    errno = 0;
    if (llam_task_local_get(key) != NULL || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged task local get did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_local_set(key, &state) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged task local set did not fail with ENOTSUP");
    }

    llam_runtime_shutdown();
    if (llam_task_local_key_delete(key) != 0 ||
        llam_channel_destroy(channel) != 0 ||
        llam_mutex_destroy(mutex) != 0 ||
        llam_cond_destroy(cond) != 0) {
        return fail_errno("unmanaged fixture destroy failed");
    }
    return 0;
}

static int test_unmanaged_channel_try_drain_after_run(void) {
    edge_state_t state;
    llam_task_t *sender = NULL;
    void *out = NULL;

    memset(&state, 0, sizeof(state));
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("unmanaged channel drain fixture create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("runtime init for unmanaged channel drain failed");
    }

    sender = llam_spawn(unmanaged_drain_sender_task, &state, NULL);
    if (sender == NULL) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged drain sender spawn failed");
    }
    if (llam_run() != 0 || llam_join(sender) != 0 || check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged drain sender run/join failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_msg("unmanaged drain sender did not run");
    }

    /*
     * Host embedders must be able to drain buffered values after the scheduler
     * has quiesced; otherwise the documented destroy-EBUSY cleanup path is
     * impossible without spawning a synthetic managed task.
     */
    out = NULL;
    if (llam_channel_try_recv_result(state.primary, &out) != 0 ||
        out != (void *)(uintptr_t)0xD0A1U) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged host drain of task-sent channel value failed");
    }

    llam_runtime_shutdown();
    if (llam_channel_destroy(state.primary) != 0) {
        return fail_errno("unmanaged drained channel destroy failed");
    }
    return 0;
}

static int test_unmanaged_channel_try_drain_after_shutdown(void) {
    edge_state_t state;
    llam_task_t *sender = NULL;
    void *out = NULL;

    memset(&state, 0, sizeof(state));
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("post-shutdown channel drain fixture create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("runtime init for post-shutdown channel drain failed");
    }

    sender = llam_spawn(unmanaged_drain_sender_task, &state, NULL);
    if (sender == NULL) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown drain sender spawn failed");
    }
    if (llam_run() != 0 || llam_join(sender) != 0 || check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown drain sender run/join failed");
    }
    llam_runtime_shutdown();

    /*
     * Destroying a channel is independent of the runtime singleton.  Buffered
     * payloads therefore remain drainable after shutdown, but no parked sender
     * is woken without a live scheduler.
     */
    out = NULL;
    if (llam_channel_try_recv_result(state.primary, &out) != 0 ||
        out != (void *)(uintptr_t)0xD0A1U) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown host drain of task-sent channel value failed");
    }
    if (llam_channel_destroy(state.primary) != 0) {
        return fail_errno("post-shutdown drained channel destroy failed");
    }
    return 0;
}

static int test_unmanaged_try_send_wakes_parked_channel_waiter(void) {
    edge_state_t state;
    llam_task_t *waiter = NULL;
    pthread_t runner;
    int runner_created = 0;
    int runner_joined = 0;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.external_waiter_started, 0U);
    atomic_init(&state.external_waiter_done, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("unmanaged external wake channel create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(unmanaged_external_recv_waiter_task, &state, NULL);
    if (waiter == NULL) {
        goto cleanup_runtime;
    }
    if (pthread_create(&runner, NULL, unmanaged_external_run_thread, &state) != 0) {
        goto cleanup_runtime;
    }
    runner_created = 1;

    if (wait_until_atomic_nonzero(&state.external_waiter_started, 1000000000ULL) != 0) {
        goto cleanup_runtime;
    }
    /*
     * Leave the receiver parked across several watchdog intervals.  Before the
     * watchdog learned about externally wakeable channel waits, this delay made
     * the runtime report EDEADLK even though the host thread could still
     * complete the wait with the documented non-parking try_send API.
     */
    {
        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 30000000L,
        };

        nanosleep(&ts, NULL);
    }
    if (llam_channel_try_send(state.primary, (void *)(uintptr_t)0x1234U) != 0) {
        goto cleanup_runtime;
    }
    if (pthread_join(runner, NULL) != 0) {
        goto cleanup_runtime;
    }
    runner_joined = 1;
    if (check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0) {
        waiter = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.external_waiter_done, memory_order_acquire) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (runner_created != 0 && runner_joined == 0) {
        (void)llam_channel_close(state.primary);
        (void)llam_runtime_request_stop();
        (void)pthread_join(runner, NULL);
        runner_joined = 1;
    }
    if (runner_joined != 0 && waiter != NULL) {
        (void)llam_join(waiter);
        waiter = NULL;
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("unmanaged external channel wake edge failed");
    }
    return rc;
}

static int test_repeated_run_clears_internal_drain_stop(void) {
    edge_state_t state;
    llam_task_t *first = NULL;
    llam_task_t *waiter = NULL;
    llam_task_t *sender = NULL;
    llam_task_t *blocker = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    first = llam_spawn(ownership_task, &state, NULL);
    if (first == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(first) != 0) {
        first = NULL;
        goto cleanup_runtime;
    }
    first = NULL;

    /*
     * A naturally drained run wakes idle workers with the runtime stop flag.
     * The next run must not inherit that internal signal, or normal channel
     * waits in this second run are incorrectly completed with ECANCELED.
     */
    waiter = llam_spawn(second_run_waiter_task, &state, NULL);
    sender = llam_spawn(second_run_sender_task, &state, NULL);
    if (waiter == NULL || sender == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(sender) != 0) {
        waiter = NULL;
        sender = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    sender = NULL;

    /*
     * Block workers are initialized once and must survive natural run drains.
     * A previous race let a block worker consume the transient drain-stop wake
     * after llam_run() cleared the stop flag, then dereference a NULL job.
     */
    blocker = llam_spawn(blocking_task, &state, NULL);
    if (blocker == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(blocker) != 0) {
        blocker = NULL;
        goto cleanup_runtime;
    }
    blocker = NULL;

    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 3U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (first != NULL) {
        (void)llam_join(first);
    }
    if (waiter != NULL) {
        (void)llam_join(waiter);
    }
    if (sender != NULL) {
        (void)llam_join(sender);
    }
    if (blocker != NULL) {
        (void)llam_join(blocker);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("repeated run drain-stop edge failed");
    }
    return rc;
}

static int test_join_timeout_preserves_ownership(void) {
    edge_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for join ownership failed");
    }
    task = llam_spawn(ownership_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn ownership task failed");
    }
    errno = 0;
    if (llam_join_until(task, 0U) != -1 || errno != ETIMEDOUT) {
        llam_runtime_shutdown();
        return fail_msg("join_until timeout did not preserve handle");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run join ownership failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("join after timeout failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_msg("ownership task did not run exactly once");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_duplicate_join_claim_race(void) {
    task_handle_race_state_t state;
    llam_task_t *joiner_a = NULL;
    llam_task_t *joiner_b = NULL;
    llam_task_t *releaser = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.target_release, 0U);
    atomic_init(&state.join_successes, 0U);
    atomic_init(&state.join_busy, 0U);
    atomic_init(&state.join_unexpected, 0U);
    atomic_init(&state.detach_busy, 0U);
    atomic_init(&state.detach_successes, 0U);

    if (init_runtime() != 0) {
        return fail_errno("runtime init for duplicate join claim race failed");
    }
    state.target = llam_spawn(task_handle_race_target_task, &state, NULL);
    joiner_a = llam_spawn(task_handle_race_joiner_task, &state, NULL);
    joiner_b = llam_spawn(task_handle_race_joiner_task, &state, NULL);
    releaser = llam_spawn(task_handle_race_release_after_busy_task, &state, NULL);
    if (state.target == NULL || joiner_a == NULL || joiner_b == NULL || releaser == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_handle_race_failures(&state) != 0 ||
        llam_join(joiner_a) != 0 ||
        llam_join(joiner_b) != 0 ||
        llam_join(releaser) != 0) {
        goto cleanup_runtime;
    }
    joiner_a = NULL;
    joiner_b = NULL;
    releaser = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.join_successes, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.join_busy, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.join_unexpected, memory_order_relaxed) != 0U) {
        goto cleanup_runtime;
    }
    /*
     * The successful join consumes the target handle.  A stale duplicate handle
     * must not remain inspectable after the racing joiner completed.
     */
    if (llam_task_id(state.target) != 0U ||
        strcmp(llam_task_state_name(state.target), "UNKNOWN") != 0) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    atomic_store_explicit(&state.target_release, 1U, memory_order_release);
    if (joiner_a != NULL) {
        (void)llam_join(joiner_a);
    }
    if (joiner_b != NULL) {
        (void)llam_join(joiner_b);
    }
    if (releaser != NULL) {
        (void)llam_join(releaser);
    }
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("duplicate join claim race failed");
    }
    return rc;
}

static int test_join_detach_claim_race(void) {
    task_handle_race_state_t state;
    llam_task_t *joiner = NULL;
    llam_task_t *detacher = NULL;
    llam_task_t *releaser = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.target_release, 0U);
    atomic_init(&state.join_successes, 0U);
    atomic_init(&state.join_busy, 0U);
    atomic_init(&state.join_unexpected, 0U);
    atomic_init(&state.detach_busy, 0U);
    atomic_init(&state.detach_successes, 0U);

    if (init_runtime() != 0) {
        return fail_errno("runtime init for join/detach claim race failed");
    }
    state.target = llam_spawn(task_handle_race_target_task, &state, NULL);
    state.raw_target = llam_task_resolve_public_handle(state.target);
    llam_task_end_public_op(state.raw_target);
    joiner = llam_spawn(task_handle_race_joiner_task, &state, NULL);
    detacher = llam_spawn(task_handle_race_detacher_task, &state, NULL);
    releaser = llam_spawn(task_handle_race_release_after_busy_task, &state, NULL);
    if (state.target == NULL || state.raw_target == NULL || joiner == NULL || detacher == NULL || releaser == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_handle_race_failures(&state) != 0 ||
        llam_join(joiner) != 0 ||
        llam_join(detacher) != 0 ||
        llam_join(releaser) != 0) {
        goto cleanup_runtime;
    }
    joiner = NULL;
    detacher = NULL;
    releaser = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.join_successes, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.detach_busy, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.detach_successes, memory_order_relaxed) != 0U) {
        goto cleanup_runtime;
    }
    if (llam_task_id(state.target) != 0U ||
        strcmp(llam_task_state_name(state.target), "UNKNOWN") != 0) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    atomic_store_explicit(&state.target_release, 1U, memory_order_release);
    if (joiner != NULL) {
        (void)llam_join(joiner);
    }
    if (detacher != NULL) {
        (void)llam_join(detacher);
    }
    if (releaser != NULL) {
        (void)llam_join(releaser);
    }
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("join/detach claim race failed");
    }
    return rc;
}

static int test_consumed_task_handle_reuse_guard(void) {
    edge_state_t state;
    llam_task_t *old_task;
    llam_task_t *new_task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for consumed task handle guard failed");
    }

    old_task = llam_spawn(ownership_task, &state, NULL);
    if (old_task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn old task for consumed handle guard failed");
    }
    if (llam_run() != 0 || llam_join(old_task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("old task join for consumed handle guard failed");
    }

    /*
     * Successful join consumes the public handle immediately.  The handle must
     * not remain inspectable while the reclaimed task object is waiting in the
     * allocator cache, because that makes stale aliases look valid until the
     * next spawn happens to reuse the same storage.
     */
    errno = 0;
    if (llam_task_id(old_task) != 0U ||
        strcmp(llam_task_state_name(old_task), "UNKNOWN") != 0) {
        llam_runtime_shutdown();
        return fail_msg("consumed joined task handle remained inspectable");
    }
    if (llam_join(old_task) != -1 || errno != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("consumed joined task handle did not fail with EINVAL");
    }

    new_task = llam_spawn(ownership_task, &state, NULL);
    if (new_task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn new task for consumed handle guard failed");
    }

    /*
     * The allocator aggressively reuses task objects.  A consumed public handle
     * must not alias the next task recycled into the same storage, or an
     * unmanaged stale join can block forever waiting for unrelated work.
     */
    errno = 0;
    if (llam_task_id(old_task) != 0U) {
        (void)llam_detach(new_task);
        llam_runtime_shutdown();
        return fail_msg("consumed task handle still inspected a recycled task");
    }
    if (llam_join(old_task) != -1 || errno != EINVAL) {
        (void)llam_detach(new_task);
        llam_runtime_shutdown();
        return fail_msg("consumed task handle did not fail with EINVAL");
    }

    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(new_task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("new task join after consumed handle guard failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return fail_msg("consumed handle guard tasks did not both run");
    }
    llam_runtime_shutdown();
    /*
     * Shutdown releases task slabs.  Stale public task inspection must short
     * circuit on the inactive runtime before reading the freed task object's
     * generation tag.
     */
    if (llam_task_id(old_task) != 0U ||
        strcmp(llam_task_state_name(old_task), "UNKNOWN") != 0 ||
        llam_task_flags(old_task) != 0U) {
        return fail_msg("consumed task handle inspected freed shutdown storage");
    }
    if (init_runtime() != 0) {
        return fail_errno("runtime reinit for consumed task handle guard failed");
    }
    /*
     * A later init republishes the singleton runtime.  The old task handle must
     * still be rejected through the live task registry rather than reading the
     * task slab freed by the previous shutdown.
     */
    if (llam_task_id(old_task) != 0U ||
        strcmp(llam_task_state_name(old_task), "UNKNOWN") != 0 ||
        llam_task_flags(old_task) != 0U) {
        llam_runtime_shutdown();
        return fail_msg("consumed task handle inspected previous runtime storage after reinit");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_detached_task_handle_consumed_immediately(void) {
    edge_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for detached task handle guard failed");
    }

    task = llam_spawn(ownership_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn detached task handle guard failed");
    }
    if (llam_detach(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("detach task handle guard failed");
    }

    /*
     * Detach consumes the handle before the target necessarily completes.  A
     * stale detached handle must therefore be rejected even while the task is
     * still queued/running and has not reached the reclaim path yet.
     */
    errno = 0;
    if (llam_task_id(task) != 0U ||
        strcmp(llam_task_state_name(task), "UNKNOWN") != 0) {
        llam_runtime_shutdown();
        return fail_msg("detached task handle remained inspectable");
    }
    if (llam_join(task) != -1 || errno != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("detached task handle did not fail with EINVAL");
    }

    if (llam_run() != 0 || check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return fail_errno("run detached task handle guard failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_msg("detached task did not run exactly once");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_destroyed_sync_handles_reject_public_ops(void) {
    llam_mutex_t *mutex = llam_mutex_create();
    llam_cond_t *cond = llam_cond_create();
    llam_channel_t *channel = llam_channel_create(1U);
    static int sentinel;
    void *value = &sentinel;

    if (mutex == NULL || cond == NULL || channel == NULL) {
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        (void)llam_channel_destroy(channel);
        return fail_errno("create destroyed sync handle guard objects failed");
    }
    if (llam_mutex_destroy(mutex) != 0 ||
        llam_cond_destroy(cond) != 0 ||
        llam_channel_destroy(channel) != 0) {
        return fail_errno("destroy sync handle guard objects failed");
    }

    /*
     * Destroyed public handles are consumed handles.  Hot-path operations used
     * to unwrap the tag by reading freed object storage; these calls must now
     * fail before touching the reclaimed mutex/cond/channel allocation.
     */
    errno = 0;
    if (llam_mutex_trylock(mutex) != -1 || errno != EINVAL) {
        return fail_msg("destroyed mutex trylock did not fail with EINVAL");
    }
    errno = 0;
    if (llam_cond_signal(cond) != -1 || errno != EINVAL) {
        return fail_msg("destroyed cond signal did not fail with EINVAL");
    }
    errno = 0;
    if (llam_cond_broadcast(cond) != -1 || errno != EINVAL) {
        return fail_msg("destroyed cond broadcast did not fail with EINVAL");
    }
    errno = 0;
    if (llam_channel_try_send(channel, value) != -1 || errno != EINVAL) {
        return fail_msg("destroyed channel try_send did not fail with EINVAL");
    }
    errno = 0;
    value = &sentinel;
    if (llam_channel_try_recv_result(channel, &value) != -1 || errno != EINVAL || value != NULL) {
        return fail_msg("destroyed channel try_recv did not fail closed with EINVAL");
    }
    errno = 0;
    if (llam_channel_close(channel) != -1 || errno != EINVAL) {
        return fail_msg("destroyed channel close did not fail with EINVAL");
    }
    return 0;
}

static int test_active_public_ops_block_destroy(void) {
    llam_mutex_t *mutex = llam_mutex_create();
    llam_cond_t *cond = llam_cond_create();
    llam_channel_t *channel = llam_channel_create(1U);
    llam_mutex_t *raw_mutex;
    llam_cond_t *raw_cond;
    llam_channel_t *raw_channel;

    if (mutex == NULL || cond == NULL || channel == NULL) {
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        (void)llam_channel_destroy(channel);
        return fail_errno("create active public op guard objects failed");
    }

    raw_mutex = llam_mutex_resolve_public_handle(mutex);
    raw_cond = llam_cond_resolve_public_handle(cond);
    raw_channel = llam_channel_resolve_public_handle(channel);
    if (raw_mutex == NULL || raw_cond == NULL || raw_channel == NULL) {
        llam_mutex_end_public_op(raw_mutex);
        llam_cond_end_public_op(raw_cond);
        llam_channel_end_public_op(raw_channel);
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        (void)llam_channel_destroy(channel);
        return fail_msg("resolve active public op guard objects failed");
    }

    /*
     * Public entry points validate stale handles through the live registry.
     * Once validation succeeds, destroy must not free the object until the
     * operation drops its active reference; otherwise a preempted operation can
     * resume on reclaimed storage.
     */
    errno = 0;
    if (llam_mutex_destroy(mutex) != -1 || errno != EBUSY) {
        return fail_msg("mutex destroy did not reject active public op");
    }
    errno = 0;
    if (llam_cond_destroy(cond) != -1 || errno != EBUSY) {
        return fail_msg("cond destroy did not reject active public op");
    }
    errno = 0;
    if (llam_channel_destroy(channel) != -1 || errno != EBUSY) {
        return fail_msg("channel destroy did not reject active public op");
    }

    llam_mutex_end_public_op(raw_mutex);
    llam_cond_end_public_op(raw_cond);
    llam_channel_end_public_op(raw_channel);

    if (llam_mutex_destroy(mutex) != 0 ||
        llam_cond_destroy(cond) != 0 ||
        llam_channel_destroy(channel) != 0) {
        return fail_errno("destroy active public op guard objects after release failed");
    }
    return 0;
}

static int test_consumed_channel_handle_reuse_guard(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for consumed channel handle guard failed");
    }

    task = llam_spawn(stale_channel_handle_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn consumed channel handle guard failed");
    }
    if (llam_run() == 0 &&
        llam_join(task) == 0 &&
        check_task_failures(&state) == 0 &&
        atomic_load_explicit(&state.ran, memory_order_relaxed) == 1U) {
        rc = 0;
    }

    if (state.secondary != NULL && llam_channel_destroy(state.secondary) != 0) {
        rc = 1;
    }
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("consumed channel handle guard failed");
    }
    return rc;
}

static int test_consumed_sync_handle_reuse_guard(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for consumed sync handle guard failed");
    }

    task = llam_spawn(stale_sync_handle_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn consumed sync handle guard failed");
    }
    if (llam_run() == 0 &&
        llam_join(task) == 0 &&
        check_task_failures(&state) == 0 &&
        atomic_load_explicit(&state.ran, memory_order_relaxed) == 1U) {
        rc = 0;
    }

    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("consumed sync handle guard failed");
    }
    return rc;
}

static int test_blocking_callback_edges(void) {
    edge_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for blocking edges failed");
    }
    task = llam_spawn(blocking_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn blocking task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run blocking edges failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("join blocking task failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return fail_msg("blocking edge counters mismatch");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_blocking_cancel_waits_for_running_callback(void) {
    edge_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *caller;
    llam_task_t *canceller;
    llam_task_t *releaser;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_started, 0U);
    atomic_init(&state.blocking_can_finish, 0U);
    atomic_init(&state.blocking_finished, 0U);
    atomic_init(&state.blocking_returned_before_finish, 0U);
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    caller = llam_spawn(blocking_cancel_caller_task, &state, &opts);
    canceller = llam_spawn(blocking_cancel_task, &state, NULL);
    releaser = llam_spawn(blocking_release_task, &state, NULL);
    if (caller == NULL || canceller == NULL || releaser == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(caller) != 0 ||
        llam_join(canceller) != 0 ||
        llam_join(releaser) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_finished, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.blocking_returned_before_finish, memory_order_acquire) != 0U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("blocking cancel running callback edge failed");
    }
    return rc;
}

static int test_channel_close_and_select_edges(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.drain = llam_channel_create(4U);
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    if (state.drain == NULL || state.primary == NULL || state.secondary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(channel_close_select_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.drain != NULL && llam_channel_destroy(state.drain) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL && llam_channel_destroy(state.secondary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("channel close/select edge failed");
    }
    return rc;
}

static int test_channel_destroy_rejects_close_woken_waiter(void) {
    edge_state_t state;
    llam_task_t *waiter = NULL;
    llam_task_t *probe = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.channel_destroy_waiter_started, 0U);
    atomic_init(&state.channel_destroy_busy_checked, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    waiter = llam_spawn(channel_destroy_waiter_task, &state, NULL);
    probe = llam_spawn(channel_destroy_probe_task, &state, NULL);
    if (waiter == NULL || probe == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(probe) != 0) {
        waiter = NULL;
        probe = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    probe = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.channel_destroy_busy_checked, memory_order_acquire) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("channel destroy close-woken waiter edge failed");
    }
    return rc;
}

static int test_cond_mutex_deadline_edges(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    if (state.mutex == NULL || state.cond == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(cond_mutex_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.cond != NULL && llam_cond_destroy(state.cond) != 0) {
        rc = 1;
    }
    if (state.mutex != NULL && llam_mutex_destroy(state.mutex) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("cond/mutex edge failed");
    }
    return rc;
}

static int test_cond_owner_recheck_unlinks_waiter(void) {
    cond_owner_recheck_state_t state;
    pthread_t corrupter;
    llam_task_t *task = NULL;
    bool corrupter_started = false;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.edge.failures, 0U);
    atomic_init(&state.edge.ran, 0U);
    atomic_init(&state.mutex_owned, 0U);
    atomic_init(&state.cond_locked, 0U);
    atomic_init(&state.stop, 0U);
    state.wait_rc = -2;
    state.wait_errno = 0;

    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    if (state.mutex == NULL || state.cond == NULL) {
        goto cleanup_no_runtime;
    }
    state.raw_mutex = llam_mutex_resolve_public_handle(state.mutex);
    state.raw_cond = llam_cond_resolve_public_handle(state.cond);
    if (state.raw_mutex == NULL || state.raw_cond == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(cond_owner_recheck_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (pthread_create(&corrupter, NULL, cond_owner_recheck_corrupt_thread, &state) != 0) {
        goto cleanup_runtime;
    }
    corrupter_started = true;
    while (atomic_load_explicit(&state.cond_locked, memory_order_acquire) == 0U) {
        struct timespec spin_delay = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };

        nanosleep(&spin_delay, NULL);
    }

    if (llam_run() != 0 || llam_join(task) != 0 || check_task_failures(&state.edge) != 0) {
        goto cleanup_runtime;
    }
    task = NULL;
    if (corrupter_started) {
        pthread_join(corrupter, NULL);
        corrupter_started = false;
    }
    if (state.raw_cond->waiters.head != NULL || state.raw_cond->waiters.depth != 0U) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.edge.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    atomic_store_explicit(&state.stop, 1U, memory_order_release);
    if (corrupter_started) {
        pthread_join(corrupter, NULL);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    llam_mutex_end_public_op(state.raw_mutex);
    llam_cond_end_public_op(state.raw_cond);
    if (state.cond != NULL && llam_cond_destroy(state.cond) != 0) {
        rc = 1;
    }
    if (state.mutex != NULL && llam_mutex_destroy(state.mutex) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.edge.failures, memory_order_relaxed) == 0U) {
        return fail_errno("cond owner recheck did not unlink waiter");
    }
    return rc;
}

static int test_timer_heap_capacity_overflow_guard(void) {
    timer_heap_overflow_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.edge.failures, 0U);
    atomic_init(&state.edge.ran, 0U);
    state.sleep_rc = -2;
    state.sleep_errno = 0;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("timer heap overflow opts init failed");
    }
    opts.deterministic = 1U;
    if (llam_runtime_init(&opts) != 0) {
        return fail_errno("timer heap overflow runtime init failed");
    }
    task = llam_spawn(timer_heap_overflow_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || llam_join(task) != 0 || check_task_failures(&state.edge) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.edge.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.edge.failures, memory_order_relaxed) == 0U) {
        return fail_errno("timer heap overflow guard failed");
    }
    return rc;
}

static void channel_select_excessive_op_count_task(void *arg) {
    edge_state_t *state = arg;
    llam_select_op_t op;
    const size_t excessive_counts[] = {
        (size_t)LLAM_CHANNEL_SELECT_MAX_OPS + 1U,
        SIZE_MAX,
    };
    void *out = NULL;
    size_t selected = 0U;
    size_t i;

    memset(&op, 0, sizeof(op));
    op.kind = LLAM_SELECT_OP_RECV;
    op.channel = state->primary;
    op.recv_out = &out;

    for (i = 0U; i < sizeof(excessive_counts) / sizeof(excessive_counts[0]); ++i) {
        errno = 0;
        /*
         * Reject excessive op_count values before walking the caller's array.
         * This catches malformed FFI calls as EINVAL instead of reading beyond
         * the single operation supplied here.
         */
        if (llam_channel_select(&op, excessive_counts[i], UINT64_MAX, &selected) != -1 ||
            errno != EINVAL) {
            task_fail(state, "select excessive op_count was not rejected", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static int test_channel_select_excessive_op_count(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    if (init_runtime() != 0) {
        return fail_errno("select excessive op_count runtime init failed");
    }
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        llam_runtime_shutdown();
        return fail_errno("select excessive op_count channel create failed");
    }

    task = llam_spawn(channel_select_excessive_op_count_task, &state, NULL);
    if (task == NULL) {
        rc = fail_errno("select excessive op_count task spawn failed");
    } else if (llam_run() != 0) {
        rc = fail_errno("select excessive op_count runtime run failed");
    } else if (llam_join(task) != 0) {
        rc = fail_errno("select excessive op_count task join failed");
    } else if (check_task_failures(&state) != 0) {
        rc = 1;
    } else if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        rc = fail_msg("select excessive op_count task did not run");
    }

    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0 && rc == 0) {
        rc = fail_errno("select excessive op_count channel destroy failed");
    }
    llam_runtime_shutdown();
    return rc;
}

static void channel_cached_double_destroy_task(void *arg) {
    edge_state_t *state = arg;
    llam_channel_t *channel;
    llam_channel_t *first;
    llam_channel_t *second;
    const size_t uncached_caps[] = {
        1024U,
        8192U,
    };
    size_t i;

    channel = llam_channel_create(1U);
    if (channel == NULL) {
        task_fail(state, "cached double destroy channel create", errno);
        return;
    }
    if (llam_channel_destroy(channel) != 0) {
        task_fail(state, "cached double destroy first destroy", errno);
        return;
    }

    errno = 0;
    /*
     * Capacity-one channels are cached inside managed tasks. This used to allow
     * a second destroy through the consumed public handle to insert the same
     * object into the cache twice, making two later creates return one aliased
     * channel.
     */
    if (llam_channel_destroy(channel) != -1 || errno != EINVAL) {
        task_fail(state, "cached double destroy was not rejected", errno);
        return;
    }

    first = llam_channel_create(1U);
    second = llam_channel_create(1U);
    if (first == NULL || second == NULL) {
        if (first != NULL) {
            (void)llam_channel_destroy(first);
        }
        if (second != NULL) {
            (void)llam_channel_destroy(second);
        }
        task_fail(state, "cached double destroy recreate", errno);
        return;
    }
    if (first == second) {
        (void)llam_channel_destroy(first);
        task_fail(state, "cached double destroy produced aliased channels", EFAULT);
        return;
    }
    if (llam_channel_destroy(first) != 0 || llam_channel_destroy(second) != 0) {
        task_fail(state, "cached double destroy cleanup", errno);
        return;
    }

    for (i = 0U; i < sizeof(uncached_caps) / sizeof(uncached_caps[0]); ++i) {
        channel = llam_channel_create(uncached_caps[i]);
        if (channel == NULL) {
            task_fail(state, "uncached double destroy channel create", errno);
            return;
        }
        if (llam_channel_destroy(channel) != 0) {
            task_fail(state, "uncached double destroy first destroy", errno);
            return;
        }
        errno = 0;
        /*
         * Larger channels bypass the capacity-one cache and are freed on the
         * first destroy.  The live registry must reject the stale handle before
         * destroy reads freed owner/lock fields.
         */
        if (llam_channel_destroy(channel) != -1 || errno != EINVAL) {
            task_fail(state, "uncached double destroy was not rejected", errno);
            return;
        }
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static int test_channel_cached_double_destroy_guard(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    if (init_runtime() != 0) {
        return fail_errno("cached double destroy runtime init failed");
    }
    task = llam_spawn(channel_cached_double_destroy_task, &state, NULL);
    if (task == NULL) {
        rc = fail_errno("cached double destroy task spawn failed");
    } else if (llam_run() != 0) {
        rc = fail_errno("cached double destroy runtime run failed");
    } else if (llam_join(task) != 0) {
        rc = fail_errno("cached double destroy task join failed");
    } else if (check_task_failures(&state) != 0) {
        rc = 1;
    } else if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        rc = fail_msg("cached double destroy task did not run");
    }
    llam_runtime_shutdown();
    return rc;
}

static void sync_double_destroy_task(void *arg) {
    edge_state_t *state = arg;
    llam_mutex_t *mutex;
    llam_cond_t *cond;

    mutex = llam_mutex_create();
    cond = llam_cond_create();
    if (mutex == NULL || cond == NULL) {
        if (mutex != NULL) {
            (void)llam_mutex_destroy(mutex);
        }
        if (cond != NULL) {
            (void)llam_cond_destroy(cond);
        }
        task_fail(state, "sync double destroy create", errno);
        return;
    }

    if (llam_mutex_destroy(mutex) != 0) {
        (void)llam_cond_destroy(cond);
        task_fail(state, "sync double destroy first mutex destroy", errno);
        return;
    }
    errno = 0;
    /*
     * Destroy consumes public sync handles.  Repeating it used to reach freed
     * mutex storage before returning an error; the live registry must reject it
     * as an invalid stale handle first.
     */
    if (llam_mutex_destroy(mutex) != -1 || errno != EINVAL) {
        (void)llam_cond_destroy(cond);
        task_fail(state, "sync double destroy stale mutex accepted", errno);
        return;
    }

    if (llam_cond_destroy(cond) != 0) {
        task_fail(state, "sync double destroy first cond destroy", errno);
        return;
    }
    errno = 0;
    if (llam_cond_destroy(cond) != -1 || errno != EINVAL) {
        task_fail(state, "sync double destroy stale cond accepted", errno);
        return;
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static int test_sync_double_destroy_guards(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    if (init_runtime() != 0) {
        return fail_errno("sync double destroy runtime init failed");
    }
    task = llam_spawn(sync_double_destroy_task, &state, NULL);
    if (task == NULL) {
        rc = fail_errno("sync double destroy task spawn failed");
    } else if (llam_run() != 0) {
        rc = fail_errno("sync double destroy runtime run failed");
    } else if (llam_join(task) != 0) {
        rc = fail_errno("sync double destroy task join failed");
    } else if (check_task_failures(&state) != 0) {
        rc = 1;
    } else if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        rc = fail_msg("sync double destroy task did not run");
    }
    llam_runtime_shutdown();
    return rc;
}

static int test_precancel_wait_edges(void) {
    edge_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
#if !LLAM_PLATFORM_WINDOWS
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
#endif
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    state.token = llam_cancel_token_create();
    if (state.primary == NULL || state.secondary == NULL || state.token == NULL) {
        goto cleanup_no_runtime;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.io_fds) != 0) {
        goto cleanup_no_runtime;
    }
#endif
    if (llam_cancel_token_cancel(state.token) != 0 ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(precancel_wait_task, &state, &opts);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) !=
#if LLAM_PLATFORM_WINDOWS
            5U
#else
            9U
#endif
    ) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
#if !LLAM_PLATFORM_WINDOWS
    if (state.io_fds[0] >= 0) {
        close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        close(state.io_fds[1]);
    }
#endif
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL) {
        void *drained = NULL;

        (void)llam_channel_try_recv_result(state.secondary, &drained);
        if (llam_channel_destroy(state.secondary) != 0) {
            rc = 1;
        }
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("pre-cancel wait edge failed");
    }
    return rc;
}

static int test_tiny_deadline_sleep_race(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for tiny deadline sleep failed");
    }

    task = llam_spawn(tiny_deadline_sleep_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("tiny deadline sleep race failed");
    }
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int run_io_completion_cancel_reorder_round(unsigned round, unsigned writer_delay, unsigned cancel_delay) {
    io_reorder_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *reader = NULL;
    llam_task_t *writer = NULL;
    llam_task_t *canceller = NULL;
    unsigned completions;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.fds[0] = -1;
    state.fds[1] = -1;
    state.writer_delay = writer_delay;
    state.cancel_delay = cancel_delay;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.reader_ready, 0U);
    atomic_init(&state.read_successes, 0U);
    atomic_init(&state.read_cancellations, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.fds) != 0) {
        return fail_errno("io reorder socketpair setup failed");
    }
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    reader = llam_spawn(io_reorder_reader_task, &state, &opts);
    writer = llam_spawn(io_reorder_writer_task, &state, NULL);
    canceller = llam_spawn(io_reorder_cancel_task, &state, NULL);
    if (reader == NULL || writer == NULL || canceller == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_io_reorder_failures(&state) != 0 ||
        llam_join(reader) != 0 ||
        llam_join(writer) != 0 ||
        llam_join(canceller) != 0) {
        reader = NULL;
        writer = NULL;
        canceller = NULL;
        goto cleanup_runtime;
    }
    reader = NULL;
    writer = NULL;
    canceller = NULL;
    completions = atomic_load_explicit(&state.read_successes, memory_order_relaxed) +
                  atomic_load_explicit(&state.read_cancellations, memory_order_relaxed);
    if (completions != 1U) {
        fprintf(stderr,
                "[test_runtime_api_edges] io reorder round=%u produced %u terminal results\n",
                round,
                completions);
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (reader != NULL) {
        (void)llam_join(reader);
    }
    if (writer != NULL) {
        (void)llam_join(writer);
    }
    if (canceller != NULL) {
        (void)llam_join(canceller);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.fds[0] >= 0) {
        (void)close(state.fds[0]);
    }
    if (state.fds[1] >= 0) {
        (void)close(state.fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("io completion/cancel reorder round failed");
    }
    return rc;
}

static int test_posix_io_completion_cancel_reorder(void) {
    /*
     * Exercise both possible orderings around a parked read: backend readiness
     * can arrive before cancellation, or cancellation can detach the request
     * before the ready byte is consumed.  Either a one-byte read or ECANCELED is
     * a valid terminal result, but the request must complete exactly once.
     */
    for (unsigned i = 0U; i < 96U; ++i) {
        unsigned mode = i % 3U;
        unsigned writer_delay = mode == 0U ? 1U : (mode == 1U ? 8U : (i & 3U));
        unsigned cancel_delay = mode == 0U ? 8U : (mode == 1U ? 1U : ((i + 1U) & 3U));

        if (run_io_completion_cancel_reorder_round(i, writer_delay, cancel_delay) != 0) {
            return 1;
        }
    }
    return 0;
}
#endif

static int test_runtime_stop_cancels_parked_channel_wait(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0) {
        (void)fail_errno("runtime stop channel wait run failed");
        goto cleanup_runtime;
    }
    if (check_task_failures(&state) != 0) {
        goto cleanup_runtime;
    }
    if (llam_join(waiter) != 0) {
        (void)fail_errno("runtime stop channel waiter join failed");
        goto cleanup_runtime;
    }
    if (llam_join(stopper) != 0) {
        (void)fail_errno("runtime stop channel stopper join failed");
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] channel stop counters ran=%u canceled=%u failures=%u\n",
                      atomic_load_explicit(&state.ran, memory_order_relaxed),
                      atomic_load_explicit(&state.canceled_waits, memory_order_relaxed),
                      atomic_load_explicit(&state.failures, memory_order_relaxed));
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop parked channel edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_late_channel_wait(void) {
    edge_state_t state;
    llam_spawn_opts_t pinned;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL ||
        llam_spawn_opts_init(&pinned, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    pinned.flags = LLAM_SPAWN_F_PINNED;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    /*
     * The stopper runs first and sets the runtime stop flag. The second task
     * then attempts to enter a fresh channel wait after stop is already active;
     * this used to park forever until the watchdog reported EDEADLK.
     */
    stopper = llam_spawn(stop_request_task, &state, &pinned);
    waiter = llam_spawn(stop_late_waiter_task, &state, &pinned);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0) {
        (void)fail_errno("runtime stop late channel run failed");
        goto cleanup_runtime;
    }
    if (check_task_failures(&state) != 0) {
        goto cleanup_runtime;
    }
    if (llam_join(waiter) != 0) {
        (void)fail_errno("runtime stop late channel waiter join failed");
        goto cleanup_runtime;
    }
    if (llam_join(stopper) != 0) {
        (void)fail_errno("runtime stop late channel stopper join failed");
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] late channel counters ran=%u canceled=%u failures=%u\n",
                      atomic_load_explicit(&state.ran, memory_order_relaxed),
                      atomic_load_explicit(&state.canceled_waits, memory_order_relaxed),
                      atomic_load_explicit(&state.failures, memory_order_relaxed));
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop late channel edge failed");
    }
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int test_runtime_stop_cancels_posix_poll_handle_wait(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    if (pipe(state.io_fds) != 0) {
        return fail_errno("runtime stop poll_handle pipe setup failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_poll_handle_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.io_fds[0] >= 0) {
        (void)close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        (void)close(state.io_fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop POSIX poll_handle edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_direct_blocking_read(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.io_fds) != 0) {
        return fail_errno("runtime stop direct blocking read socketpair setup failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_direct_read_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.io_fds[0] >= 0) {
        (void)close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        (void)close(state.io_fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop direct blocking read edge failed");
    }
    return rc;
}

static int test_cancel_token_cancels_blocking_socket_read(void) {
    edge_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *waiter = NULL;
    llam_task_t *canceller = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    atomic_init(&state.external_waiter_started, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.io_fds) != 0) {
        return fail_errno("cancel token read socketpair setup failed");
    }
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(token_cancel_read_waiter_task, &state, &opts);
    canceller = llam_spawn(token_cancel_read_trigger_task, &state, NULL);
    if (waiter == NULL || canceller == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(canceller) != 0) {
        goto cleanup_runtime;
    }
    waiter = NULL;
    canceller = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (waiter != NULL) {
        (void)llam_join(waiter);
    }
    if (canceller != NULL) {
        (void)llam_join(canceller);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.io_fds[0] >= 0) {
        (void)close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        (void)close(state.io_fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("cancel token blocking socket read edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_blocking_accept(void) {
    edge_state_t state;
    struct sockaddr_in addr;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int listener = -1;
    int one = 1;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return fail_errno("runtime stop blocking accept socket setup failed");
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 16) != 0) {
        (void)fail_errno("runtime stop blocking accept listener bind/listen failed");
        goto cleanup_no_runtime;
    }
    state.io_fds[0] = listener;
    if (init_runtime() != 0) {
        (void)fail_errno("runtime stop blocking accept runtime init failed");
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_accept_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        (void)fail_errno("runtime stop blocking accept spawn failed");
        goto cleanup_runtime;
    }
    if (llam_run() != 0) {
        (void)fail_errno("runtime stop blocking accept run failed");
        goto cleanup_runtime;
    }
    if (check_task_failures(&state) != 0) {
        goto cleanup_runtime;
    }
    if (llam_join(waiter) != 0) {
        (void)fail_errno("runtime stop blocking accept waiter join failed");
        goto cleanup_runtime;
    }
    if (llam_join(stopper) != 0) {
        (void)fail_errno("runtime stop blocking accept stopper join failed");
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] blocking accept counters ran=%u canceled=%u failures=%u\n",
                      atomic_load_explicit(&state.ran, memory_order_relaxed),
                      atomic_load_explicit(&state.canceled_waits, memory_order_relaxed),
                      atomic_load_explicit(&state.failures, memory_order_relaxed));
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (listener >= 0) {
        (void)close(listener);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop blocking accept edge failed");
    }
    return rc;
}
#endif

typedef int (*edge_case_fn)(void);

static int test_public_slot_generation_wrap_guard(void) {
    llam_public_slot_table_t table;
    int first_object = 1;
    int second_object = 2;
    size_t slot = 0U;
    size_t reused_slot = 0U;
    uint32_t generation = 0U;
    uint32_t reused_generation = 0U;
    int rc = 1;

    memset(&table, 0, sizeof(table));
    if (llam_public_slot_reserve(&table, &first_object, 1U, &slot, &generation) != 0) {
        return fail_errno("public slot initial reserve failed");
    }
    if (slot != 0U || generation != 1U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] unexpected initial slot=%zu generation=%u\n",
                      slot,
                      generation);
        goto cleanup;
    }

    /*
     * A registry slot that has reached UINT32_MAX must not wrap to generation
     * 1 on reuse. Otherwise a very old handle from the first generation can
     * alias a later object after enough churn in a long-lived process.
     */
    table.slots[slot].generation = UINT32_MAX;
    llam_public_slot_release(&table, slot, &first_object, UINT32_MAX);
    if (llam_public_slot_resolve(&table, slot, UINT32_MAX) != NULL) {
        (void)fprintf(stderr, "[test_runtime_api_edges] released max-generation slot resolved live\n");
        goto cleanup;
    }
    if (llam_public_slot_reserve(&table, &second_object, 1U, &reused_slot, &reused_generation) != 0) {
        rc = fail_errno("public slot reserve after max generation failed");
        goto cleanup;
    }
    if (reused_slot == slot) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] max-generation slot reused slot=%zu generation=%u\n",
                      reused_slot,
                      reused_generation);
        goto cleanup;
    }
    if (llam_public_slot_resolve(&table, slot, 1U) != NULL ||
        llam_public_slot_resolve(&table, slot, UINT32_MAX) != NULL) {
        (void)fprintf(stderr, "[test_runtime_api_edges] retired slot accepted a stale generation\n");
        goto cleanup;
    }

    /*
     * Live-slot reactivation is the task-handle reuse path. It must report
     * overflow instead of wrapping the same live slot back to generation one.
     */
    table.slots[reused_slot].generation = UINT32_MAX;
    errno = 0;
    if (llam_public_slot_reactivate(&table, reused_slot, &second_object, &reused_generation) != -1 ||
        errno != EOVERFLOW) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] max-generation live slot reactivated errno=%d generation=%u\n",
                      errno,
                      reused_generation);
        goto cleanup;
    }
    if (llam_public_slot_resolve(&table, reused_slot, 1U) != NULL) {
        (void)fprintf(stderr, "[test_runtime_api_edges] live slot reactivation wrapped to stale generation\n");
        goto cleanup;
    }
    rc = 0;

cleanup:
    free(table.slots);
    return rc;
}

static int test_public_slot_family_tags_reject_wrong_family(void) {
    llam_public_slot_table_t channel_table;
    llam_public_slot_table_t mutex_table;
    llam_public_slot_table_t reuse_table;
    int channel_object = 1;
    int mutex_object = 2;
    int reuse_first_object = 3;
    int reuse_second_object = 4;
    int reuse_third_object = 5;
    size_t channel_slot = 0U;
    size_t mutex_slot = 0U;
    size_t reuse_slot = 0U;
    size_t reuse_second_slot = 0U;
    size_t reuse_third_slot = 0U;
    uint32_t channel_generation = 0U;
    uint32_t mutex_generation = 0U;
    uint32_t reuse_generation = 0U;
    uint32_t reuse_second_generation = 0U;
    uint32_t reuse_third_generation = 0U;
    uint32_t naive_next_generation = 0U;
    uint32_t reused_generation = 0U;
    size_t reused_slot = 0U;
    int rc = 1;

    memset(&channel_table, 0, sizeof(channel_table));
    memset(&mutex_table, 0, sizeof(mutex_table));
    memset(&reuse_table, 0, sizeof(reuse_table));
    reuse_table.handle_secret = UINT64_C(0x8e14f6c2a9b57d31);
    if (llam_public_slot_reserve_family(&channel_table,
                                        &channel_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &channel_slot,
                                        &channel_generation) != 0) {
        rc = fail_errno("family slot channel reserve failed");
        goto cleanup;
    }
    if (llam_public_slot_reserve_family(&mutex_table,
                                        &mutex_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_MUTEX,
                                        &mutex_slot,
                                        &mutex_generation) != 0) {
        rc = fail_errno("family slot mutex reserve failed");
        goto cleanup;
    }
    if (channel_slot != 0U || mutex_slot != 0U || channel_generation == mutex_generation) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] family initial handles did not split: "
                      "channel=(%zu,%u) mutex=(%zu,%u)\n",
                      channel_slot,
                      channel_generation,
                      mutex_slot,
                      mutex_generation);
        goto cleanup;
    }
    if (channel_generation == LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL ||
        mutex_generation == LLAM_PUBLIC_HANDLE_FAMILY_MUTEX ||
        llam_public_slot_resolve(&channel_table,
                                 channel_slot,
                                 LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL) != NULL ||
        llam_public_slot_resolve(&mutex_table,
                                 mutex_slot,
                                 LLAM_PUBLIC_HANDLE_FAMILY_MUTEX) != NULL) {
        (void)fprintf(stderr, "[test_runtime_api_edges] guessable family generation resolved live\n");
        goto cleanup;
    }
    if (llam_public_slot_resolve(&channel_table, channel_slot, mutex_generation) != NULL ||
        llam_public_slot_resolve(&mutex_table, mutex_slot, channel_generation) != NULL) {
        (void)fprintf(stderr, "[test_runtime_api_edges] wrong-family generation resolved live\n");
        goto cleanup;
    }

    if (llam_public_slot_reserve_family(&reuse_table,
                                        &reuse_first_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &reuse_slot,
                                        &reuse_generation) != 0) {
        rc = fail_errno("family slot reuse first reserve failed");
        goto cleanup;
    }
    llam_public_slot_release(&reuse_table, reuse_slot, &reuse_first_object, reuse_generation);
    if (llam_public_slot_reserve_family(&reuse_table,
                                        &reuse_second_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &reuse_second_slot,
                                        &reuse_second_generation) != 0) {
        rc = fail_errno("family slot reuse second reserve failed");
        goto cleanup;
    }
    naive_next_generation =
        llam_public_slot_family_generation((reuse_generation >> LLAM_PUBLIC_HANDLE_FAMILY_BITS) + 1U,
                                           LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL);
    if (reuse_second_slot != reuse_slot ||
        reuse_second_generation == naive_next_generation ||
        llam_public_slot_resolve(&reuse_table, reuse_slot, naive_next_generation) != NULL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] predictable family generation reuse accepted: "
                      "old=%u naive=%u new=(%zu,%u)\n",
                      reuse_generation,
                      naive_next_generation,
                      reuse_second_slot,
                      reuse_second_generation);
        goto cleanup;
    }

    /*
     * Reuse once more to verify the consumed second handle stays invalid. The
     * broader injectivity test below covers long windows of old stale handles.
     */
    llam_public_slot_release(&reuse_table, reuse_second_slot, &reuse_second_object, reuse_second_generation);
    if (llam_public_slot_reserve_family(&reuse_table,
                                        &reuse_third_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &reuse_third_slot,
                                        &reuse_third_generation) != 0) {
        rc = fail_errno("family slot adjacent collision reserve failed");
        goto cleanup;
    }
    if (reuse_third_slot != reuse_slot ||
        reuse_third_generation == reuse_second_generation ||
        llam_public_slot_resolve(&reuse_table, reuse_slot, reuse_second_generation) != NULL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] consumed family generation was reissued: "
                      "slot=%zu consumed=%u new=(%zu,%u)\n",
                      reuse_slot,
                      reuse_second_generation,
                      reuse_third_slot,
                      reuse_third_generation);
        goto cleanup;
    }

    /*
     * Family-tagged generations also need the same ABA wrap guard as the
     * original untagged helper.  A retired max-epoch slot must not wrap back to
     * the initial family generation.
     */
    channel_table.slots[channel_slot].generation =
        llam_public_slot_family_generation(LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH,
                                           LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL);
    channel_table.slots[channel_slot].epoch = LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH;
    llam_public_slot_release(&channel_table,
                             channel_slot,
                             &channel_object,
                             channel_table.slots[channel_slot].generation);
    if (llam_public_slot_reserve_family(&channel_table,
                                        &mutex_object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &reused_slot,
                                        &reused_generation) != 0) {
        rc = fail_errno("family slot reserve after max epoch failed");
        goto cleanup;
    }
    if (reused_slot == channel_slot ||
        llam_public_slot_resolve(&channel_table, channel_slot, channel_generation) != NULL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] max-epoch family slot reused slot=%zu generation=%u\n",
                      reused_slot,
                      reused_generation);
        goto cleanup;
    }
    rc = 0;

cleanup:
    free(channel_table.slots);
    free(mutex_table.slots);
    free(reuse_table.slots);
    return rc;
}

static int compare_u32_values(const void *lhs, const void *rhs) {
    const uint32_t a = *(const uint32_t *)lhs;
    const uint32_t b = *(const uint32_t *)rhs;

    return (a > b) - (a < b);
}

static int test_public_slot_family_generation_window_is_injective(void) {
    enum { generation_count = 131072U };
    llam_public_slot_table_t table;
    int object = 1;
    uint32_t *generations = NULL;
    size_t slot = 0U;
    uint32_t generation = 0U;
    int rc = 1;

    memset(&table, 0, sizeof(table));
    table.handle_secret = UINT64_C(0x6a3d2b8f19c47e55);
    generations = malloc((size_t)generation_count * sizeof(*generations));
    if (generations == NULL) {
        return fail_errno("family generation window allocation failed");
    }
    if (llam_public_slot_reserve_family(&table,
                                        &object,
                                        1U,
                                        LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                        &slot,
                                        &generation) != 0) {
        rc = fail_errno("family generation window initial reserve failed");
        goto cleanup;
    }
    if (slot != 0U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] family generation window got unexpected slot=%zu\n",
                      slot);
        goto cleanup;
    }
    generations[0] = generation;
    for (uint32_t i = 1U; i < generation_count; ++i) {
        if (llam_public_slot_reactivate_family(&table,
                                               slot,
                                               &object,
                                               LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                               &generation) != 0) {
            rc = fail_errno("family generation window reactivation failed");
            goto cleanup;
        }
        generations[i] = generation;
    }

    /*
     * Public handles are consumed by generation, so a stale handle from any
     * earlier task/channel lifetime must not become valid again before the
     * internal epoch space is exhausted. This catches truncated-MAC token
     * schemes that only avoid adjacent-generation collisions.
     */
    qsort(generations, generation_count, sizeof(*generations), compare_u32_values);
    for (uint32_t i = 1U; i < generation_count; ++i) {
        if (generations[i] == generations[i - 1U]) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] family generation repeated within live epoch window: "
                          "generation=%u index=%u\n",
                          generations[i],
                          i);
            goto cleanup;
        }
    }
    rc = 0;

cleanup:
    free(generations);
    free(table.slots);
    return rc;
}

static int test_public_slot_mersenne_reducer_matches_modulus(void) {
    static const uint64_t values[] = {
        UINT64_C(0),
        UINT64_C(1),
        UINT64_C(268435454),
        UINT64_C(268435455),
        UINT64_C(268435456),
        UINT64_C(72057593769492480),
        UINT64_C(72057594037927935),
        UINT64_C(0xffffffffffffffff)
    };

    /*
     * Handle issuance uses a division-free Mersenne reducer on the spawn/join
     * hot path.  Keep a direct modulus oracle here so future tuning cannot
     * silently weaken stale-handle generation uniqueness.
     */
    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint32_t reduced = llam_public_slot_mersenne28_reduce(values[i]);
        uint32_t expected =
            (uint32_t)(values[i] % (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK);

        if (reduced != expected) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] mersenne reducer mismatch: "
                          "value=%" PRIu64 " reduced=%u expected=%u\n",
                          values[i],
                          reduced,
                          expected);
            return 1;
        }
    }
    {
        uint32_t stride = llam_public_slot_choose_affine_multiplier(UINT64_C(0x4f1bbcdc9a71e947));
        uint32_t addend = llam_public_slot_mersenne28_reduce(UINT64_C(0x86d1f23a6b9c4e05));
        uint32_t generation = 0U;

        for (uint32_t epoch = 1U; epoch <= 4096U; ++epoch) {
            uint32_t token;
            uint32_t expected_token;

            token = llam_public_slot_next_affine_token(generation,
                                                       LLAM_PUBLIC_HANDLE_FAMILY_TASK,
                                                       stride,
                                                       addend);
            expected_token = llam_public_slot_affine_epoch_token(epoch, stride, addend);
            if (token != expected_token) {
                (void)fprintf(stderr,
                              "[test_runtime_api_edges] affine token step mismatch: "
                              "epoch=%u token=%u expected=%u\n",
                              epoch,
                              token,
                              expected_token);
                return 1;
            }
            generation = llam_public_slot_family_generation(token, LLAM_PUBLIC_HANDLE_FAMILY_TASK);
        }
    }
    return 0;
}

static int test_public_slot_shift_bounds(void) {
    const unsigned word_bits = (unsigned)(sizeof(uintptr_t) * CHAR_BIT);
    size_t decoded_slot = 123U;
    uint32_t decoded_generation = 456U;
    uintptr_t encoded;

    /*
     * Public handle helpers are internal, but every opaque-handle family uses
     * them. Invalid shift values must fail closed instead of invoking C shift
     * UB, because sanitizer-only UB in a helper can mask the real caller fault.
     */
    encoded = llam_public_slot_encode_handle(0U, 1U, 0U);
    if (encoded != 0U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] zero public slot shift encoded handle=%" PRIuPTR "\n",
                      encoded);
        return 1;
    }
    if (llam_public_slot_decode_handle((uintptr_t)1U, 0U, &decoded_slot, &decoded_generation)) {
        (void)fprintf(stderr, "[test_runtime_api_edges] zero public slot shift decoded successfully\n");
        return 1;
    }

    encoded = llam_public_slot_encode_handle(0U, 1U, word_bits);
    if (encoded != 0U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] oversized public slot shift encoded handle=%" PRIuPTR "\n",
                      encoded);
        return 1;
    }
    if (llam_public_slot_decode_handle((uintptr_t)1U, word_bits, &decoded_slot, &decoded_generation)) {
        (void)fprintf(stderr, "[test_runtime_api_edges] oversized public slot shift decoded successfully\n");
        return 1;
    }
    if (decoded_slot != 123U || decoded_generation != 456U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] failed public slot decode mutated outputs: slot=%zu generation=%u\n",
                      decoded_slot,
                      decoded_generation);
        return 1;
    }

    if (word_bits > 32U) {
        const size_t unencodable_slot = (size_t)(UINTPTR_MAX >> 32U);

        encoded = llam_public_slot_encode_handle(unencodable_slot, 1U, 32U);
        if (encoded != 0U) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] unencodable public slot encoded handle=%" PRIuPTR "\n",
                          encoded);
            return 1;
        }
    }
    if (word_bits > 16U) {
        decoded_slot = 0U;
        decoded_generation = 0U;
        encoded = llam_public_slot_encode_handle(2U, 0x1234U, 16U);
        if (encoded == 0U ||
            !llam_public_slot_decode_handle(encoded, 16U, &decoded_slot, &decoded_generation) ||
            decoded_slot != 2U ||
            decoded_generation != 0x1234U) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] narrow public slot shift roundtrip failed: "
                          "encoded=%" PRIuPTR " slot=%zu generation=%u\n",
                          encoded,
                          decoded_slot,
                          decoded_generation);
            return 1;
        }
    }
    return 0;
}

static int test_public_active_op_overflow_fails_closed(void) {
    _Atomic size_t active_ops;

    atomic_init(&active_ops, SIZE_MAX);
    /*
     * active_ops gates public destroy paths. If begin wraps SIZE_MAX to zero,
     * a corrupted or saturated counter can make destroy believe no public
     * operation is active and open a UAF window. The only safe overflow policy
     * is to stay saturated so the object remains EBUSY until process teardown.
     */
    errno = 0;
    if (llam_public_active_op_try_begin(&active_ops) != -1 || errno != EBUSY) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] active op try_begin did not fail saturated counter with EBUSY\n");
        return 1;
    }
    if (llam_public_active_op_count(&active_ops) != SIZE_MAX) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] active op try_begin changed saturated counter to %zu\n",
                      llam_public_active_op_count(&active_ops));
        return 1;
    }
    llam_public_active_op_begin(&active_ops);
    if (llam_public_active_op_count(&active_ops) != SIZE_MAX) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] active op begin wrapped saturated counter to %zu\n",
                      llam_public_active_op_count(&active_ops));
        return 1;
    }
    llam_public_active_op_end(&active_ops);
    if (llam_public_active_op_count(&active_ops) != SIZE_MAX) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] active op end released saturated overflow sentinel to %zu\n",
                      llam_public_active_op_count(&active_ops));
        return 1;
    }
    return 0;
}

#if !LLAM_PLATFORM_WINDOWS
typedef struct public_op_sentinel_select_state {
    llam_channel_t *poisoned;
    llam_channel_t *peer;
    _Atomic int result;
} public_op_sentinel_select_state_t;

static void public_op_sentinel_noop_task(void *arg) {
    (void)arg;
}

static void public_op_sentinel_select_task(void *arg) {
    public_op_sentinel_select_state_t *state = arg;
    void *out = NULL;
    size_t selected = SIZE_MAX;
    llam_select_op_t ops[2];

    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->poisoned;
    ops[0].recv_out = &out;
    ops[0].send_value = NULL;
    ops[0].result_errno = 0;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->peer;
    ops[1].recv_out = &out;
    ops[1].send_value = NULL;
    ops[1].result_errno = 0;

    errno = 0;
    if (llam_channel_select(ops, 2U, 0U, &selected) == -1 && errno == EBUSY) {
        atomic_store_explicit(&state->result, 0, memory_order_release);
        return;
    }
    atomic_store_explicit(&state->result, errno != 0 ? errno : -1, memory_order_release);
}

static int test_public_op_sentinel_rejects_new_public_ops(void) {
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        return fail_errno("public-op sentinel reject fork failed");
    }
    if (pid == 0) {
        llam_channel_t *channel;
        llam_channel_t *raw_channel;
        llam_channel_t *select_peer;
        llam_mutex_t *mutex;
        llam_mutex_t *raw_mutex;
        llam_cond_t *cond;
        llam_cond_t *raw_cond;
        llam_task_group_t *group;
        llam_task_group_t *raw_group;
        llam_task_t *task;
        llam_task_t *raw_task;
        public_op_sentinel_select_state_t select_state;
        int value = 7;

        (void)alarm(2U);
        if (llam_runtime_init(NULL) != 0) {
            _exit(10);
        }
        channel = llam_channel_create(1U);
        select_peer = llam_channel_create(1U);
        mutex = llam_mutex_create();
        cond = llam_cond_create();
        group = llam_task_group_create();
        task = llam_spawn(public_op_sentinel_noop_task, NULL, NULL);
        if (channel == NULL || select_peer == NULL || mutex == NULL || cond == NULL || group == NULL || task == NULL) {
            _exit(11);
        }

        raw_channel = llam_channel_resolve_public_handle(channel);
        raw_mutex = llam_mutex_resolve_public_handle(mutex);
        raw_cond = llam_cond_resolve_public_handle(cond);
        raw_group = llam_task_group_resolve_public_handle(group);
        raw_task = llam_task_resolve_public_handle(task);
        if (raw_channel == NULL || raw_mutex == NULL || raw_cond == NULL || raw_group == NULL || raw_task == NULL) {
            _exit(12);
        }

        /*
         * A saturated active-op counter represents corrupted/exhausted
         * lifecycle accounting. New public operations must fail closed instead
         * of continuing to mutate the object and leaving destroy permanently
         * busy. The child exits without cleanup because the fixture is
         * intentionally poisoned.
         */
        atomic_store_explicit(&raw_channel->active_ops, SIZE_MAX, memory_order_release);
        atomic_store_explicit(&raw_mutex->active_ops, SIZE_MAX, memory_order_release);
        atomic_store_explicit(&raw_cond->active_ops, SIZE_MAX, memory_order_release);
        atomic_store_explicit(&raw_group->active_ops, SIZE_MAX, memory_order_release);
        atomic_store_explicit(&raw_task->active_ops, SIZE_MAX, memory_order_release);
        llam_channel_end_public_op(raw_channel);
        llam_mutex_end_public_op(raw_mutex);
        llam_cond_end_public_op(raw_cond);
        llam_task_group_end_public_op(raw_group);
        llam_task_end_public_op(raw_task);

        errno = 0;
        if (llam_channel_try_send(channel, &value) != -1 || errno != EBUSY) {
            _exit(13);
        }
        errno = 0;
        if (llam_mutex_trylock(mutex) != -1 || errno != EBUSY) {
            _exit(14);
        }
        errno = 0;
        if (llam_cond_signal(cond) != -1 || errno != EBUSY) {
            _exit(15);
        }
        errno = 0;
        if (llam_task_group_cancel(group) != -1 || errno != EBUSY) {
            _exit(16);
        }
        if (llam_task_id(task) != 0U || strcmp(llam_task_state_name(task), "UNKNOWN") != 0) {
            _exit(17);
        }
        select_state.poisoned = channel;
        select_state.peer = select_peer;
        atomic_init(&select_state.result, EINVAL);
        if (llam_spawn(public_op_sentinel_select_task, &select_state, NULL) == NULL) {
            _exit(18);
        }
        if (llam_run() != 0) {
            _exit(19);
        }
        if (atomic_load_explicit(&select_state.result, memory_order_acquire) != 0) {
            _exit(20);
        }
        _exit(0);
    }

    if (waitpid(pid, &status, 0) != pid) {
        return fail_errno("public-op sentinel reject waitpid failed");
    }
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGALRM) {
            return fail_msg("public-op sentinel reject test hung");
        }
        return fail_msg("public-op sentinel reject child died from signal");
    }
    if (!WIFEXITED(status)) {
        return fail_msg("public-op sentinel reject child did not exit cleanly");
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        return 0;
    case 10:
        return fail_msg("public-op sentinel reject runtime init failed");
    case 11:
        return fail_msg("public-op sentinel reject fixture allocation failed");
    case 12:
        return fail_msg("public-op sentinel reject fixture resolve failed");
    case 13:
        return fail_msg("channel public op did not fail saturated active-op sentinel with EBUSY");
    case 14:
        return fail_msg("mutex public op did not fail saturated active-op sentinel with EBUSY");
    case 15:
        return fail_msg("cond public op did not fail saturated active-op sentinel with EBUSY");
    case 16:
        return fail_msg("task group public op did not fail saturated active-op sentinel with EBUSY");
    case 17:
        return fail_msg("task introspection accepted saturated active-op sentinel");
    case 18:
        return fail_msg("public-op sentinel select task spawn failed");
    case 19:
        return fail_msg("public-op sentinel select runtime run failed");
    case 20:
        return fail_msg("channel select public op did not fail saturated active-op sentinel with EBUSY");
    default:
        return fail_msg("public-op sentinel reject child returned unexpected status");
    }
}

static int test_task_public_op_sentinel_teardown_does_not_hang(void) {
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        return fail_errno("task public-op sentinel fork failed");
    }
    if (pid == 0) {
        llam_task_t task;
        llam_task_t *slab = calloc(1U, sizeof(*slab));

        if (slab == NULL) {
            _exit(10);
        }
        /*
         * A saturated public-op counter is the fail-closed corruption sentinel.
         * Teardown must report EBUSY instead of spinning forever on a value no
         * valid public operation can drain.
         */
        memset(&task, 0, sizeof(task));
        llam_public_active_op_init(&task.active_ops);
        atomic_store_explicit(&task.active_ops, SIZE_MAX, memory_order_release);
        (void)alarm(2U);
        errno = 0;
        if (llam_task_wait_public_ops_quiescent(&task) == 0 || errno != EBUSY) {
            _exit(11);
        }

        llam_task_register_public_slab(slab, 1U);
        atomic_store_explicit(&slab[0].active_ops, SIZE_MAX, memory_order_release);
        errno = 0;
        if (llam_task_unregister_public_slab(slab, 1U) == 0 || errno != EBUSY) {
            _exit(12);
        }
        _exit(0);
    }

    if (waitpid(pid, &status, 0) != pid) {
        return fail_errno("task public-op sentinel waitpid failed");
    }
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGALRM) {
            return fail_msg("task public-op sentinel teardown hung");
        }
        return fail_msg("task public-op sentinel child died from signal");
    }
    if (!WIFEXITED(status)) {
        return fail_msg("task public-op sentinel child did not exit cleanly");
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        return 0;
    case 10:
        return fail_msg("task public-op sentinel fixture allocation failed");
    case 11:
        return fail_msg("task wait public-op sentinel did not fail with EBUSY");
    case 12:
        return fail_msg("task slab unregister public-op sentinel did not fail with EBUSY");
    default:
        return fail_msg("task public-op sentinel child returned unexpected status");
    }
}
#endif

static int test_public_channel_forged_initial_handle_rejected(void) {
    llam_channel_t *channel = NULL;
    llam_channel_t *forged = NULL;
    uintptr_t real_raw = 0U;
    size_t real_slot = 0U;
    uint32_t real_generation = 0U;
    int value = 17;
    void *out = NULL;
    int rc = 1;

    if (llam_runtime_init(NULL) != 0) {
        return fail_errno("forged channel handle runtime init failed");
    }

    channel = llam_channel_create(1U);
    if (channel == NULL) {
        rc = fail_errno("forged channel handle fixture allocation failed");
        goto cleanup;
    }
    real_raw = (uintptr_t)channel;
    real_slot = public_handle_slot(channel);
    real_generation = (uint32_t)real_raw;

    /*
     * This is the value a caller can guess if the first channel slot always
     * starts at generation == LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL.  It must not
     * be accepted as a capability for the live channel unless the caller was
     * actually given the encoded handle returned by llam_channel_create().
     */
    forged = (llam_channel_t *)llam_public_slot_encode_handle(0U,
                                                              LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                                              LLAM_SYNC_PUBLIC_HANDLE_SHIFT);

    errno = 0;
    if (llam_channel_try_send(forged, &value) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged channel handle accepted errno=%d\n",
                      errno);
        goto cleanup;
    }

    /*
     * Regression guard for the sealed generation token: a caller that knows the
     * slot number and tries sequential family-tagged epochs must still fail.
     * The real generation is skipped so this only exercises forged handles.
     */
    for (uint32_t epoch = 1U; epoch <= 4096U; ++epoch) {
        uint32_t guessed_generation =
            llam_public_slot_family_generation(epoch, LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL);

        if (guessed_generation == real_generation) {
            continue;
        }
        forged = (llam_channel_t *)llam_public_slot_encode_handle(real_slot,
                                                                  guessed_generation,
                                                                  LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
        errno = 0;
        if (llam_channel_try_send(forged, &value) != -1 || errno != EINVAL) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] sequential forged channel handle accepted "
                          "slot=%zu epoch=%u generation=%u real=%u errno=%d\n",
                          real_slot,
                          epoch,
                          guessed_generation,
                          real_generation,
                          errno);
            goto cleanup;
        }
    }

    if (llam_channel_try_recv_result(channel, &out) != -1 || errno != EAGAIN || out != NULL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged channel handle changed channel state errno=%d\n",
                      errno);
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (channel != NULL) {
        if (llam_channel_try_recv_result(channel, &out) == 0) {
            out = NULL;
        }
        (void)llam_channel_destroy(channel);
    }
    llam_runtime_shutdown();
    return rc;
}

typedef int (*forged_handle_probe_fn)(uintptr_t raw_handle, const char *label);

static int expect_forged_mutex_rejected(uintptr_t raw_handle, const char *label) {
    errno = 0;
    if (llam_mutex_trylock((llam_mutex_t *)raw_handle) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged mutex handle accepted at %s errno=%d\n",
                      label,
                      errno);
        return 1;
    }
    return 0;
}

static int expect_forged_cond_rejected(uintptr_t raw_handle, const char *label) {
    errno = 0;
    if (llam_cond_signal((llam_cond_t *)raw_handle) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged cond handle accepted at %s errno=%d\n",
                      label,
                      errno);
        return 1;
    }
    return 0;
}

static int expect_forged_cancel_token_rejected(uintptr_t raw_handle, const char *label) {
    errno = 0;
    if (llam_cancel_token_cancel((llam_cancel_token_t *)raw_handle) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged cancel-token handle accepted at %s errno=%d\n",
                      label,
                      errno);
        return 1;
    }
    return 0;
}

static int expect_forged_task_group_rejected(uintptr_t raw_handle, const char *label) {
    errno = 0;
    if (llam_task_group_cancel((llam_task_group_t *)raw_handle) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged task-group handle accepted at %s errno=%d\n",
                      label,
                      errno);
        return 1;
    }
    return 0;
}

static int expect_forged_task_rejected(uintptr_t raw_handle, const char *label) {
    const llam_task_t *task = (const llam_task_t *)raw_handle;

    /*
     * Introspection has no errno channel.  A forged task handle must therefore
     * resolve exactly like an unknown/consumed handle before any consuming path
     * is tried.
     */
    if (llam_task_id(task) != 0U ||
        strcmp(llam_task_state_name(task), "UNKNOWN") != 0 ||
        llam_task_flags(task) != 0U) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged task handle inspected live state at %s\n",
                      label);
        return 1;
    }

    errno = 0;
    if (llam_detach((llam_task_t *)raw_handle) != -1 || errno != EINVAL) {
        (void)fprintf(stderr,
                      "[test_runtime_api_edges] forged task handle detach accepted at %s errno=%d\n",
                      label,
                      errno);
        return 1;
    }
    return 0;
}

static int probe_family_forged_handles(const char *name,
                                       size_t real_slot,
                                       uint32_t real_generation,
                                       uint32_t family,
                                       forged_handle_probe_fn probe) {
    uintptr_t forged;

    /*
     * Exercise the attacker's cheapest guess first: slot zero plus the public
     * family tag.  This used to be predictable before per-runtime sealing was
     * added to public slot generation.
     */
    forged = llam_public_slot_encode_handle(0U, family, 32U);
    if (probe(forged, name) != 0) {
        return 1;
    }

    /*
     * Then try the low-cost sequential guesses for the real slot.  The actual
     * live generation is skipped; the test is only proving forged handles fail
     * closed without mutating the live object.
     */
    for (uint32_t epoch = 1U; epoch <= 4096U; ++epoch) {
        uint32_t guessed_generation = llam_public_slot_family_generation(epoch, family);

        if (guessed_generation == real_generation) {
            continue;
        }
        forged = llam_public_slot_encode_handle(real_slot, guessed_generation, 32U);
        if (probe(forged, name) != 0) {
            (void)fprintf(stderr,
                          "[test_runtime_api_edges] sequential forged %s handle accepted "
                          "slot=%zu epoch=%u generation=%u real=%u\n",
                          name,
                          real_slot,
                          epoch,
                          guessed_generation,
                          real_generation);
            return 1;
        }
    }
    return 0;
}

static int test_public_sync_forged_initial_handles_rejected(void) {
    llam_mutex_t *mutex = NULL;
    llam_cond_t *cond = NULL;
    llam_cancel_token_t *token = NULL;
    llam_task_group_t *group = NULL;
    int rc = 1;

    if (llam_runtime_init(NULL) != 0) {
        return fail_errno("forged sync/control handle runtime init failed");
    }

    mutex = llam_mutex_create();
    cond = llam_cond_create();
    token = llam_cancel_token_create();
    group = llam_task_group_create();
    if (mutex == NULL || cond == NULL || token == NULL || group == NULL) {
        rc = fail_errno("forged sync/control handle fixture allocation failed");
        goto cleanup;
    }

    if (probe_family_forged_handles("mutex",
                                    public_handle_slot(mutex),
                                    public_handle_generation(mutex),
                                    LLAM_PUBLIC_HANDLE_FAMILY_MUTEX,
                                    expect_forged_mutex_rejected) != 0 ||
        probe_family_forged_handles("cond",
                                    public_handle_slot(cond),
                                    public_handle_generation(cond),
                                    LLAM_PUBLIC_HANDLE_FAMILY_COND,
                                    expect_forged_cond_rejected) != 0 ||
        probe_family_forged_handles("cancel-token",
                                    public_handle_slot(token),
                                    public_handle_generation(token),
                                    LLAM_PUBLIC_HANDLE_FAMILY_CANCEL_TOKEN,
                                    expect_forged_cancel_token_rejected) != 0 ||
        probe_family_forged_handles("task-group",
                                    public_handle_slot(group),
                                    public_handle_generation(group),
                                    LLAM_PUBLIC_HANDLE_FAMILY_TASK_GROUP,
                                    expect_forged_task_group_rejected) != 0) {
        goto cleanup;
    }

    /*
     * Forged probes must not cancel, lock, or otherwise consume the live
     * objects.  Successful destroy on all fixtures is the final state check.
     */
    if (llam_cancel_token_is_cancelled(token) != 0) {
        (void)fprintf(stderr, "[test_runtime_api_edges] forged cancel-token probe cancelled token\n");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (mutex != NULL && llam_mutex_destroy(mutex) != 0) {
        rc = fail_errno("forged sync/control mutex cleanup failed");
    }
    if (cond != NULL && llam_cond_destroy(cond) != 0) {
        rc = fail_errno("forged sync/control cond cleanup failed");
    }
    if (token != NULL && llam_cancel_token_destroy(token) != 0) {
        rc = fail_errno("forged sync/control token cleanup failed");
    }
    if (group != NULL && llam_task_group_destroy(group) != 0) {
        rc = fail_errno("forged sync/control group cleanup failed");
    }
    llam_runtime_shutdown();
    return rc;
}

static int test_public_task_forged_initial_handle_rejected(void) {
    edge_state_t state;
    llam_task_t *task = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return fail_errno("forged task handle runtime init failed");
    }

    task = llam_spawn(ownership_task, &state, NULL);
    if (task == NULL) {
        rc = fail_errno("forged task handle fixture spawn failed");
        goto cleanup;
    }

    if (probe_family_forged_handles("task",
                                    public_handle_slot(task),
                                    public_handle_generation(task),
                                    LLAM_PUBLIC_HANDLE_FAMILY_TASK,
                                    expect_forged_task_rejected) != 0) {
        goto cleanup;
    }

    /*
     * The forged probes must not consume the live task handle.  If detach/join
     * claiming ever accepts a guessed generation, this final join either fails
     * or observes the wrong task lifecycle.
     */
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        rc = fail_errno("forged task handle changed real task lifecycle");
        goto cleanup;
    }
    if (llam_join(task) != 0) {
        rc = fail_errno("forged task handle real join failed");
        goto cleanup;
    }
    task = NULL;
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    llam_runtime_shutdown();
    return rc;
}

static int run_edge_case(const char *name, edge_case_fn fn) {
    /*
     * CI repeats this binary as a race detector.  Emit case boundaries to stderr
     * so timeout artifacts identify the stuck contract instead of only reporting
     * a silent process-level timeout.
     */
    fprintf(stderr, "[test_runtime_api_edges] begin %s\n", name);
    fflush(stderr);
    if (fn() != 0) {
        fprintf(stderr, "[test_runtime_api_edges] fail %s\n", name);
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "[test_runtime_api_edges] ok %s\n", name);
    fflush(stderr);
    return 0;
}

int main(void) {
#if !LLAM_PLATFORM_WINDOWS
    /*
     * Force the compensated direct-poll path before any poll policy is cached.
     * Runtime stop must still cancel parked infinite waits; otherwise this mode
     * can pin a scheduler thread inside poll(2).
     */
    if (setenv("LLAM_DIRECT_BLOCKING_POLL", "1", 1) != 0) {
        return fail_errno("enable direct blocking poll edge mode failed");
    }
    if (setenv("LLAM_DIRECT_BLOCKING_IO", "1", 1) != 0) {
        return fail_errno("enable direct blocking I/O edge mode failed");
    }
    if (setenv("LLAM_ACCEPT_DIRECT_BLOCKING", "1", 1) != 0) {
        return fail_errno("enable blocking accept edge mode failed");
    }
#endif
    if (run_edge_case("public_slot_generation_wrap_guard",
                      test_public_slot_generation_wrap_guard) != 0) {
        return 1;
    }
    if (run_edge_case("public_slot_family_tags_reject_wrong_family",
                      test_public_slot_family_tags_reject_wrong_family) != 0) {
        return 1;
    }
    if (run_edge_case("public_slot_family_generation_window_is_injective",
                      test_public_slot_family_generation_window_is_injective) != 0) {
        return 1;
    }
    if (run_edge_case("public_slot_mersenne_reducer_matches_modulus",
                      test_public_slot_mersenne_reducer_matches_modulus) != 0) {
        return 1;
    }
    if (run_edge_case("public_slot_shift_bounds",
                      test_public_slot_shift_bounds) != 0) {
        return 1;
    }
    if (run_edge_case("public_active_op_overflow_fails_closed",
                      test_public_active_op_overflow_fails_closed) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("task_public_op_sentinel_teardown_does_not_hang",
                      test_task_public_op_sentinel_teardown_does_not_hang) != 0) {
        return 1;
    }
    if (run_edge_case("public_op_sentinel_rejects_new_public_ops",
                      test_public_op_sentinel_rejects_new_public_ops) != 0) {
        return 1;
    }
#endif
    if (run_edge_case("public_channel_forged_initial_handle_rejected",
                      test_public_channel_forged_initial_handle_rejected) != 0) {
        return 1;
    }
    if (run_edge_case("public_sync_forged_initial_handles_rejected",
                      test_public_sync_forged_initial_handles_rejected) != 0) {
        return 1;
    }
    if (run_edge_case("public_task_forged_initial_handle_rejected",
                      test_public_task_forged_initial_handle_rejected) != 0) {
        return 1;
    }
    if (run_edge_case("cancel_token_destroy_race", test_cancel_token_destroy_race) != 0) {
        return 1;
    }
    if (run_edge_case("consumed_cancel_token_handle_reuse_guard",
                      test_consumed_cancel_token_handle_reuse_guard) != 0) {
        return 1;
    }
    if (run_edge_case("cancel_token_task_refcount_overflow_guard",
                      test_cancel_token_task_refcount_overflow_guard) != 0) {
        return 1;
    }
    if (run_edge_case("consumed_task_group_handle_reuse_guard",
                      test_consumed_task_group_handle_reuse_guard) != 0) {
        return 1;
    }
    if (run_edge_case("task_group_capacity_overflow_guard",
                      test_task_group_capacity_overflow_guard) != 0) {
        return 1;
    }
    if (run_edge_case("task_group_active_spawn_overflow_guard",
                      test_task_group_active_spawn_overflow_guard) != 0) {
        return 1;
    }
    if (run_edge_case("fault_boundary_contracts", test_fault_boundary_contracts) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("task_bootstrap_invariant_fails_closed",
                      test_task_bootstrap_invariant_fails_closed) != 0) {
        return 1;
    }
#endif
    if (run_edge_case("unmanaged_boundary_contracts", test_unmanaged_boundary_contracts) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_channel_try_drain_after_run",
                      test_unmanaged_channel_try_drain_after_run) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_channel_try_drain_after_shutdown",
                      test_unmanaged_channel_try_drain_after_shutdown) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_try_send_wakes_parked_channel_waiter",
                      test_unmanaged_try_send_wakes_parked_channel_waiter) != 0) {
        return 1;
    }
    if (run_edge_case("repeated_run_clears_internal_drain_stop",
                      test_repeated_run_clears_internal_drain_stop) != 0) {
        return 1;
    }
    if (run_edge_case("join_timeout_preserves_ownership", test_join_timeout_preserves_ownership) != 0) {
        return 1;
    }
    if (run_edge_case("duplicate_join_claim_race", test_duplicate_join_claim_race) != 0) {
        return 1;
    }
    if (run_edge_case("join_detach_claim_race", test_join_detach_claim_race) != 0) {
        return 1;
    }
    if (run_edge_case("consumed_task_handle_reuse_guard",
                      test_consumed_task_handle_reuse_guard) != 0) {
        return 1;
    }
    if (run_edge_case("detached_task_handle_consumed_immediately",
                      test_detached_task_handle_consumed_immediately) != 0) {
        return 1;
    }
    if (run_edge_case("destroyed_sync_handles_reject_public_ops",
                      test_destroyed_sync_handles_reject_public_ops) != 0) {
        return 1;
    }
    if (run_edge_case("active_public_ops_block_destroy",
                      test_active_public_ops_block_destroy) != 0) {
        return 1;
    }
    if (run_edge_case("consumed_channel_handle_reuse_guard",
                      test_consumed_channel_handle_reuse_guard) != 0) {
        return 1;
    }
    if (run_edge_case("consumed_sync_handle_reuse_guard",
                      test_consumed_sync_handle_reuse_guard) != 0) {
        return 1;
    }
    if (run_edge_case("blocking_callback_edges", test_blocking_callback_edges) != 0) {
        return 1;
    }
    if (run_edge_case("blocking_cancel_waits_for_running_callback",
                      test_blocking_cancel_waits_for_running_callback) != 0) {
        return 1;
    }
    if (run_edge_case("channel_close_and_select_edges", test_channel_close_and_select_edges) != 0) {
        return 1;
    }
    if (run_edge_case("channel_destroy_rejects_close_woken_waiter",
                      test_channel_destroy_rejects_close_woken_waiter) != 0) {
        return 1;
    }
    if (run_edge_case("cond_mutex_deadline_edges", test_cond_mutex_deadline_edges) != 0) {
        return 1;
    }
    if (run_edge_case("cond_owner_recheck_unlinks_waiter",
                      test_cond_owner_recheck_unlinks_waiter) != 0) {
        return 1;
    }
    if (run_edge_case("timer_heap_capacity_overflow_guard",
                      test_timer_heap_capacity_overflow_guard) != 0) {
        return 1;
    }
    if (run_edge_case("channel_select_excessive_op_count",
                      test_channel_select_excessive_op_count) != 0) {
        return 1;
    }
    if (run_edge_case("channel_cached_double_destroy_guard",
                      test_channel_cached_double_destroy_guard) != 0) {
        return 1;
    }
    if (run_edge_case("sync_double_destroy_guards", test_sync_double_destroy_guards) != 0) {
        return 1;
    }
    if (run_edge_case("precancel_wait_edges", test_precancel_wait_edges) != 0) {
        return 1;
    }
    if (run_edge_case("tiny_deadline_sleep_race", test_tiny_deadline_sleep_race) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("posix_io_completion_cancel_reorder",
                      test_posix_io_completion_cancel_reorder) != 0) {
        return 1;
    }
#endif
    if (run_edge_case("runtime_stop_cancels_parked_channel_wait",
                      test_runtime_stop_cancels_parked_channel_wait) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_late_channel_wait",
                      test_runtime_stop_cancels_late_channel_wait) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("runtime_stop_cancels_posix_poll_handle_wait",
                      test_runtime_stop_cancels_posix_poll_handle_wait) != 0) {
        return 1;
    }
    if (run_edge_case("cancel_token_cancels_blocking_socket_read",
                      test_cancel_token_cancels_blocking_socket_read) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_direct_blocking_read",
                      test_runtime_stop_cancels_direct_blocking_read) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_blocking_accept",
                      test_runtime_stop_cancels_blocking_accept) != 0) {
        return 1;
    }
#endif
    puts("test_runtime_api_edges ok");
    return 0;
}
