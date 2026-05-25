/**
 * @file src/core/runtime_scheduler.c
 * @brief Worker scheduling loop, task dispatch, queue selection, and cooperative execution policy.
 *
 * @details
 * This translation unit contains the worker-side scheduler loops:
 *  - ::llam_scheduler_loop for primary shard workers,
 *  - ::llam_opaque_helper_main for temporary opaque-blocking compensation.
 *
 * Scheduler loop outline:
 *  - Establish thread-local shard/task/scheduler context.
 *  - Bind/tune the worker thread and install a signal stack for diagnostics.
 *  - Repeatedly drain inject work, fire timers, detect pressure, select a
 *    runnable task, switch into the task fiber, and reclaim dead tasks.
 *  - Cooperate with watchdog merge/steal pause requests.
 *  - Respect dynamic-worker online/offline transitions.
 *
 * Opaque helper behavior:
 *  - Sleeps until the primary worker enters an opaque blocking region.
 *  - Temporarily becomes the scheduler thread for the shard.
 *  - Yields ownership back to the primary worker once compensation depth
 *    reaches zero or shutdown begins.
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
 * @brief Mark a task as running on a shard and update dispatch metrics.
 *
 * This prepares thread-local task state before switching from the scheduler
 * context into the task fiber.  Optional timing is sampled only when runtime
 * profiling requires it to keep the hot dispatch path cheap.
 *
 * @param shard Scheduler shard dispatching the task.
 * @param task Runnable task selected by the scheduler.
 *
 * @return Start timestamp in nanoseconds when run timing is enabled, otherwise
 *         0.
 */
static uint64_t llam_set_task_running(llam_shard_t *shard, llam_task_t *task) {
    bool run_timing = shard->runtime->run_timing_enabled != 0U;
    bool wake_timing = task->last_runnable_ns > 0U && shard->runtime->wake_latency_metrics_enabled != 0U;
    bool preempt_timing = shard->runtime->preempt_mode >= LLAM_PREEMPT_AUTO;
    bool sample_safepoint = shard->runtime->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ||
                            (shard->metrics.ctx_switches & 63ULL) == 0U;
    uint64_t now_ns = (run_timing || wake_timing || preempt_timing || sample_safepoint) ? llam_now_ns() : 0U;

    atomic_store_explicit(&shard->current, task, memory_order_release);

    g_llam_tls_task = task;
    task->last_shard = shard->id;
    task->state = LLAM_TASK_STATE_RUNNING;
    task->last_started_ns = now_ns;
    if (wake_timing && now_ns >= task->last_runnable_ns) {
        shard->metrics.wake_latency_ns += now_ns - task->last_runnable_ns;
        shard->metrics.wake_samples += 1U;
    }
    atomic_store(&shard->last_run_started_ns, run_timing ? now_ns : 0U);
    if (now_ns != 0U) {
        atomic_store_explicit(&shard->last_safepoint_ns, now_ns, memory_order_relaxed);
    }
    llam_trace_shard(shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNABLE, LLAM_TASK_STATE_RUNNING, LLAM_WAIT_NONE);
    return run_timing ? now_ns : 0U;
}

/**
 * @brief Check whether a shard still owns local work.
 *
 * Dynamic workers use this check before staying offline.  Timers, queued work,
 * a currently running task, or opaque redirect state all count as local work.
 *
 * @param shard Scheduler shard to inspect.
 * @return true if the shard has work that requires an online worker.
 */
static bool llam_shard_has_local_work(llam_shard_t *shard) {
    bool has_work;

    pthread_mutex_lock(&shard->lock);
    has_work = shard->inject_q.depth > 0U || shard->hot_q.depth > 0U || llam_norm_queue_depth(shard) > 0U ||
               shard->timers != NULL ||
               atomic_load_explicit(&shard->current, memory_order_acquire) != NULL ||
               shard->opaque_redirect_active;
    pthread_mutex_unlock(&shard->lock);
    return has_work;
}

