/**
 * @file src/core/runtime_lifecycle.c
 * @brief Runtime lifecycle state transitions and guards against invalid API ordering.
 *
 * @details
 * Lifecycle checks are currently implemented directly in the init/run/shutdown
 * entry points. This file is kept as a small, explicit home for future shared
 * lifecycle helpers instead of mixing ordering policy into unrelated modules.
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

#include "runtime_internal.h"

/*
 * Runtime storage is byte-cleared during shutdown, so the lifecycle gate must
 * live outside individual runtime objects. It protects the construction window
 * before the public initialized flag is published and serializes shutdown
 * against partial-init cleanup. The mutex is recursive because init failure
 * paths call the shutdown helper while the initializing thread already owns it.
 */
#if LLAM_PLATFORM_WINDOWS
static INIT_ONCE g_llam_runtime_lifecycle_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_llam_runtime_lifecycle_lock;

static BOOL CALLBACK llam_runtime_lifecycle_lock_init_once(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_llam_runtime_lifecycle_lock);
    return TRUE;
}

static void llam_runtime_lifecycle_init(void) {
    if (!InitOnceExecuteOnce(&g_llam_runtime_lifecycle_once, llam_runtime_lifecycle_lock_init_once, NULL, NULL)) {
        abort();
    }
}
#else
static pthread_once_t g_llam_runtime_lifecycle_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_llam_runtime_lifecycle_lock;

static void llam_runtime_lifecycle_lock_init_once(void) {
    pthread_mutexattr_t attr;
    int rc;

    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        abort();
    }
    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (rc != 0) {
        pthread_mutexattr_destroy(&attr);
        abort();
    }
    rc = pthread_mutex_init(&g_llam_runtime_lifecycle_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        abort();
    }
}

static void llam_runtime_lifecycle_init(void) {
    pthread_once(&g_llam_runtime_lifecycle_once, llam_runtime_lifecycle_lock_init_once);
}
#endif

void llam_runtime_lifecycle_lock(void) {
#if LLAM_PLATFORM_WINDOWS
    llam_runtime_lifecycle_init();
    EnterCriticalSection(&g_llam_runtime_lifecycle_lock);
#else
    int rc;

    llam_runtime_lifecycle_init();
    rc = pthread_mutex_lock(&g_llam_runtime_lifecycle_lock);
    if (rc != 0) {
        abort();
    }
#endif
}

int llam_runtime_lifecycle_trylock(void) {
#if LLAM_PLATFORM_WINDOWS
    llam_runtime_lifecycle_init();
    if (!TryEnterCriticalSection(&g_llam_runtime_lifecycle_lock)) {
        errno = EBUSY;
        return -1;
    }
    return 0;
#else
    int rc;

    llam_runtime_lifecycle_init();
    rc = pthread_mutex_trylock(&g_llam_runtime_lifecycle_lock);
    if (rc != 0) {
        errno = rc == EBUSY ? EBUSY : rc;
        return -1;
    }
    return 0;
#endif
}

void llam_runtime_lifecycle_unlock(void) {
#if LLAM_PLATFORM_WINDOWS
    LeaveCriticalSection(&g_llam_runtime_lifecycle_lock);
#else
    int rc = pthread_mutex_unlock(&g_llam_runtime_lifecycle_lock);

    if (rc != 0) {
        abort();
    }
#endif
}

/**
 * @brief Return the total number of live managed tasks.
 *
 * The hot path maintains shard-local live counters and a runtime-wide count of
 * non-empty live shards.  Summing is intentionally reserved for diagnostics and
 * watchdog policy so spawn/exit do not contend on one global task counter.
 */
unsigned llam_runtime_live_tasks(llam_runtime_t *rt) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (rt == NULL) {
        return 0U;
    }
    return atomic_load_explicit(&rt->live_tasks, memory_order_acquire);
#else
    unsigned live = 0U;
    unsigned i;

    if (rt == NULL || rt->shards == NULL) {
        return 0U;
    }
    for (i = 0; i < rt->active_shards; ++i) {
        unsigned shard_live = atomic_load_explicit(&rt->shards[i].live_tasks, memory_order_acquire);

        /*
         * This total feeds diagnostics, watchdog policy, and stop-time parked
         * waiter cancellation sizing.  It must never wrap to a small value if a
         * shard counter is corrupted or saturated; fail closed by reporting the
         * maximum representable live count.
         */
        if (LLAM_UNLIKELY(live > UINT_MAX - shard_live)) {
            return UINT_MAX;
        }
        live += shard_live;
    }
    return live;
