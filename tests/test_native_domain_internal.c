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
#include "llam/managed.h"

#include <stdio.h>

typedef struct domain_state {
    llam_runtime_t *runtime;
    llam_runtime_t *peer;
    llam_task_t *child;
    llam_mutex_t *mutex;
    llam_worker_t *worker;
    llam_task_t *task;
    atomic_uint failure;
    atomic_uint peer_progress;
    atomic_uint peer_ready;
    atomic_uint release_peer;
    atomic_uint child_runs;
    int peer_result;
} domain_state_t;

static domain_state_t *g_domain_test;
#if !LLAM_PLATFORM_WINDOWS
static volatile sig_atomic_t g_handler_depth;
static volatile sig_atomic_t g_handler_calls;
#endif

static void failure(domain_state_t *s, unsigned code) {
    unsigned zero = 0U;
    atomic_compare_exchange_strong_explicit(&s->failure, &zero, code,
                                           memory_order_relaxed, memory_order_relaxed);
}

static void native_callback(void) {
    domain_state_t *s = g_domain_test;
    int saved = errno;
#if LLAM_PLATFORM_WINDOWS
    DWORD saved_last_error = GetLastError();
#endif
    llam_managed_native_enter();
    if (errno != saved) failure(s, 1U);
#if LLAM_PLATFORM_WINDOWS
    if (GetLastError() != saved_last_error) failure(s, 25U);
#endif
    unsigned depth = atomic_load_explicit(&s->worker->native_domain_depth, memory_order_acquire);
    if (depth == 0U || depth > 2U || !llam_native_domain_blocked()) failure(s, 2U);
    if (llam_worker_load_active_g(s->worker) != s->task) failure(s, 3U);
    errno = 0;
    if (llam_current_task() != NULL || errno != ENOTSUP) failure(s, 4U);
    errno = 0;
    if (llam_mutex_lock(s->mutex) != -1 || errno != ENOTSUP) failure(s, 5U);
    errno = 0;
    if (llam_join(s->child) != -1 || errno != ENOTSUP) failure(s, 6U);
    errno = 0;
    if (llam_sleep_ns(1U) != -1 || errno != ENOTSUP) failure(s, 7U);
    errno = 0;
    llam_yield();
    if (errno != ENOTSUP || llam_worker_load_active_g(s->worker) != s->task) failure(s, 8U);
    llam_runtime_t *created = (llam_runtime_t *)(uintptr_t)1U;
    if (llam_runtime_create(NULL, 0U, &created) != -1 || errno != ENOTSUP || created != NULL) {
        failure(s, 9U);
    }
    errno = 0;
    llam_runtime_destroy(s->peer);
    if (errno != ENOTSUP || atomic_load_explicit(&s->peer->stop_requested, memory_order_acquire)) {
        failure(s, 10U);
    }
    if (depth == 1U) {
#if !LLAM_PLATFORM_WINDOWS
        if (g_handler_depth != 0) {
            if (raise(SIGUSR2) != 0) failure(s, 11U);
        } else
#endif
        {
            native_callback();
        }
    }
    errno = saved;
#if LLAM_PLATFORM_WINDOWS
    SetLastError(saved_last_error);
#endif
    llam_managed_native_leave();
    if (errno != saved) failure(s, 12U);
#if LLAM_PLATFORM_WINDOWS
    if (GetLastError() != saved_last_error) failure(s, 26U);
#endif
}

#if !LLAM_PLATFORM_WINDOWS
static void native_handler(int signo) {
    if (signo != SIGUSR2) return;
    ++g_handler_depth;
    ++g_handler_calls;
    native_callback();
    --g_handler_depth;
}
#endif

static void child_task(void *argument) {
    domain_state_t *s = argument;
    atomic_fetch_add_explicit(&s->child_runs, 1U, memory_order_relaxed);
}

static void peer_task(void *argument) {
    domain_state_t *s = argument;
    atomic_store_explicit(&s->peer_ready, 1U, memory_order_release);
    while (atomic_load_explicit(&s->release_peer, memory_order_acquire) == 0U) {
        if (llam_current_task() == NULL) failure(s, 13U);
        atomic_fetch_add_explicit(&s->peer_progress, 1U, memory_order_release);
        llam_yield();
    }
}

static void *peer_driver(void *argument) {
    domain_state_t *s = argument;
    s->peer_result = llam_runtime_run_handle(s->peer);
    return NULL;
}