/**
 * @brief Treat an empty live-task set as a drained runtime.
 *
 * Task exit normally requests stop when the last task completes.  This guard
 * makes the scheduler robust against missed or reordered wake/stop observation:
 * once no managed tasks remain, the run loop is complete and all idle workers
 * should be woken so they can leave their scheduler loops.
 */
static bool llam_runtime_drained(llam_runtime_t *rt) {
    if (llam_runtime_has_live_tasks(rt)) {
        return false;
    }
    llam_request_stop(rt);
    return true;
}

/**
 * @brief Clear the current task after returning from a task fiber.
 *
 * This records run timing, stack samples, and slice-overrun metrics before the
 * scheduler chooses the next task.  It does not reclaim the task; reclamation
 * is handled after the caller observes the task state.
 *
 * @param shard Scheduler shard that just ran a task.
 * @param run_ns Measured run duration, or 0 when timing is disabled.
 */
static void llam_clear_current_task(llam_shard_t *shard, uint64_t run_ns) {
    llam_task_t *task = g_llam_tls_task;

    if (run_ns == 0U) {
        atomic_store_explicit(&shard->current, NULL, memory_order_release);
        g_llam_tls_task = NULL;
        return;
    }

    pthread_mutex_lock(&shard->lock);
    atomic_store_explicit(&shard->current, NULL, memory_order_release);
    if (task != NULL) {
        uint64_t slice_ns = llam_slice_ns((llam_task_class_t)atomic_load_explicit(&task->task_class, memory_order_acquire));

        task->last_run_ns = run_ns;
        task->total_run_ns += run_ns;
#if ((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) || LLAM_PLATFORM_WINDOWS) && LLAM_ARCH_X86_64
        llam_task_sample_stack_rsp(task, (uintptr_t)task->ctx.rsp);
#elif LLAM_ARCH_AARCH64
        llam_task_sample_stack_rsp(task, (uintptr_t)task->ctx.sp);
#endif
        shard->metrics.slice_budget_ns += slice_ns;
        if (run_ns > shard->metrics.max_run_ns) {
            shard->metrics.max_run_ns = run_ns;
        }
        if (run_ns > slice_ns) {
            shard->metrics.slice_overruns += 1U;
        }
    }
    shard->metrics.total_run_ns += run_ns;
    pthread_mutex_unlock(&shard->lock);
    g_llam_tls_task = NULL;
}

/**
 * @brief Cooperate with watchdog merge/steal pause requests.
 *
 * The watchdog can request a shard pause before merging or rehoming runtime
 * state.  The scheduler acknowledges the pause, idles until the request clears,
 * and then resumes normal work selection.
 *
 * @param shard Scheduler shard checking pause state.
 * @return true if the loop paused and should restart its iteration.
 */
static bool llam_shard_pause_for_merge(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;

    if (rt == NULL) {
        return false;
    }

    if (llam_runtime_steal_pause_active(rt)) {
        atomic_store_explicit(&shard->steal_pause_ack, 1U, memory_order_release);
    } else {
        atomic_store_explicit(&shard->steal_pause_ack, 0U, memory_order_release);
    }

    if (!llam_shard_merge_pause_requested(shard)) {
        return false;
    }

    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);
    while (llam_shard_merge_pause_requested(shard)) {
        if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
            llam_runtime_drained(rt)) {
            break;
        }
        llam_idle_wait(shard);
        if (llam_runtime_steal_pause_active(rt)) {
            atomic_store_explicit(&shard->steal_pause_ack, 1U, memory_order_release);
        } else {
            atomic_store_explicit(&shard->steal_pause_ack, 0U, memory_order_release);
        }
    }
    atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);
    return true;
}

#if defined(__linux__)
/**
 * @brief Report whether the Linux opaque helper may block in the opaque wait path.
 *
 * @param shard Scheduler shard owned by the helper.
 * @return true when no timers require the helper to use the ordinary idle path.
 */
