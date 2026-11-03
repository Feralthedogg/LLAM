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

#include "engine/runtime_watchdog_internal.h"
#if defined(LLAM_ENABLE_TEST_HOOKS)
#include "runtime_test_watchdog.h"
#include "async_context_phase.h"

static _Atomic(llam_watchdog_test_gate_t *) g_llam_watchdog_test_gate;

void llam_watchdog_test_set_gate(llam_watchdog_test_gate_t *gate) {
    atomic_store_explicit(&g_llam_watchdog_test_gate, gate, memory_order_release);
}

void llam_watchdog_test_scheduler_point(llam_shard_t *shard, llam_task_t *task, unsigned point) {
    llam_watchdog_test_gate_t *gate = atomic_load_explicit(&g_llam_watchdog_test_gate,
                                                          memory_order_acquire);
    unsigned expected = 0U;
    int saved_errno = errno;
    uint64_t deadline;
    if (gate == NULL || gate->runtime != shard->runtime || gate->point != point) return;
    if (point == LLAM_WATCHDOG_TEST_RETURN) {
#if LLAM_ASYNC_CONTEXT_SHARED
        llam_async_shared_checkpoint_t checkpoint;
        if (!llam_async_shared_test_checkpoint(shard->runtime, &checkpoint) ||
            checkpoint.capture_phase != LLAM_ASYNC_BANK_CAPTURED ||
            checkpoint.capture_ops == 0U || checkpoint.ready_depth != 0U) {
            errno = saved_errno;
            return;
        }
#else
        return;
#endif
    }
    if (!atomic_compare_exchange_strong_explicit(&gate->reached, &expected, 1U,
                                                 memory_order_acq_rel, memory_order_acquire)) return;
    /* This scheduler still owns task. Copy values now; no raw task escapes. */
    gate->selected_id = task->id;
    gate->selected_state = atomic_load_explicit(&task->state, memory_order_acquire);
    gate->selected_listed = atomic_load_explicit(&task->task_listed, memory_order_acquire);
    atomic_store_explicit(&gate->reached, 2U, memory_order_release);
    deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&gate->release, memory_order_acquire) == 0U) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        if (llam_now_ns() >= deadline) _Exit(126); /* Failure, never successful rescue. */
        (void)nanosleep(&delay, NULL);
    }
    errno = saved_errno;
}

static void llam_watchdog_test_record(llam_runtime_t *rt, uint64_t progress, unsigned decision) {
    llam_watchdog_test_gate_t *gate = atomic_load_explicit(&g_llam_watchdog_test_gate,
                                                          memory_order_acquire);
    llam_watchdog_test_record_t record = {0};
    unsigned remaining = 256U;
    if (gate == NULL || gate->runtime != rt ||
        atomic_load_explicit(&gate->decision_ready, memory_order_acquire) != 0U ||
        (gate->point != LLAM_WATCHDOG_TEST_NO_PAUSE &&
         atomic_load_explicit(&gate->reached, memory_order_acquire) != 2U)) return;
    record.decision = decision;
    record.streak = rt->deadlock_probe_streak;
    record.live = llam_runtime_live_tasks(rt);
    record.progress = progress;
    record.selected_id = gate->selected_id;
    record.selected_state = gate->selected_state;
    record.selected_listed = gate->selected_listed;
    for (unsigned i = 0U; i < rt->active_shards && i < 64U; ++i) {
        llam_shard_t *owner = &rt->shards[i];
        pthread_mutex_lock(&owner->opaque_lock);
        pthread_mutex_lock(&owner->lock);
        record.queues += owner->processor.inject_q.depth + owner->processor.hot_q.depth +
                         llam_norm_queue_depth(owner) + llam_async_preempt_ready_depth(owner);
        record.active += llam_shard_load_lease_current(owner) != NULL;
        for (llam_task_t *task = owner->all_tasks; task != NULL; task = task->all_next) {
            if (remaining == 0U) { record.truncated = 1U; break; }
            --remaining;
            ++record.listed;
            unsigned state = atomic_load_explicit(&task->state, memory_order_acquire);
            if (state <= LLAM_TASK_STATE_DEAD) ++record.states[state];
            else record.truncated = 1U;
            if (state == LLAM_TASK_STATE_PARKED) {
                unsigned reason = atomic_load_explicit(&task->wait_reason, memory_order_acquire);
                record.parked_mutex += reason == LLAM_WAIT_MUTEX;
                record.parked_join += reason == LLAM_WAIT_JOIN;
                record.resolver_busy += atomic_load_explicit(&task->wait_resolver_state,
                                                              memory_order_acquire) != 0U;
            }
            if (task->id == record.selected_id) {
                record.selected_seen = 1U;
                record.selected_state = state;
            }
        }
        pthread_mutex_unlock(&owner->lock);
        pthread_mutex_unlock(&owner->opaque_lock);
    }
    if (rt->active_shards > 64U) record.truncated = 1U;
#if LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_checkpoint_t checkpoint;
    llam_async_shared_stats_t stats;
    if (llam_async_shared_test_checkpoint(rt, &checkpoint)) {
        record.capture_ops = checkpoint.capture_ops;
        record.restore_ops = checkpoint.restore_ops;
        record.pending_phase = checkpoint.pending_phase;
        record.ready = checkpoint.ready_depth;
        record.mailbox = checkpoint.mailbox_state;
    }
    if (llam_async_shared_stats(rt, &stats)) record.lifetime_debt = stats.execution_debt;
#endif
    gate->record = record;
}

