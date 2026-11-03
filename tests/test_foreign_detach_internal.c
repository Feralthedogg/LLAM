/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"

#include <errno.h>
#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef struct foreign_test_state {
    llam_runtime_t *runtime;
    llam_task_t *child;
    atomic_uint failures;
    atomic_uint child_done;
    atomic_uint root_done;
    atomic_uint cross_thread_done;
    atomic_int cross_thread_leave_rc;
    atomic_int cross_thread_leave_errno;
    uintptr_t primary_worker;
    uintptr_t child_worker;
    unsigned child_worker_role;
    bool child_external_alloc_pool;
} foreign_test_state_t;

static int fail(const char *message) {
    fprintf(stderr, "test_foreign_detach_internal: %s\n", message);
    return -1;
}

static void record_failure(foreign_test_state_t *state, const char *message) {
    fprintf(stderr, "test_foreign_detach_internal: %s\n", message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static int create_runtime(llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    *out = NULL;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    return llam_runtime_create(&opts, sizeof(opts), out);
}

static void *cross_thread_leave(void *arg) {
    foreign_test_state_t *state = arg;

    errno = 0;
    atomic_store_explicit(&state->cross_thread_leave_rc,
                          llam_leave_blocking(),
                          memory_order_relaxed);
    atomic_store_explicit(&state->cross_thread_leave_errno, errno, memory_order_relaxed);
    atomic_store_explicit(&state->cross_thread_done, 1U, memory_order_release);
    return NULL;
}

static void foreign_child(void *arg) {
    foreign_test_state_t *state = arg;
    llam_worker_t *worker = g_llam_tls_worker;

    if (worker == NULL || g_llam_tls_task == NULL || g_llam_tls_processor == NULL ||
        worker->current_p != g_llam_tls_processor ||
        atomic_load_explicit(&g_llam_tls_task->run_generation,
                             memory_order_acquire) == 0U) {
        record_failure(state, "foreign-spawn child did not receive a coherent M/P publication");
    } else {
        state->child_worker = (uintptr_t)worker;
        state->child_worker_role = (unsigned)worker->role;
        state->child_external_alloc_pool = g_llam_tls_task->alloc_external_pool;
    }
    atomic_store_explicit(&state->child_done, 1U, memory_order_release);
}

static bool snapshot_foreign(foreign_test_state_t *state,
                             llam_shard_t *shard,
                             llam_worker_t *worker,
                             llam_task_t *task,
                             llam_foreign_token_t *token_out,
                             unsigned *worker_state_out,
                             unsigned *depth_out,
                             llam_task_t **active_g_out,
                             llam_processor_t **current_p_out) {
    bool locked = false;

    if (state == NULL || shard == NULL || worker == NULL || task == NULL ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }
    if (token_out != NULL) {
        *token_out = worker->foreign_token;
    }
    if (worker_state_out != NULL) {
        *worker_state_out = atomic_load_explicit(&worker->execution_state,
                                                 memory_order_acquire);
    }
    if (depth_out != NULL) {
        *depth_out = atomic_load_explicit(&task->opaque_blocking_depth,
                                          memory_order_acquire);
    }
    if (active_g_out != NULL) {
        *active_g_out = llam_worker_load_active_g(worker);
    }
    if (current_p_out != NULL) {
        *current_p_out = atomic_load_explicit(&worker->current_p,
                                              memory_order_acquire);
    }
    locked = true;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return locked;
}

static bool wait_for_child(foreign_test_state_t *state) {
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);

    while (atomic_load_explicit(&state->child_done, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    return atomic_load_explicit(&state->child_done, memory_order_acquire) != 0U;
}

static void foreign_root(void *arg) {
    foreign_test_state_t *state = arg;
    llam_task_t *task = g_llam_tls_task;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_processor_owner_ticket_t initial_ticket;
    llam_processor_owner_ticket_t reacquired_ticket;
    llam_foreign_token_t first_token;
    llam_foreign_token_t nested_token;
    llam_foreign_token_t after_cross_thread_token;
    llam_foreign_token_t stale_token;
    pthread_t cross_thread;
    unsigned worker_state = 0U;
    unsigned depth = 0U;
    llam_task_t *active_g = NULL;
    llam_processor_t *current_p = NULL;
    bool cross_thread_started = false;
    int rc;

    if (task == NULL || worker == NULL || shard == NULL ||
        worker != &shard->primary_worker ||
        !llam_processor_owner_ticket_snapshot(&shard->processor, &initial_ticket) ||
        initial_ticket.owner_m != worker ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_MANAGED_SAFE ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) !=
            &shard->processor ||
        atomic_load_explicit(&worker->active_g, memory_order_acquire) != task ||
        atomic_load_explicit(&task->run_generation, memory_order_acquire) == 0U) {
        record_failure(state, "initial task/M/P publication was not coherent");
        goto done;
    }
    state->primary_worker = (uintptr_t)worker;

    errno = 0;
    rc = llam_enter_blocking();
    if (rc != 0 || errno != 0 ||
        !snapshot_foreign(state,
                          shard,
                          worker,
                          task,
                          &first_token,
                          &worker_state,
                          &depth,
                          &active_g,
                          &current_p) ||
        first_token.state != LLAM_FOREIGN_TOKEN_ACTIVE ||
        first_token.worker != worker || first_token.processor != &shard->processor ||
        first_token.task_run_generation !=
            atomic_load_explicit(&task->run_generation, memory_order_acquire) ||
        first_token.worker_run_generation !=
            atomic_load_explicit(&worker->run_generation, memory_order_acquire) ||
        first_token.worker_incarnation !=
            atomic_load_explicit(&worker->incarnation, memory_order_acquire) ||
        first_token.p_generation != initial_ticket.p_generation ||
        first_token.lease_projection_epoch != initial_ticket.lease_projection_epoch ||
        worker_state != LLAM_WORKER_EXEC_FOREIGN || depth != 1U ||
        active_g != NULL || current_p != NULL) {
        record_failure(state, "outer foreign enter did not publish a detached, token-bound M");
        goto recover;
    }

    if (pthread_create(&cross_thread, NULL, cross_thread_leave, state) != 0) {
        record_failure(state, "cross-thread leave probe could not start");
        goto recover;
    }
    cross_thread_started = true;
    pthread_join(cross_thread, NULL);
    cross_thread_started = false;
    if (atomic_load_explicit(&state->cross_thread_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state->cross_thread_leave_rc, memory_order_relaxed) != 0 ||
        !snapshot_foreign(state,
                          shard,
                          worker,
                          task,
                          &after_cross_thread_token,
                          &worker_state,
                          &depth,
                          &active_g,
                          &current_p) ||
        after_cross_thread_token.generation != first_token.generation ||
        depth != 1U || worker_state != LLAM_WORKER_EXEC_FOREIGN ||
        active_g != NULL || current_p != NULL) {
        record_failure(state, "cross-thread leave altered the M's foreign token or P ownership");
        goto recover;
    }

    state->child = llam_runtime_spawn_ex(state->runtime, foreign_child, state, NULL, 0U);
    if (state->child == NULL || !wait_for_child(state) ||
        state->child_worker == state->primary_worker ||
        !state->child_external_alloc_pool) {
        record_failure(state, "foreign spawn was not routed away from the detached primary M");
        goto recover;
    }

    /* A helper-backed foreign G is not an executor, even if yield is explicit. */
    errno = EDOM;
    llam_yield();
    if (g_llam_tls_task != task || g_llam_tls_worker != worker || errno != EDOM ||
        !snapshot_foreign(state, shard, worker, task, &after_cross_thread_token,
                          &worker_state, &depth, &active_g, &current_p) ||
        after_cross_thread_token.generation != first_token.generation ||
        worker_state != LLAM_WORKER_EXEC_FOREIGN || depth != 1U ||
        active_g != NULL || current_p != NULL ||
        atomic_load_explicit(&task->state, memory_order_acquire) !=
            LLAM_TASK_STATE_BLOCKED_OPAQUE) {
        record_failure(state, "foreign yield queued or migrated its native continuation");
        goto recover;
    }
    if (llam_join_until(state->child, llam_now_ns() + UINT64_C(5000000000)) != 0) {
        record_failure(state, "foreign child join failed");
        goto recover;
    }
    state->child = NULL;
    if (g_llam_tls_task != task || g_llam_tls_worker != worker || errno != EDOM ||
        !snapshot_foreign(state, shard, worker, task, &after_cross_thread_token,
                          &worker_state, &depth, &active_g, &current_p) ||
        after_cross_thread_token.generation != first_token.generation ||
        depth != 1U || worker_state != LLAM_WORKER_EXEC_FOREIGN ||
        active_g != NULL || current_p != NULL) {
        record_failure(state, "foreign join changed native binding or consumed its P token");
        goto recover;
    }

    errno = 0;
    rc = llam_enter_blocking();
    if (rc != 0 || errno != 0 ||
        !snapshot_foreign(state,
                          shard,
                          worker,
                          task,
                          &nested_token,
                          &worker_state,
                          &depth,
                          &active_g,
                          &current_p) ||
        nested_token.generation != first_token.generation || depth != 2U ||
        worker_state != LLAM_WORKER_EXEC_FOREIGN || active_g != NULL || current_p != NULL) {
        record_failure(state, "nested foreign enter changed the one-owner detach token");
        goto recover;
    }
    if (llam_leave_blocking() != 0 ||
        !snapshot_foreign(state,
                          shard,
                          worker,
                          task,
                          &nested_token,
                          &worker_state,
                          &depth,
                          &active_g,
                          &current_p) ||
        nested_token.generation != first_token.generation || depth != 1U ||
        worker_state != LLAM_WORKER_EXEC_FOREIGN || active_g != NULL || current_p != NULL) {
        record_failure(state, "nested foreign leave attempted to reacquire P prematurely");
        goto recover;
    }

    errno = 0;
    if (llam_leave_blocking() != 0 || errno != 0 ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U ||
        worker->foreign_token.state != LLAM_FOREIGN_TOKEN_INACTIVE ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_MANAGED_SAFE ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) !=
            &shard->processor ||
        atomic_load_explicit(&worker->active_g, memory_order_acquire) != task ||
        !llam_processor_owner_ticket_snapshot(&shard->processor, &reacquired_ticket) ||
        reacquired_ticket.owner_m != worker) {
        record_failure(state, "outer foreign leave did not validate a fresh P ticket before G publication");
        goto done;
    }

    errno = 0;
    if (llam_enter_blocking() != 0 || errno != 0) {
        record_failure(state, "second foreign detach failed before stale-token validation");
        goto done;
    }
    if (!snapshot_foreign(state,
                          shard,
                          worker,
                          task,
                          &stale_token,
                          &worker_state,
                          &depth,
                          &active_g,
                          &current_p)) {
        record_failure(state, "could not snapshot second foreign token");
        goto recover;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    worker->foreign_token.task_run_generation += 1U;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    errno = 0;
    rc = llam_leave_blocking();
    if (rc != -1 || errno != ECANCELED ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 1U ||
        worker->foreign_token.state != LLAM_FOREIGN_TOKEN_ACTIVE ||
        atomic_load_explicit(&worker->active_g, memory_order_acquire) != NULL ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL) {
        record_failure(state, "stale foreign run token falsely reacquired P or republished G");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    worker->foreign_token.task_run_generation = stale_token.task_run_generation;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    errno = 0;
    if (llam_leave_blocking() != 0 || errno != 0) {
        record_failure(state, "restored foreign token could not complete handback");
    }

recover:
    if (cross_thread_started) {
        pthread_join(cross_thread, NULL);
    }
    if (atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        if (worker->foreign_token.state == LLAM_FOREIGN_TOKEN_ACTIVE) {
            (void)llam_leave_blocking();
        }
    }
    if (state->child != NULL) {
        if (llam_join(state->child) != 0) {
            record_failure(state, "foreign-spawn child join failed");
        }
        state->child = NULL;
    }
done:
    atomic_store_explicit(&state->root_done, 1U, memory_order_release);
}

typedef struct foreign_join_probe {
    llam_runtime_t *runtime;
    llam_task_t *subject;
    llam_cancel_token_t *token;
    atomic_uint release_target;
    atomic_uint target_started;
    atomic_uint failures;
} foreign_join_probe_t;

static void foreign_join_target(void *opaque) {
    foreign_join_probe_t *probe = opaque;

    atomic_store_explicit(&probe->target_started, 1U, memory_order_release);
    while (atomic_load_explicit(&probe->release_target, memory_order_acquire) == 0U) {
        llam_yield();
    }
}

static void foreign_join_subject(void *opaque) {
    foreign_join_probe_t *probe = opaque;
    llam_worker_t *origin = g_llam_tls_worker;
    llam_task_t *self = g_llam_tls_task;
    llam_task_t *target = NULL;
    uint64_t deadline;
    bool foreign = false;
    int rc;

    if (llam_enter_blocking() != 0) {
        atomic_fetch_add(&probe->failures, 1U);
        return;
    }
    foreign = true;
    target = llam_runtime_spawn_ex(probe->runtime, foreign_join_target, probe, NULL, 0U);
    if (target == NULL) {
        atomic_fetch_add(&probe->failures, 1U);
        goto cleanup;
    }
    deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&probe->target_started, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&delay, NULL);
    }
    errno = 0;
    if (llam_join(probe->subject) != -1 || errno != EDEADLK) {
        atomic_fetch_add(&probe->failures, 1U);
    }
    errno = 0;
    if (llam_join_until(target, llam_now_ns() + UINT64_C(1000000)) != -1 || errno != ETIMEDOUT) {
        atomic_fetch_add(&probe->failures, 1U);
    }
    if (llam_cancel_token_cancel(probe->token) != 0) {
        atomic_fetch_add(&probe->failures, 1U);
    }
    errno = 0;
    if (llam_join(target) != -1 || errno != ECANCELED) {
        atomic_fetch_add(&probe->failures, 1U);
    }
    atomic_store_explicit(&probe->release_target, 1U, memory_order_release);
    /* Cancellation must not consume the handle/claim; completion can later win. */
    deadline = llam_now_ns() + UINT64_C(5000000000);
    do {
        errno = EDOM;
        rc = llam_join_until(target, deadline);
        if (rc == 0) {
            if (errno != EDOM) {
                atomic_fetch_add(&probe->failures, 1U);
            }
            target = NULL;
            break;
        }
        if (errno != ECANCELED) {
            atomic_fetch_add(&probe->failures, 1U);
            break;
        }
        {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            nanosleep(&delay, NULL);
        }
    } while (llam_now_ns() < deadline);
    if (target != NULL || g_llam_tls_worker != origin || g_llam_tls_task != self ||
        atomic_load_explicit(&self->opaque_blocking_depth, memory_order_acquire) != 1U ||
        origin->foreign_token.state != LLAM_FOREIGN_TOKEN_ACTIVE ||
        atomic_load_explicit(&origin->current_p, memory_order_acquire) != NULL) {
        atomic_fetch_add(&probe->failures, 1U);
    }
cleanup:
    atomic_store_explicit(&probe->release_target, 1U, memory_order_release);
    if (foreign && llam_leave_blocking() != 0) {
        atomic_fetch_add(&probe->failures, 1U);
    }
}

static void foreign_join_supervisor(void *opaque) {
    foreign_join_probe_t *probe = opaque;
    llam_spawn_opts_t opts;

    probe->token = llam_cancel_token_create();
    if (probe->token == NULL || llam_spawn_opts_init(&opts, sizeof(opts)) != 0) {
        atomic_fetch_add(&probe->failures, 1U);
        return;
    }
    opts.cancel_token = probe->token;
    probe->subject = llam_runtime_spawn_ex(probe->runtime, foreign_join_subject,
                                           probe, &opts, sizeof(opts));
    if (probe->subject == NULL || llam_join(probe->subject) != 0) {
        atomic_fetch_add(&probe->failures, 1U);
    }
    if (llam_cancel_token_destroy(probe->token) != 0) {
        atomic_fetch_add(&probe->failures, 1U);
    }
}

static int test_foreign_join_wait_contract(void) {
    foreign_join_probe_t probe = {0};
    llam_runtime_opts_t opts;
    llam_task_t *root;
    int result = 1;

    atomic_init(&probe.release_target, 0U);
    atomic_init(&probe.target_started, 0U);
    atomic_init(&probe.failures, 0U);
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return 1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create(&opts, sizeof(opts), &probe.runtime) != 0) {
        return 1;
    }
    root = llam_runtime_spawn_ex(probe.runtime, foreign_join_supervisor, &probe, NULL, 0U);
    if (root != NULL && llam_runtime_run_handle(probe.runtime) == 0 && llam_join(root) == 0 &&
        atomic_load_explicit(&probe.failures, memory_order_acquire) == 0U) {
        result = 0;
    }
    llam_runtime_destroy(probe.runtime);
    if (result != 0) {
        (void)fail("foreign join timeout/cancel/claim/errno contract failed");
    }
    return result;
}

