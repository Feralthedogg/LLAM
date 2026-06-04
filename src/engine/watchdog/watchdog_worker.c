/**
 * @file src/engine/watchdog/watchdog_worker.c
 * @brief Watchdog worker thread loop and periodic maintenance dispatch.
 *
 * @details
 * The controller thread periodically checks shard safepoints, nudges migrated
 * I/O watches, adjusts dynamic shard online state, and detects global deadlock
 * by comparing a runtime progress snapshot across watchdog intervals.
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

#include "engine/runtime_watchdog_internal.h"

#if LLAM_ARCH_AARCH64
#include <arm_neon.h>
#elif defined(__linux__) && LLAM_ARCH_X86_64 && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

#if defined(__linux__) && LLAM_ARCH_X86_64 && (defined(__GNUC__) || defined(__clang__))
#define LLAM_WATCHDOG_HAVE_AVX512 1
#else
#define LLAM_WATCHDOG_HAVE_AVX512 0
#endif

#define LLAM_METRIC_NEXT_FIELD(prev, next) \
    _Static_assert(offsetof(llam_metrics_t, next) == offsetof(llam_metrics_t, prev) + sizeof(uint64_t), \
                   "llam_metrics_t progress counter fields must remain contiguous")

LLAM_METRIC_NEXT_FIELD(ctx_switches, yields);
LLAM_METRIC_NEXT_FIELD(yields, parks);
LLAM_METRIC_NEXT_FIELD(parks, wakes);
LLAM_METRIC_NEXT_FIELD(wakes, timeout_wakes);
LLAM_METRIC_NEXT_FIELD(timeout_wakes, cancel_wakes);
LLAM_METRIC_NEXT_FIELD(steals, migrations);
LLAM_METRIC_NEXT_FIELD(blocking_calls, blocking_completions);
LLAM_METRIC_NEXT_FIELD(blocking_completions, io_submits);
LLAM_METRIC_NEXT_FIELD(io_submits, io_completions);

#if LLAM_RUNTIME_BACKEND_WINDOWS
/**
 * @brief Nudge shards that own timers on Windows.
 *
 * Windows 10 can occasionally miss the precise waitable-timer wake path under
 * dense task fanout.  The watchdog already runs at a 1ms cadence, so a cheap
 * pending-timer kick turns that failure mode into a bounded late wake instead
 * of leaving parked sleepers stranded.
 */
static void llam_watchdog_nudge_pending_shards(llam_runtime_t *rt) {
    unsigned i;

    if (rt == NULL) {
        return;
    }
    for (i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];
        bool has_runnable;

        if (atomic_load_explicit(&shard->timer_count, memory_order_acquire) != 0U) {
            llam_fire_expired_timers(shard);
            llam_kick_shard(shard);
            continue;
        }

        pthread_mutex_lock(&shard->lock);
        has_runnable = shard->inject_q.depth > 0U ||
                       shard->hot_q.depth > 0U ||
                       llam_norm_queue_depth(shard) > 0U;
        pthread_mutex_unlock(&shard->lock);
        if (has_runnable) {
            llam_kick_shard(shard);
        }
    }
}
#endif

#if !LLAM_ARCH_AARCH64 || LLAM_WATCHDOG_HAVE_AVX512
static uint64_t llam_watchdog_sum_u64_scalar(const uint64_t *values, size_t count) {
    uint64_t sum = 0U;
    size_t i;

    for (i = 0U; i < count; ++i) {
        sum += values[i];
    }
    return sum;
}
#endif

#if LLAM_WATCHDOG_HAVE_AVX512
__attribute__((target("avx512f")))
static uint64_t llam_watchdog_sum_u64_avx512(const uint64_t *values, size_t count) {
    __m512i acc = _mm512_setzero_si512();
    uint64_t lanes[8];
    uint64_t sum = 0U;
    size_t i = 0U;

    while (i + 8U <= count) {
        __m512i chunk = _mm512_loadu_si512((const void *)(values + i));

        acc = _mm512_add_epi64(acc, chunk);
        i += 8U;
    }
    if (i < count) {
        __mmask8 mask = (__mmask8)((1U << (count - i)) - 1U);
        __m512i chunk = _mm512_maskz_loadu_epi64(mask, values + i);

        acc = _mm512_add_epi64(acc, chunk);
    }
    _mm512_storeu_si512((void *)lanes, acc);
    for (i = 0U; i < 8U; ++i) {
        sum += lanes[i];
    }
    return sum;
}

