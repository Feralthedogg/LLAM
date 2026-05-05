/**
 * @file src/engine/runtime_watchdog_worker.c
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

#include "runtime_watchdog_internal.h"

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
static uint64_t nm_runtime_progress_snapshot(nm_runtime_t *rt) {
    uint64_t snapshot = atomic_load(&rt->live_tasks);
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        pthread_mutex_lock(&rt->shards[i].lock);
        snapshot += rt->shards[i].metrics.ctx_switches;
        snapshot += rt->shards[i].metrics.yields;
        snapshot += rt->shards[i].metrics.parks;
        snapshot += rt->shards[i].metrics.wakes;
        snapshot += rt->shards[i].metrics.timeout_wakes;
        snapshot += rt->shards[i].metrics.cancel_wakes;
        snapshot += rt->shards[i].metrics.blocking_calls;
        snapshot += rt->shards[i].metrics.blocking_completions;
        snapshot += rt->shards[i].metrics.io_submits;
        snapshot += rt->shards[i].metrics.io_completions;
        snapshot += rt->shards[i].metrics.steals;
        snapshot += rt->shards[i].metrics.migrations;
        snapshot += rt->shards[i].metrics.opaque_compensations;
        snapshot += rt->shards[i].metrics.opaque_redirect_activations;
        snapshot += rt->shards[i].metrics.total_run_ns;
        pthread_mutex_unlock(&rt->shards[i].lock);
    }

    return snapshot;
}

/**
 * @brief Main loop for the runtime controller/watchdog thread.
 *
 * @param arg Runtime pointer.
 *
 * @return Always @c NULL.
 */
void *nm_ctrl_worker_main(void *arg) {
    nm_runtime_t *rt = arg;

    nm_tune_ctrl_thread();

    for (;;) {
        uint64_t now_ns = nm_now_ns();
        unsigned i;

        if (atomic_load(&rt->stop_requested) && atomic_load(&rt->live_tasks) == 0U) {
            break;
        }

        for (i = 0; i < rt->active_shards; ++i) {
            nm_watchdog_check_shard(&rt->shards[i], now_ns);
        }

        nm_runtime_nudge_marked_watch_migrations(rt);
        nm_runtime_adjust_online_shards(rt);

        if (atomic_load(&rt->fatal_errno) == 0 &&
            atomic_load(&rt->live_tasks) > 0U &&
            !nm_runtime_has_runnable_work(rt) &&
            !nm_runtime_has_pending_timers(rt) &&
            atomic_load(&rt->block_pending) == 0U &&
            !nm_runtime_has_io_pending(rt) &&
            !nm_runtime_has_opaque_blocking(rt)) {
            uint64_t snapshot = nm_runtime_progress_snapshot(rt);

            if (snapshot == rt->deadlock_progress_snapshot) {
                rt->deadlock_probe_streak += 1U;
            } else {
                rt->deadlock_progress_snapshot = snapshot;
                rt->deadlock_probe_streak = 1U;
            }

            if (rt->deadlock_probe_streak >= NM_DEADLOCK_SUSPECT_STREAK) {
                for (i = 0; i < rt->active_shards; ++i) {
                    pthread_mutex_lock(&rt->shards[i].lock);
                    rt->shards[i].metrics.deadlock_suspicions += 1U;
                    pthread_mutex_unlock(&rt->shards[i].lock);
                }
                nm_record_fatal(rt, EDEADLK);
            }
        } else {
            rt->deadlock_progress_snapshot = nm_runtime_progress_snapshot(rt);
            rt->deadlock_probe_streak = 0U;
        }

        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)NM_WATCHDOG_INTERVAL_NS;
            nanosleep(&ts, NULL);
        }
    }

    return NULL;
}