int main(void) {
    foreign_test_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    int failures;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.child_done, 0U);
    atomic_init(&state.root_done, 0U);
    atomic_init(&state.cross_thread_done, 0U);
    atomic_init(&state.cross_thread_leave_rc, -99);
    atomic_init(&state.cross_thread_leave_errno, 0);

    if (llam_enter_blocking() != 0 || llam_leave_blocking() != 0) {
        return fail("blocking APIs changed their outside-task no-op contract") != 0;
    }
    if (create_runtime(&runtime) != 0 || runtime == NULL) {
        llam_runtime_destroy(runtime);
        return fail("could not create explicit runtime");
    }
    state.runtime = runtime;
    root = llam_runtime_spawn_ex(runtime, foreign_root, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(runtime) != 0 ||
        llam_join(root) != 0 ||
        atomic_load_explicit(&state.root_done, memory_order_acquire) == 0U) {
        record_failure(&state, "foreign detach root task did not complete");
    }
    failures = (int)atomic_load_explicit(&state.failures, memory_order_acquire);
    llam_runtime_destroy(runtime);
    if (failures != 0) {
        return 1;
    }
    if (test_foreign_join_wait_contract() != 0) {
        return 1;
    }
    puts("test_foreign_detach_internal: foreign-token lifecycle checks passed");
    return 0;
}