static bool llam_watchdog_cpu_has_avx512f(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        value = __builtin_cpu_supports("avx512f") != 0 ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}
#endif

#if LLAM_ARCH_AARCH64
static uint64_t llam_watchdog_sum_u64_neon(const uint64_t *values, size_t count) {
    uint64x2_t acc = vdupq_n_u64(0U);
    uint64_t sum;
    size_t i = 0U;

    while (i + 2U <= count) {
        uint64x2_t chunk = vld1q_u64(values + i);

        acc = vaddq_u64(acc, chunk);
        i += 2U;
    }
    sum = vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
    if (i < count) {
        sum += values[i];
    }
    return sum;
}
#endif

static uint64_t llam_watchdog_sum_u64(const uint64_t *values, size_t count) {
#if LLAM_WATCHDOG_HAVE_AVX512
    if (llam_watchdog_cpu_has_avx512f()) {
        return llam_watchdog_sum_u64_avx512(values, count);
    }
#endif
#if LLAM_ARCH_AARCH64
    return llam_watchdog_sum_u64_neon(values, count);
#else
    return llam_watchdog_sum_u64_scalar(values, count);
#endif
}

static uint64_t llam_watchdog_metrics_progress_sum(const llam_metrics_t *metrics) {
    uint64_t first_group[6];
    uint64_t second_group[2];
    uint64_t third_group[4];
    uint64_t sum;

    /*
     * Keep the previous deadlock semantics: include externally visible
     * scheduler/I/O progress, but do not include watchdog-owned counters that
     * could mask a real stall.
     */
    first_group[0] = atomic_load_explicit(&metrics->ctx_switches, memory_order_relaxed);
    first_group[1] = atomic_load_explicit(&metrics->yields, memory_order_relaxed);
    first_group[2] = atomic_load_explicit(&metrics->parks, memory_order_relaxed);
    first_group[3] = atomic_load_explicit(&metrics->wakes, memory_order_relaxed);
    first_group[4] = atomic_load_explicit(&metrics->timeout_wakes, memory_order_relaxed);
    first_group[5] = atomic_load_explicit(&metrics->cancel_wakes, memory_order_relaxed);
    second_group[0] = atomic_load_explicit(&metrics->steals, memory_order_relaxed);
    second_group[1] = atomic_load_explicit(&metrics->migrations, memory_order_relaxed);
    third_group[0] = atomic_load_explicit(&metrics->blocking_calls, memory_order_relaxed);
    third_group[1] = atomic_load_explicit(&metrics->blocking_completions, memory_order_relaxed);
    third_group[2] = atomic_load_explicit(&metrics->io_submits, memory_order_relaxed);
    third_group[3] = atomic_load_explicit(&metrics->io_completions, memory_order_relaxed);

    sum = llam_watchdog_sum_u64(first_group, 6U);
    sum += llam_watchdog_sum_u64(second_group, 2U);
    sum += llam_watchdog_sum_u64(third_group, 4U);
    sum += atomic_load_explicit(&metrics->opaque_compensations, memory_order_relaxed);
    sum += atomic_load_explicit(&metrics->opaque_redirect_activations, memory_order_relaxed);
    sum += atomic_load_explicit(&metrics->total_run_ns, memory_order_relaxed);
    return sum;
}

/**
 * @brief Build a coarse monotonic progress counter for deadlock detection.
 *
 * The snapshot intentionally combines live task count and many per-shard metric
 * counters. Any change means the runtime is still making observable progress.
 *
 * @param rt Runtime to inspect.
 *
 * @return Aggregated progress value.
 */
static uint64_t llam_runtime_progress_snapshot(llam_runtime_t *rt) {
    uint64_t snapshot = llam_runtime_live_tasks(rt);
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        pthread_mutex_lock(&rt->shards[i].lock);
        snapshot += llam_watchdog_metrics_progress_sum(&rt->shards[i].metrics);
        pthread_mutex_unlock(&rt->shards[i].lock);
    }

    return snapshot;
}