static void root_task(void *argument) {
    domain_state_t *s = argument;
    s->worker = g_llam_tls_worker;
    s->task = g_llam_tls_task;
    s->mutex = llam_mutex_create();
    s->child = llam_spawn(child_task, s, NULL);
    if (s->worker == NULL || s->worker->native_domain_slot == 0U || s->mutex == NULL ||
        s->child == NULL || llam_mutex_lock(s->mutex) != 0) {
        failure(s, 14U);
        return;
    }
    llam_worker_t duplicate = {0};
    llam_native_domain_worker_init(&duplicate);
    if (llam_native_domain_bind(&duplicate) != -1 || errno != EBUSY || duplicate.native_domain_slot != 0U) {
        failure(s, 15U);
    }
    errno = E2BIG;
    native_callback();
    if (errno != E2BIG) failure(s, 16U);
#if !LLAM_PLATFORM_WINDOWS
    if (raise(SIGUSR2) != 0 || g_handler_calls != 2 || g_handler_depth != 0) failure(s, 17U);
#endif
    if (g_llam_tls_worker != s->worker || g_llam_tls_task != s->task ||
        llam_current_task() == NULL || llam_native_domain_blocked() ||
        atomic_load_explicit(&s->worker->native_domain_depth, memory_order_acquire) != 0U) failure(s, 18U);

    unsigned before = atomic_load_explicit(&s->peer_progress, memory_order_acquire);
    uint64_t deadline = llam_now_ns() + UINT64_C(1000000000);
    llam_managed_native_enter();
    while (atomic_load_explicit(&s->peer_progress, memory_order_acquire) <= before &&
           llam_now_ns() < deadline) llam_pause_cpu();
    llam_managed_native_leave();
    if (atomic_load_explicit(&s->peer_progress, memory_order_acquire) <= before) failure(s, 19U);
    if (llam_mutex_unlock(s->mutex) != 0 || llam_join(s->child) != 0 ||
        atomic_load_explicit(&s->child_runs, memory_order_acquire) != 1U) failure(s, 20U);
    llam_mutex_destroy(s->mutex);
}

static int finite_failure_case(bool depth_limit) {
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    llam_runtime_opts_t opts = {.deterministic = 1U};
    llam_ctx_t context = {0};
    if (runtime == NULL || llam_runtime_init_registered_rt(runtime, &opts, sizeof(opts), true, 1U) != 0 ||
        llam_worker_bind_registered(&runtime->managed_workers[0], &context) != 0) return 126;
    if (depth_limit) {
        atomic_store_explicit(&runtime->managed_workers[0].native_domain_depth, 65535U, memory_order_release);
        llam_managed_native_enter();
    } else {
        llam_managed_native_leave();
    }
    return 127;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--bad-leave") == 0) return finite_failure_case(false);
    if (argc == 2 && strcmp(argv[1], "--depth-limit") == 0) return finite_failure_case(true);
    if (argc != 1) return 2;
    domain_state_t s = {0};
    llam_runtime_opts_t opts = {.deterministic = 1U, .preempt_mode = LLAM_PREEMPT_OFF};
    pthread_t peer;
    llam_task_t *root;
    llam_task_t *other;
#if !LLAM_PLATFORM_WINDOWS
    struct sigaction action = {0}, old_action;
    action.sa_handler = native_handler;
    action.sa_flags = SA_NODEFER;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, &old_action) != 0) return 1;
#endif
    g_domain_test = &s;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &s.runtime) != 0 ||
        llam_runtime_create_compat(&opts, sizeof(opts), &s.peer) != 0) return 1;
    other = llam_runtime_spawn_ex(s.peer, peer_task, &s, NULL, 0U);
    root = llam_runtime_spawn_ex(s.runtime, root_task, &s, NULL, 0U);
    if (root == NULL || other == NULL || pthread_create(&peer, NULL, peer_driver, &s) != 0) return 1;
    uint64_t deadline = llam_now_ns() + UINT64_C(1000000000);
    while (atomic_load_explicit(&s.peer_ready, memory_order_acquire) == 0U && llam_now_ns() < deadline) {
        llam_pause_cpu();
    }
    int run = llam_runtime_run_handle(s.runtime);
    atomic_store_explicit(&s.release_peer, 1U, memory_order_release);
    pthread_join(peer, NULL);
    if (run != 0 || s.peer_result != 0 || llam_join(root) != 0 || llam_join(other) != 0) failure(&s, 21U);
    for (size_t i = 0U; i < s.runtime->worker_slot_count; ++i) {
        if (s.runtime->worker_slots[i]->native_domain_slot != 0U) failure(&s, 22U);
    }
    if (atomic_load_explicit(&g_llam_native_domains, memory_order_acquire) != 0U ||
        g_llam_tls_task != NULL || g_llam_tls_worker != NULL) failure(&s, 23U);
    llam_runtime_destroy(s.peer);
    llam_runtime_destroy(s.runtime);
#if !LLAM_PLATFORM_WINDOWS
    if (sigaction(SIGUSR2, &old_action, NULL) != 0) failure(&s, 24U);
#endif
    unsigned error = atomic_load_explicit(&s.failure, memory_order_acquire);
    if (error != 0U) {
        fprintf(stderr, "native domain regression %u\n", error);
        return 1;
    }
    puts("native domain: nested callbacks, native signals, API rejection and peer progress passed");
    return 0;
}