static bool llam_opaque_helper_can_opaque_wait(llam_shard_t *shard) {
    return atomic_load_explicit(&shard->timer_count, memory_order_acquire) == 0U;
}

/**
 * @brief Sleep the Linux opaque helper until work or a wake signal arrives.
 *
 * @param shard Scheduler shard owned by the helper.
 *
 * @note Caller holds shard->opaque_lock.  The function briefly publishes
 *       opaque-helper wait state so wake producers can avoid lost signals.
 */
static void llam_opaque_helper_wait_for_signal_locked(llam_shard_t *shard) {
    atomic_store_explicit(&shard->opaque_helper_opaque_wait, 1U, memory_order_release);
    if (atomic_exchange_explicit(&shard->event_pending, 0U, memory_order_acq_rel) != 0U) {
        atomic_store_explicit(&shard->opaque_helper_opaque_wait, 0U, memory_order_release);
        shard->last_idle_wake_ns = llam_now_ns();
        return;
    }
    llam_opaque_wake_wait(shard);
    atomic_store_explicit(&shard->opaque_helper_opaque_wait, 0U, memory_order_release);
    if (atomic_exchange_explicit(&shard->event_pending, 0U, memory_order_acq_rel) != 0U) {
        shard->last_idle_wake_ns = llam_now_ns();
    }
}
#endif

/**
 * @brief Primary scheduler loop executed by a shard worker thread.
 *
 * The loop runs until stop is requested and the live task count reaches zero.
 * It is responsible for all owner-side queue maintenance for the shard:
 * allocator quiescence, inject draining, timer firing, local task selection,
 * stealing, overflow draining, task context switching, and dead-task reclaim
 * publication.
 *
 * @param shard Scheduler shard assigned to this worker.
 *
 * @note This function never runs inside a LLAM task.  It owns
 *       g_llam_tls_scheduler_ctx for its worker thread.
 */
void llam_scheduler_loop(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;

    g_llam_tls_shard = shard;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = &shard->scheduler_ctx;
#if !LLAM_RUNTIME_BACKEND_WINDOWS
    shard->thread = pthread_self();
#endif
    shard->primary_thread = pthread_self();
    llam_bind_current_thread_to_cpu(shard->cpu_id);
    llam_tune_scheduler_thread(shard, false);
    if (llam_install_thread_signal_stack(shard) != 0) {
        llam_record_fatal(rt, errno);
        g_llam_tls_shard = NULL;
        g_llam_tls_task = NULL;
        g_llam_tls_scheduler_ctx = NULL;
        return;
    }

    while (!atomic_load(&rt->stop_requested) || llam_runtime_has_live_tasks(rt)) {
        llam_task_t *task;
        uint64_t started_ns;
        bool pressure;

        if (llam_runtime_drained(rt)) {
            break;
        }
        if (llam_shard_pause_for_merge(shard)) {
            continue;
        }
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
            llam_runtime_has_live_tasks(rt)) {
            /*
             * A task can enter a blocking wait after another task has already
             * requested runtime stop.  Re-run the stop cancellation pass from
             * the scheduler loop so those late parkers observe ECANCELED
             * instead of waiting for a producer that may never arrive.
             */
            llam_runtime_cancel_parked_waiters(rt);
        }
        if (rt->experimental_dynamic_shards != 0U &&
            atomic_load_explicit(&shard->online, memory_order_acquire) == 0U) {
            if (!llam_shard_has_local_work(shard)) {
                if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                    llam_runtime_drained(rt)) {
                    break;
                }
                llam_idle_wait(shard);
                continue;
            }
            atomic_store_explicit(&shard->online, 1U, memory_order_release);
            llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        }

        llam_allocator_quiescent(shard);
        llam_drain_inject_queue(shard);
        llam_fire_expired_timers(shard);
        pressure = llam_runtime_pressure_signal(rt);

        task = llam_take_local_task_with_pressure(shard, pressure);
        if (task == NULL) {
            if (pressure) {
                task = llam_take_overflow_task(rt);
            }
        }
        if (task == NULL) {
            task = llam_try_steal_task(rt, shard);
        }
        if (task == NULL) {
            task = llam_take_overflow_task(rt);
        }

        if (task == NULL) {
            if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                llam_runtime_drained(rt)) {
                break;
            }
            llam_idle_wait(shard);
            continue;
        }

        started_ns = llam_set_task_running(shard, task);
        shard->metrics.ctx_switches += 1U;
        llam_switch_scheduler_to_task(g_llam_tls_scheduler_ctx, task);
        task = g_llam_tls_task != NULL ? g_llam_tls_task : task;
        llam_clear_current_task(shard, started_ns != 0U ? llam_now_ns() - started_ns : 0U);
        if (task->state == LLAM_TASK_STATE_DEAD) {
            llam_task_release_stack(task);
            llam_task_mark_reclaim_ready(task);
            llam_try_reclaim_detached_task(rt, task);
        }
    }

    llam_uninstall_thread_signal_stack(shard);
    llam_channel_tls_cache_drain();
    /*
     * The caller returns to unmanaged host code after driving shard 0. Leaving
     * the shard cursor installed makes later host-side cleanup look like a
     * managed task from the last runtime, which can falsely reject explicit
     * handles from another runtime with EXDEV.
     */
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
}

