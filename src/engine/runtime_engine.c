/**
 * @file src/engine/runtime_engine.c
 * @brief Worker-thread engine startup, parking, notification, and runtime-wide worker coordination.
 *
 * @details
 * This file owns scheduler-side load balancing and idle waiting:
 *  - work stealing chooses same-node victims before remote victims,
 *  - idle waits derive their timeout from the shard timer heap,
 *  - optional idle spinning catches short wake/sleep churn before entering the
 *    platform wait primitive,
 *  - Linux shards sleep on a private futex-backed wake word.
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

/**
 * @brief Snapshot the shard's normal runnable queue depth.
 *
 * @param shard Shard to inspect.
 *
 * @return Approximate normal queue depth.
 */
static unsigned nm_snapshot_norm_depth(nm_shard_t *shard) {
    return nm_norm_queue_depth(shard);
}

/**
 * @brief Decide whether a victim shard is worth stealing from.
 *
 * The policy avoids stealing during merge/steal pauses, avoids hot/injected or
 * redirecting victims, and applies a deeper threshold for lock-free normal
 * queues to prevent migration churn on short wakeup workloads.
 *
 * @param thief        Candidate stealing shard.
 * @param victim       Candidate victim shard.
 * @param victim_depth Snapshot of victim normal queue depth.
 * @param thief_depth  Snapshot of thief normal queue depth.
 *
 * @return @c true when stealing should proceed.
 */