/**
 * @brief Return whether a parked task can still be woken by an unmanaged host thread.
 *
 * @details
 * Some public non-parking APIs are explicitly valid outside a managed LLAM
 * task.  In particular, host threads may use channel try-send/try-recv/close
 * and condition-variable signal/broadcast to wake managed tasks that are
 * currently parked.  When any such waiter exists, the watchdog cannot prove a
 * global runtime deadlock only from an idle scheduler snapshot; the external
 * host event may simply not have happened yet.
 *
 * Mutex waiters are intentionally not included here because unmanaged
 * @c llam_mutex_unlock() is rejected with @c ENOTSUP, so a parked mutex waiter
 * does not have the same documented external wake path.
 */
static bool llam_runtime_has_external_sync_waiters(llam_runtime_t *rt) {
    unsigned i;

    if (rt == NULL || rt->shards == NULL) {
        return false;
    }
    for (i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];
        llam_task_t *task;

        pthread_mutex_lock(&shard->lock);
        for (task = shard->all_tasks; task != NULL; task = task->all_next) {
            unsigned state = atomic_load_explicit(&task->state, memory_order_acquire);
            unsigned reason = atomic_load_explicit(&task->wait_reason, memory_order_acquire);

            if (state == LLAM_TASK_STATE_PARKED &&
                (reason == LLAM_WAIT_CHANNEL_SEND ||
                 reason == LLAM_WAIT_CHANNEL_RECV ||
                 reason == LLAM_WAIT_COND)) {
                pthread_mutex_unlock(&shard->lock);
                return true;
            }
        }
        pthread_mutex_unlock(&shard->lock);
    }
    return false;
}

/**
 * @brief Main loop for the runtime controller/watchdog thread.
 *
 * @param arg Runtime pointer.
 *
 * @return Always @c NULL.
 */
void *llam_ctrl_worker_main(void *arg) {
    llam_runtime_t *rt = arg;

    llam_tune_ctrl_thread();

    for (;;) {
        uint64_t now_ns = llam_now_ns();
        unsigned i;

        /*
         * shutdown_requested is only published by teardown after the caller has
         * left the scheduler/run phase.  At that point the controller must not
         * wait for live_tasks to drain: explicit runtime_destroy() may be
         * tearing down a never-run runtime whose queued tasks can no longer make
         * progress.  Task/object quiescence is handled by the shutdown reclaim
         * and public active-op pins after this thread exits.
         */
        if (atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
            break;
        }

        for (i = 0; i < rt->active_shards; ++i) {
            llam_watchdog_check_shard(&rt->shards[i], now_ns);
        }

        llam_runtime_nudge_marked_watch_migrations(rt);
        llam_runtime_adjust_online_shards(rt);
#if LLAM_RUNTIME_BACKEND_WINDOWS
        if (llam_runtime_has_pending_timers(rt) || llam_runtime_has_runnable_backlog(rt)) {
            llam_watchdog_nudge_pending_shards(rt);
        }
#endif

        if (atomic_load(&rt->fatal_errno) == 0 &&
            llam_runtime_has_live_tasks(rt) &&
            !llam_runtime_has_runnable_work(rt) &&
            !llam_runtime_has_pending_timers(rt) &&
            atomic_load(&rt->block_pending) == 0U &&
            !llam_runtime_has_io_pending(rt) &&
            !llam_runtime_has_opaque_blocking(rt) &&
            !llam_runtime_has_external_sync_waiters(rt)) {
            uint64_t snapshot = llam_runtime_progress_snapshot(rt);

            if (snapshot == rt->deadlock_progress_snapshot) {
                rt->deadlock_probe_streak += 1U;
            } else {
                rt->deadlock_progress_snapshot = snapshot;
                rt->deadlock_probe_streak = 1U;
            }

            if (rt->deadlock_probe_streak >= LLAM_DEADLOCK_SUSPECT_STREAK) {
                for (i = 0; i < rt->active_shards; ++i) {
                    pthread_mutex_lock(&rt->shards[i].lock);
                    rt->shards[i].metrics.deadlock_suspicions += 1U;
                    pthread_mutex_unlock(&rt->shards[i].lock);
                }
                llam_record_fatal(rt, EDEADLK);
            }
        } else {
            rt->deadlock_progress_snapshot = llam_runtime_progress_snapshot(rt);
            rt->deadlock_probe_streak = 0U;
        }

        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)LLAM_WATCHDOG_INTERVAL_NS;
            nanosleep(&ts, NULL);
        }
    }

    return NULL;
}