/**
 * @brief Opaque-blocking helper scheduler loop.
 *
 * A helper thread is created for shards that need compensation while their
 * primary worker is inside opaque blocking code.  The helper temporarily uses
 * the shard's opaque scheduler context, drains normal scheduler work, and then
 * returns control when compensation is no longer needed.
 *
 * @param arg Scheduler shard pointer.
 * @return NULL when the helper exits.
 */
void *llam_opaque_helper_main(void *arg) {
    llam_shard_t *shard = arg;
    llam_runtime_t *rt = shard->runtime;

    g_llam_tls_shard = shard;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = &shard->opaque_scheduler_ctx;
    llam_bind_current_thread_to_cpu(shard->cpu_id);
    llam_tune_scheduler_thread(shard, true);
    if (llam_install_thread_signal_stack(shard) != 0) {
        pthread_mutex_lock(&shard->opaque_lock);
        shard->opaque_helper_failed = true;
        shard->opaque_helper_ready = false;
        atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
        llam_opaque_wake_signal(shard);
        pthread_mutex_unlock(&shard->opaque_lock);
        llam_record_fatal(rt, errno);
        return NULL;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    shard->opaque_helper_failed = false;
    shard->opaque_helper_ready = true;
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
    llam_opaque_wake_signal(shard);
    pthread_mutex_unlock(&shard->opaque_lock);

    for (;;) {
        pthread_mutex_lock(&shard->opaque_lock);
        while (!shard->opaque_helper_stop && shard->opaque_compensation_depth == 0U) {
            if (shard->opaque_helper_active) {
                shard->opaque_helper_active = false;
                atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
                shard->thread = shard->primary_thread;
#endif
                llam_opaque_wake_signal(shard);
            }
            llam_opaque_wake_wait(shard);
        }
        if (shard->opaque_helper_stop) {
            shard->opaque_helper_active = false;
            atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
            shard->thread = shard->primary_thread;
#endif
            llam_opaque_wake_signal(shard);
            pthread_mutex_unlock(&shard->opaque_lock);
            break;
        }
        shard->opaque_helper_active = true;
        atomic_store_explicit(&shard->opaque_helper_active_hint, 1U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
        shard->thread = pthread_self();
#endif
        llam_opaque_wake_signal(shard);
        pthread_mutex_unlock(&shard->opaque_lock);

        while (!atomic_load(&rt->stop_requested) || llam_runtime_has_live_tasks(rt)) {
            llam_task_t *task;
            uint64_t started_ns;
            bool pressure;
            bool keep_running;
            bool stop_requested;

            if (llam_runtime_drained(rt)) {
                break;
            }
            if (llam_shard_pause_for_merge(shard)) {
                continue;
            }
            if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
                llam_runtime_has_live_tasks(rt)) {
                llam_runtime_cancel_parked_waiters(rt);
            }
            llam_allocator_quiescent(shard);
            llam_drain_inject_queue(shard);
            llam_fire_expired_timers(shard);
            pressure = llam_runtime_pressure_signal(rt);

            task = llam_take_local_task_with_pressure(shard, pressure);
            if (task == NULL) {
                if (pressure) {
                    task = llam_take_overflow_task(rt);
                }
            }
            if (task == NULL) {
                if (pressure) {
                    task = llam_try_steal_task(rt, shard);
                }
            }
            if (task == NULL) {
                if (pressure) {
                    task = llam_take_overflow_task(rt);
                }
            }

            if (task == NULL) {
                pthread_mutex_lock(&shard->opaque_lock);
                stop_requested = shard->opaque_helper_stop;
                keep_running = shard->opaque_compensation_depth > 0U;
                if (!keep_running || stop_requested) {
                    shard->opaque_helper_active = false;
                    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
                    shard->thread = shard->primary_thread;
#endif
                    llam_opaque_wake_signal(shard);
                    pthread_mutex_unlock(&shard->opaque_lock);
                    break;
                }
#if defined(__linux__)
                if (llam_opaque_helper_can_opaque_wait(shard)) {
                    llam_opaque_helper_wait_for_signal_locked(shard);
                    pthread_mutex_unlock(&shard->opaque_lock);
                    continue;
                }
#endif
                pthread_mutex_unlock(&shard->opaque_lock);

                if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                    llam_runtime_drained(rt)) {
                    break;
                }
                llam_idle_wait(shard);
                continue;
            }

            started_ns = llam_set_task_running(shard, task);
            shard->metrics.ctx_switches += 1U;
            llam_switch_scheduler_to_task(g_llam_tls_scheduler_ctx, task);
            task = g_llam_tls_task != NULL ? g_llam_tls_task : task;
            llam_clear_current_task(shard, started_ns != 0U ? llam_now_ns() - started_ns : 0U);
            if (task->state == LLAM_TASK_STATE_DEAD) {
                llam_task_release_stack(task);
                llam_task_mark_reclaim_ready(task);
                llam_try_reclaim_detached_task(rt, task);
            }

            pthread_mutex_lock(&shard->opaque_lock);
            stop_requested = shard->opaque_helper_stop;
            keep_running = shard->opaque_compensation_depth > 0U;
            if (!keep_running || stop_requested) {
                shard->opaque_helper_active = false;
                atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
                shard->thread = shard->primary_thread;
#endif
                llam_opaque_wake_signal(shard);
                pthread_mutex_unlock(&shard->opaque_lock);
                if (stop_requested) {
                    goto out;
                }
                break;
            }
            pthread_mutex_unlock(&shard->opaque_lock);
        }
    }

out:
    llam_uninstall_thread_signal_stack(shard);
    llam_channel_tls_cache_drain();
    g_llam_tls_scheduler_ctx = NULL;
    pthread_mutex_lock(&shard->opaque_lock);
    shard->opaque_helper_ready = false;
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
#if !LLAM_RUNTIME_BACKEND_WINDOWS
    shard->thread = shard->primary_thread;
#endif
    llam_opaque_wake_signal(shard);
    pthread_mutex_unlock(&shard->opaque_lock);
    return NULL;
}

void *llam_shard_worker_main(void *arg) {
    llam_shard_t *shard = arg;

    llam_scheduler_loop(shard);
    return NULL;
}