static void llam_watchdog_test_publish(llam_runtime_t *rt, unsigned decision) {
    llam_watchdog_test_gate_t *gate = atomic_load_explicit(&g_llam_watchdog_test_gate,
                                                          memory_order_acquire);
    if (gate == NULL || gate->runtime != rt || gate->record.streak == 0U ||
        atomic_load_explicit(&gate->decision_ready, memory_order_acquire) != 0U) return;
    gate->record.decision = decision;
    /* Publish AFTER the actual decision, not while fatal publication is still
     * pending. This observation never enters scheduler progress counters. */
    atomic_store_explicit(&gate->decision_ready, 1U, memory_order_release);
}
#else
#define llam_watchdog_test_record(rt, progress, decision) ((void)0)
#define llam_watchdog_test_publish(rt, decision) ((void)0)
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "windows_async_lifetime.h"
#if defined(LLAM_ENABLE_TEST_HOOKS)
static _Atomic(atomic_uint *) g_windows_async_ctrl_entry_observed =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_ctrl_entry_release =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_ctrl_entry_ticks =
    ATOMIC_VAR_INIT(NULL);

void llam_windows_async_test_ctrl_entry_gate(atomic_uint *observed,
                                            atomic_uint *release,
                                            atomic_uint *ticks) {
    atomic_store_explicit(&g_windows_async_ctrl_entry_observed, observed,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_ctrl_entry_release, release,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_ctrl_entry_ticks, ticks,
                          memory_order_release);
}

static void llam_windows_async_test_hold_ctrl_entry(void) {
    atomic_uint *observed = atomic_load_explicit(
        &g_windows_async_ctrl_entry_observed, memory_order_acquire);
    atomic_uint *release = atomic_load_explicit(
        &g_windows_async_ctrl_entry_release, memory_order_acquire);

    if (observed == NULL || release == NULL) {
        return;
    }
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}

static void llam_windows_async_test_note_ctrl_tick(void) {
    atomic_uint *ticks = atomic_load_explicit(
        &g_windows_async_ctrl_entry_ticks, memory_order_acquire);

    if (ticks != NULL) {
        (void)atomic_fetch_add_explicit(ticks, 1U, memory_order_release);
    }
}
#endif
#endif

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

        if (atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire) != 0U) {
            llam_fire_expired_timers(shard);
            llam_kick_shard(shard);
            continue;
        }

        pthread_mutex_lock(&shard->lock);
        has_runnable = shard->processor.inject_q.depth > 0U ||
                       shard->processor.hot_q.depth > 0U ||
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

uint64_t llam_watchdog_metrics_progress_sum(const llam_metrics_t *metrics) {
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
static void llam_ctrl_wait(llam_runtime_t *runtime) {
    long interval = (long)LLAM_WATCHDOG_INTERVAL_NS;
#if LLAM_ASYNC_CONTEXT_SHARED
    /* A GATE retains its fresh P until native restore. Service accepted native
     * work promptly instead of idling that P for a full policy/watchdog tick. */
    if (llam_async_shared_controller_urgent(runtime)) interval = 50000L;
    if (llam_async_shared_controller_wait(runtime, (uint64_t)interval)) return;
#else
    (void)runtime;
#endif
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = interval};
    (void)nanosleep(&delay, NULL);
}