#endif
}

/**
 * @brief Return whether any shard still owns live managed tasks.
 */
bool llam_runtime_has_live_tasks(llam_runtime_t *rt) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return rt != NULL &&
           atomic_load_explicit(&rt->live_tasks, memory_order_acquire) != 0U;
#else
    return rt != NULL &&
           atomic_load_explicit(&rt->live_task_shards, memory_order_acquire) != 0U;
#endif
}

/**
 * @brief Increment a live-task counter and fail closed on saturation.
 *
 * @details
 * Live counters are scheduler liveness gates.  If corruption or an impossible
 * workload saturates one of them, wrapping to zero can make shutdown and stop
 * paths believe work is drained while tasks still exist.  Detect that state,
 * restore saturation, and mark the runtime fatal.  The saturation edge is not a
 * supported operational capacity target; it is a corruption/overflow guard.
 */
#if defined(__GNUC__) || defined(__clang__)
static inline __attribute__((always_inline)) bool
#else
static inline bool
#endif
llam_runtime_try_note_live_counter(llam_runtime_t *rt,
                                   atomic_uint *counter,
                                   unsigned *previous_out) {
    unsigned previous;

    previous = atomic_fetch_add_explicit(counter, 1U, memory_order_acq_rel);
    if (LLAM_UNLIKELY(previous == UINT_MAX)) {
        /*
         * This path only runs after a corrupted/saturated diagnostic counter.
         * Restore the saturated value so shutdown/drain observers do not keep
         * a wrapped zero after this edge returns, then mark the runtime fatal.
         * The common spawn path remains a single atomic increment.
         */
        atomic_store_explicit(counter, UINT_MAX, memory_order_release);
        llam_record_fatal(rt, EOVERFLOW);
        if (previous_out != NULL) {
            *previous_out = previous;
        }
        return false;
    }
    if (previous_out != NULL) {
        *previous_out = previous;
    }
    return true;
}

/**
 * @brief Mark a task live on its owner shard.
 *
 * Only the 0 -> 1 transition touches the runtime-wide live-shard counter.  This
 * keeps spawn-heavy workloads off a single global atomic cacheline.
 */
void llam_runtime_note_task_live(llam_runtime_t *rt, llam_shard_t *shard) {
    if (rt == NULL || shard == NULL) {
        return;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    (void)llam_runtime_try_note_live_counter(rt, &rt->live_tasks, NULL);
    (void)shard;
    return;
#else
    unsigned previous;

    if (!llam_runtime_try_note_live_counter(rt, &shard->live_tasks, &previous)) {
        return;
    }
    if (previous == 0U) {
        (void)llam_runtime_try_note_live_counter(rt, &rt->live_task_shards, NULL);
    }
#endif
}

/**
 * @brief Mark a task dead and report whether it was the final live task.
 */
bool llam_runtime_note_task_dead(llam_runtime_t *rt, llam_task_t *task) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    unsigned previous;
#else
    llam_shard_t *shard;
    unsigned shard_id;
    unsigned previous;
#endif

    if (rt == NULL || task == NULL || rt->shards == NULL || rt->active_shards == 0U) {
        return false;
    }

#if LLAM_RUNTIME_BACKEND_WINDOWS
    previous = atomic_fetch_sub_explicit(&rt->live_tasks, 1U, memory_order_acq_rel);
    if (previous == 0U) {
        atomic_fetch_add_explicit(&rt->live_tasks, 1U, memory_order_acq_rel);
        llam_record_fatal(rt, EINVAL);
        return false;
    }
    return previous == 1U;
#else
    shard_id = task->live_shard < rt->active_shards ? task->live_shard : task->home_shard;
    if (shard_id >= rt->active_shards) {
        shard_id = 0U;
    }
    shard = &rt->shards[shard_id];

    previous = atomic_fetch_sub_explicit(&shard->live_tasks, 1U, memory_order_acq_rel);
    if (previous == 0U) {
        atomic_fetch_add_explicit(&shard->live_tasks, 1U, memory_order_acq_rel);
        llam_record_fatal(rt, EINVAL);
        return false;
    }
    if (previous != 1U) {
        return false;
    }

    previous = atomic_fetch_sub_explicit(&rt->live_task_shards, 1U, memory_order_acq_rel);
    if (previous == 0U) {
        atomic_fetch_add_explicit(&rt->live_task_shards, 1U, memory_order_acq_rel);
        llam_record_fatal(rt, EINVAL);
        return false;
    }
    return previous == 1U;
#endif
}