static bool nm_should_steal_from_victim(nm_shard_t *thief, nm_shard_t *victim, unsigned victim_depth, unsigned thief_depth) {
    bool busy = false;

    if (thief == NULL || victim == NULL || victim == thief) {
        return false;
    }
    if (nm_runtime_steal_pause_active(thief->runtime) || nm_shard_merge_pause_requested(thief) ||
        nm_shard_merge_pause_requested(victim)) {
        return false;
    }
    if (victim_depth < 2U || victim_depth < thief_depth * 2U) {
        return false;
    }

    pthread_mutex_lock(&victim->lock);
    busy = victim->inject_q.depth > 0U || victim->hot_q.depth > 0U || victim->opaque_redirect_active;
    pthread_mutex_unlock(&victim->lock);
    if (busy) {
        // Hot/injected work usually has affinity or urgency; leave it local.
        return false;
    }

    if (nm_lockfree_normq_enabled(victim->runtime)) {
        // The lock-free deque is best with deep backlog; stealing tiny queues
        // mostly adds migration churn to short wake/sleep workloads.
        if (victim_depth < 8U || victim_depth <= thief_depth + 2U) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Move runnable tasks from a victim shard to a thief shard.
 *
 * The number of tasks stolen is proportional to victim depth. Pinned tasks are
 * returned to the victim inject queue because they must continue running on
 * their owner shard.
 *
 * @param thief  Shard attempting to steal work.
 * @param victim Shard donating work.
 *
 * @return Number of tasks migrated to @p thief.
 */
unsigned nm_steal_from_victim(nm_shard_t *thief, nm_shard_t *victim) {
    unsigned stolen = 0;
    unsigned victim_depth;
    unsigned thief_depth = nm_snapshot_norm_depth(thief);

    if (victim == thief || !nm_shard_is_online(thief) || !nm_shard_is_online(victim)) {
        return 0;
    }

    victim_depth = nm_snapshot_norm_depth(victim);
    if (!nm_should_steal_from_victim(thief, victim, victim_depth, thief_depth)) {
        return 0;
    }

    if (nm_lockfree_normq_enabled(victim->runtime)) {
        victim_depth = nm_max_unsigned(1U, victim_depth / 4U);
    } else {
        victim_depth /= 2U;
    }
    while (victim_depth > 0U) {
        nm_task_t *task;

        if (nm_lockfree_normq_enabled(victim->runtime)) {
            task = nm_norm_queue_steal(victim);
        } else {
            pthread_mutex_lock(&victim->lock);
            task = nm_queue_pop_tail(&victim->norm_q);
            if (task != NULL) {
                atomic_fetch_sub_explicit(&victim->norm_depth, 1U, memory_order_release);
            }
            pthread_mutex_unlock(&victim->lock);
        }
        if (task == NULL) {
            break;
        }
        if ((task->flags & NM_TASK_FLAG_PINNED) != 0U) {
            // Pinned tasks must remain on the victim; put them back on inject.
            pthread_mutex_lock(&victim->lock);
            task->enqueue_hot = 0U;
            if (nm_queue_push_bounded_locked(victim, &victim->inject_q, NM_INJECT_QUEUE_CAP, task)) {
                victim->metrics.inject_enqueues += 1U;
            }
            pthread_mutex_unlock(&victim->lock);
            nm_kick_shard(victim);
            victim_depth -= 1U;
            continue;
        }

        pthread_mutex_lock(&thief->lock);
        (void)nm_norm_queue_push_owner_locked(thief, task);
        thief->metrics.migrations += 1U;
        nm_trace_shard(thief, task, NM_TRACE_STEAL, NM_TASK_STATE_RUNNABLE, NM_TASK_STATE_RUNNABLE, NM_WAIT_NONE);
        pthread_mutex_unlock(&thief->lock);
        stolen += 1U;
        victim_depth -= 1U;
    }

    if (stolen > 0U) {
        pthread_mutex_lock(&thief->lock);
        thief->metrics.steals += stolen;
        pthread_mutex_unlock(&thief->lock);
    }
    return stolen;
}

/**
 * @brief Try to steal runnable work for an idle shard.
 *
 * Same-NUMA-node victims are preferred before remote victims. On success, the
 * function immediately takes a local task from the thief after migration.
 *
 * @param rt    Runtime containing the shard set.
 * @param shard Idle shard requesting work.
 *
 * @return Runnable task on success, or @c NULL when no suitable victim exists.
 */
nm_task_t *nm_try_steal_task(nm_runtime_t *rt, nm_shard_t *shard) {
    unsigned i;
    nm_shard_t *best_same_node = NULL;
    nm_shard_t *best_remote = NULL;
    unsigned best_same_depth = 0U;
    unsigned best_remote_depth = 0U;

    if (rt->deterministic != 0U || nm_runtime_online_shards(rt) < 2U || nm_runtime_steal_pause_active(rt)) {
        return NULL;
    }
    if (!nm_shard_is_online(shard)) {
        return NULL;
    }

    for (i = 0; i < rt->active_shards; ++i) {
        unsigned depth;

        // Prefer same-locality work first; remote stealing is the fallback below.
        if (rt->shards[i].id == shard->id || rt->shards[i].node_index != shard->node_index ||
            !nm_shard_is_online(&rt->shards[i])) {
            continue;
        }

        depth = nm_snapshot_norm_depth(&rt->shards[i]);
        if (depth > best_same_depth) {
            best_same_depth = depth;
            best_same_node = &rt->shards[i];
        }
    }
    if (best_same_node != NULL && nm_steal_from_victim(shard, best_same_node) > 0U) {
        return nm_take_local_task(shard);
    }

    for (i = 0; i < rt->active_shards; ++i) {
        unsigned depth;

        if (rt->shards[i].node_index == shard->node_index || !nm_shard_is_online(&rt->shards[i])) {
            continue;
        }

        depth = nm_snapshot_norm_depth(&rt->shards[i]);
        if (depth > best_remote_depth) {
            best_remote_depth = depth;
            best_remote = &rt->shards[i];
        }
    }
    if (best_remote != NULL && nm_steal_from_victim(shard, best_remote) > 0U) {
        return nm_take_local_task(shard);
    }

    return NULL;
}

/**
 * @brief Compute the next idle wait timeout from the shard timer heap.
 *
 * @param shard       Shard whose timers are inspected.
 * @param has_timeout Optional output set when a timer exists.
 * @param timeout_ns  Optional precise nanosecond delta for platforms that can
 *                    wait with sub-millisecond precision.
 *
 * @return Timeout in milliseconds, 0 for already-expired timers, or -1 when no
 *         timer constrains the wait.
 */
static int nm_shard_next_timeout(nm_shard_t *shard, bool *has_timeout, uint64_t *timeout_ns) {
    int timeout_ms = -1;
    uint64_t now;

    if (has_timeout != NULL) {
        *has_timeout = false;
    }
    if (timeout_ns != NULL) {
        *timeout_ns = 0U;
    }
    if (atomic_load_explicit(&shard->timer_count, memory_order_acquire) == 0U) {
        return timeout_ms;
    }

    now = nm_now_ns();
    pthread_mutex_lock(&shard->lock);
    if (shard->timers != NULL) {
        if (has_timeout != NULL) {
            *has_timeout = true;
        }
        if (shard->timers->deadline_ns <= now) {
            timeout_ms = 0;
        } else {
            uint64_t delta_ns = shard->timers->deadline_ns - now;
            uint64_t delta_ms = delta_ns / 1000000ULL;

            if (timeout_ns != NULL) {
                *timeout_ns = delta_ns;
            }
            if (delta_ms == 0U) {
                delta_ms = 1U;
            }
            timeout_ms = delta_ms > (uint64_t)INT_MAX ? INT_MAX : (int)delta_ms;
        }
    }
    pthread_mutex_unlock(&shard->lock);
    return timeout_ms;
}

/**
 * @brief Check whether work became available during an idle-spin window.
 *
 * @param shard  Shard being idled.
 * @param now_ns Current monotonic timestamp.
 *
 * @return @c true when the shard should leave idle wait immediately.
 */
static bool nm_idle_spin_ready(nm_shard_t *shard, uint64_t now_ns) {
    nm_runtime_t *rt = shard->runtime;
    bool ready = false;

    if (atomic_load(&rt->stop_requested) && atomic_load(&rt->live_tasks) == 0U) {
        return true;
    }
    if (nm_runtime_overflow_depth(rt) > 0U) {
        // Overflow work is runtime-global, so any idle shard can help drain it.
        return true;
    }
    if (nm_norm_queue_depth(shard) > 0U) {
        return true;
    }

    pthread_mutex_lock(&shard->lock);
    ready = shard->inject_q.depth > 0U || shard->hot_q.depth > 0U || nm_norm_queue_depth(shard) > 0U;
    if (!ready && shard->timers != NULL && shard->timers->deadline_ns <= now_ns) {
        ready = true;
    }
    pthread_mutex_unlock(&shard->lock);
    return ready;
}

/**
 * @brief Decide whether idle spinning is worthwhile before sleeping.
 *
 * Spinning is limited to configured runtime budgets and is biased toward recent
 * wakeups or runtime pressure. Long quiet periods fall through to the platform
 * wait primitive.
 *
 * @param shard      Shard entering idle wait.
 * @param timeout_ms Computed timer wait timeout.
 * @param now_ns     Current monotonic timestamp.
 *
 * @return @c true when the shard should spin first.
 */
static bool nm_should_idle_spin(nm_shard_t *shard, int timeout_ms, uint64_t now_ns) {
    nm_runtime_t *rt = shard->runtime;
    uint64_t since_last_wake_ns;

    if (rt->idle_spin_ns == 0U || rt->idle_spin_max_iters == 0U) {
        return false;
    }
    if (timeout_ms == 0) {
        return false;
    }
    if (shard->last_idle_wake_ns == 0U || now_ns < shard->last_idle_wake_ns) {
        return nm_runtime_pressure_signal(rt);
    }

    since_last_wake_ns = now_ns - shard->last_idle_wake_ns;
    // Recent wakeups usually mean a short sleep would bounce immediately.
    if (since_last_wake_ns <= rt->idle_spin_ns * 4U) {
        return true;
    }
    return nm_runtime_pressure_signal(rt);
}

/**
 * @brief Spin for a short configured interval before blocking in idle wait.
 *
 * Metrics record whether the spin caught work or fell back to the platform wait
 * path. The loop uses CPU pause instructions rather than scheduler yields.
 *
 * @param shard      Shard entering idle wait.
 * @param timeout_ms Computed timer wait timeout.
 *
 * @return @c true when work became available during the spin.
 */
static bool nm_idle_spin(nm_shard_t *shard, int timeout_ms) {
    nm_runtime_t *rt = shard->runtime;
    uint64_t start_ns = nm_now_ns();
    uint64_t spin_budget_ns = rt->idle_spin_ns;
    unsigned max_iters = rt->idle_spin_max_iters;
    unsigned iters = 0U;

    if (!nm_should_idle_spin(shard, timeout_ms, start_ns)) {
        return false;
    }

    for (;;) {
        uint64_t now_ns;

        if (nm_idle_spin_ready(shard, nm_now_ns())) {
            uint64_t done_ns = nm_now_ns();

            pthread_mutex_lock(&shard->lock);
            shard->metrics.idle_spin_loops += iters;
            shard->metrics.idle_spin_hits += 1U;
            shard->metrics.idle_spin_ns += done_ns - start_ns;
            pthread_mutex_unlock(&shard->lock);
            shard->last_idle_wake_ns = done_ns;
            return true;
        }

        if (iters >= max_iters) {
            break;
        }

        nm_pause_cpu();
        iters += 1U;
        now_ns = nm_now_ns();
        if (now_ns < start_ns || now_ns - start_ns >= spin_budget_ns) {
            break;
        }
    }

    pthread_mutex_lock(&shard->lock);
    shard->metrics.idle_spin_loops += iters;
    shard->metrics.idle_spin_fallbacks += 1U;
    shard->metrics.idle_spin_ns += nm_now_ns() - start_ns;
    pthread_mutex_unlock(&shard->lock);
    return false;
}

#if defined(__linux__)
/**
 * @brief Sleep on a shard wake futex until work or timeout arrives.
 *
 * @param shard      Shard entering idle wait.
 * @param timeout_ms Computed timeout in milliseconds.
 */
static void nm_idle_futex_wait(nm_shard_t *shard, int timeout_ms) {
    int wait_timeout_ms = timeout_ms < 0 ? NM_IDLE_POLL_TIMEOUT_MS : timeout_ms;
    struct timespec ts;
    struct timespec *ts_ptr = NULL;

    if (atomic_exchange_explicit(&shard->event_pending, 0U, memory_order_acq_rel) != 0U) {
        shard->last_idle_wake_ns = nm_now_ns();
        return;
    }
    if (wait_timeout_ms == 0) {
        return;
    }
    if (wait_timeout_ms > 0) {
        ts.tv_sec = wait_timeout_ms / 1000;
        ts.tv_nsec = (long)(wait_timeout_ms % 1000) * 1000000L;
        ts_ptr = &ts;
    }

    for (;;) {
        long rc = nm_linux_futex_wait_private_timeout(&shard->event_pending, 0U, ts_ptr);

        if (rc == 0) {
            atomic_store_explicit(&shard->event_pending, 0U, memory_order_release);
            shard->last_idle_wake_ns = nm_now_ns();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            atomic_store_explicit(&shard->event_pending, 0U, memory_order_release);
            shard->last_idle_wake_ns = nm_now_ns();
        }
        return;
    }
}
#endif

/**
 * @brief Park an idle shard until work, timer expiry, or runtime shutdown.
 *
 * The function computes the nearest timer deadline, optionally idle-spins, then
 * sleeps using the backend wake mechanism. It is called by scheduler loops when
 * local queues and stealing produce no runnable task.
 *
 * @param shard Shard that has become idle.
 */
void nm_idle_wait(nm_shard_t *shard) {
    int timeout_ms;
#if defined(__APPLE__)
    bool has_precise_timeout = false;
    uint64_t precise_timeout_ns = 0U;
#endif

#if defined(__APPLE__)
    timeout_ms = nm_shard_next_timeout(shard, &has_precise_timeout, &precise_timeout_ns);
#else
    timeout_ms = nm_shard_next_timeout(shard, NULL, NULL);
#endif
    if (nm_idle_spin(shard, timeout_ms)) {
        return;
    }

    shard->metrics.idle_polls += 1U;
    nm_trace_shard(shard, NULL, NM_TRACE_IDLE, NM_TASK_STATE_RUNNABLE, NM_TASK_STATE_PARKED, NM_WAIT_NONE);

#if defined(__linux__)
    nm_idle_futex_wait(shard, timeout_ms);
#else
    for (;;) {
        int wait_timeout_ms = timeout_ms < 0 ? NM_IDLE_POLL_TIMEOUT_MS : timeout_ms;
#if defined(__APPLE__)
        uint64_t wait_timeout_ns =
            has_precise_timeout ? precise_timeout_ns : (uint64_t)wait_timeout_ms * 1000000ULL;
        // Darwin keeps the precise timer delta to avoid millisecond rounding drift.
        int rc = nm_wake_handle_wait_ns(shard->event_fd, wait_timeout_ms, wait_timeout_ns);
#else
        int rc = nm_wake_handle_wait(shard->event_fd, wait_timeout_ms);
#endif
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc > 0) {
            nm_drain_shard_wake(shard);
            shard->last_idle_wake_ns = nm_now_ns();
        }
        return;
    }
#endif
}