void *llam_ctrl_worker_main(void *arg) {
    llam_runtime_t *rt = arg;
    bool native_progress = false;
#if LLAM_ASYNC_CONTEXT_SHARED
    uint64_t next_policy_ns = 0U;
#endif

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && defined(LLAM_ENABLE_TEST_HOOKS)
    /* Test-only first-action pause: no runtime lock or async op is owned. */
    llam_windows_async_test_hold_ctrl_entry();
#endif
    llam_tune_ctrl_thread(native_progress);

    for (;;) {
        bool native_enabled = atomic_load_explicit(&rt->async_preempt_enabled,
                                                   memory_order_acquire) != 0U;
        if (native_enabled != native_progress) {
            native_progress = native_enabled;
            llam_tune_ctrl_thread(native_progress);
        }
        uint64_t now_ns = llam_now_ns();
        unsigned i;
#if LLAM_ASYNC_CONTEXT_SHARED
        bool shared_async_pending = llam_async_shared_controller_tick(rt, now_ns);
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        bool windows_async_pending = llam_windows_async_lifetime_controller_tick(rt, now_ns);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_windows_async_test_note_ctrl_tick();
#endif
#endif

        /* Admission can close during a run. Keep only its required async drain
         * service; never run scaling/autotune after close. Never-run NEW Gs do
         * not keep this service alive (startup abort has its own release path). */
        if (llam_async_preempt_drain_service_needed(rt)) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)LLAM_WATCHDOG_INTERVAL_NS};
            for (i = 0U; i < rt->active_shards; ++i) {
                llam_async_preempt_drain_probe(&rt->shards[i], now_ns);
            }
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (atomic_load_explicit(&rt->shutdown_requested,
                                 memory_order_acquire)) {
#if LLAM_ASYNC_CONTEXT_SHARED
            if (shared_async_pending) {
                llam_ctrl_wait(rt);
                continue;
            }
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
            if (windows_async_pending) {
                /* Shutdown joins this controller before M unbind. Restore
                 * already-admitted G context without doing unrelated tuning. */
                const struct timespec delay = {
                    .tv_sec = 0,
                    .tv_nsec = (long)LLAM_WATCHDOG_INTERVAL_NS
                };

                (void)nanosleep(&delay, NULL);
                continue;
            }
#endif
            break;
        }
#if LLAM_ASYNC_CONTEXT_SHARED
        /* Faster native service must not accelerate scaling cooldowns,
         * autotuning or deadlock-suspect streaks measured in policy ticks. */
        if (now_ns < next_policy_ns) {
            llam_ctrl_wait(rt);
            continue;
        }
        next_policy_ns = now_ns <= UINT64_MAX - LLAM_WATCHDOG_INTERVAL_NS
            ? now_ns + LLAM_WATCHDOG_INTERVAL_NS : UINT64_MAX;
#endif

        /*
         * Lock-order-sensitive wait/accounting paths can only latch a fatal
         * errno: requesting stop there may synchronously reacquire the owner
         * shard/watch lock.  The controller owns the guaranteed lock-free
         * consumption point that turns that latch into cancellation and wakes.
         */
        if (atomic_load_explicit(&rt->exec_started, memory_order_acquire) &&
            atomic_exchange_explicit(&rt->deferred_fatal_pending,
                                     0U,
                                     memory_order_acq_rel) != 0U) {
            llam_request_stop(rt);
        }

        for (i = 0; i < rt->active_shards; ++i) {
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
            if (llam_windows_async_lifetime_slot_pending(rt, i)) {
                /* Do not signal a target inside the restore bridge/gate. */
                continue;
            }
#endif
            /* The probe derives and validates each target from the live lease. */
            llam_watchdog_check_shard(&rt->shards[i], now_ns);
        }

        llam_runtime_nudge_marked_watch_migrations(rt);
        llam_runtime_adjust_online_shards(rt);
        llam_watchdog_autotune_tick(rt, now_ns);
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
                if (rt->deadlock_probe_streak < LLAM_DEADLOCK_SUSPECT_STREAK) {
                    rt->deadlock_probe_streak += 1U;
                }
            } else {
                rt->deadlock_progress_snapshot = snapshot;
                rt->deadlock_probe_streak = 1U;
            }

            if (rt->deadlock_probe_streak >= LLAM_DEADLOCK_SUSPECT_STREAK) {
                if (llam_watchdog_confirm_deadlock(rt, snapshot)) {
                    llam_watchdog_test_record(rt, snapshot, LLAM_WATCHDOG_TEST_FATAL);
                    for (i = 0; i < rt->active_shards; ++i) {
                        pthread_mutex_lock(&rt->shards[i].lock);
                        rt->shards[i].metrics.deadlock_suspicions += 1U;
                        pthread_mutex_unlock(&rt->shards[i].lock);
                    }
                    llam_record_fatal(rt, EDEADLK);
                    llam_watchdog_test_publish(rt,
                        atomic_load_explicit(&rt->fatal_errno, memory_order_acquire) == EDEADLK
                            ? LLAM_WATCHDOG_TEST_FATAL : LLAM_WATCHDOG_TEST_INCONCLUSIVE);
                } else {
                    llam_watchdog_test_record(rt, snapshot, LLAM_WATCHDOG_TEST_INCONCLUSIVE);
                    llam_watchdog_test_publish(rt, LLAM_WATCHDOG_TEST_INCONCLUSIVE);
                }
            }
        } else {
            rt->deadlock_progress_snapshot = llam_runtime_progress_snapshot(rt);
            rt->deadlock_probe_streak = 0U;
        }

        llam_ctrl_wait(rt);
    }

    return NULL;
}
