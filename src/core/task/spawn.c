/**
 * @file src/core/task/spawn.c
 * @brief Task creation, spawn option normalization, and initial runnable queue insertion.
 *
 * @details
 * This translation unit implements the internal spawn path:
 *  - ::llam_spawn
 *
 * High-level behavior:
 *  - Validates runtime state and the task entry point.
 *  - Normalizes optional spawn settings into task class, stack class, flags,
 *    deadline, and cancellation-token state.
 *  - Allocates a task object and stack from the selected shard allocator.
 *  - Publishes the task in the runtime task list and marks it live.
 *  - Inserts the task into the correct runnable lane: local normal queue,
 *    remote inject queue, or opaque-blocking redirect queue.
 *
 * Scheduling notes:
 *  - Spawned tasks start on a chosen home shard and may later migrate unless
 *    pinned.
 *  - Local spawns avoid an immediate wake unless fanout reaches the configured
 *    wake interval.
 *  - Dynamic shards are brought online lazily when spawn targets a parked
 *    shard.
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

#include "runtime_internal.h"

/**
 * @brief Decide whether a local spawn burst should wake other scheduler shards.
 *
 * Local spawns normally avoid cross-worker wakeups because the current worker
 * can continue draining its own queue.  This helper reintroduces fanout only
 * after the target normal queue reaches a configured depth multiple.
 *
 * @param rt Runtime instance.
 * @param target Shard receiving the newly spawned task.
 *
 * @return true if all shards should be woken for fanout, false otherwise.
 *
 * @note Internal helper.  The caller does not need to hold @p target->lock;
 *       ::llam_norm_queue_depth provides the queue-depth snapshot used here.
 */
static bool llam_spawn_should_wake_fanout(llam_runtime_t *rt, llam_shard_t *target) {
    unsigned interval;
    unsigned depth;

    if (LLAM_UNLIKELY(rt == NULL || target == NULL || rt->active_shards <= 1U)) {
        return false;
    }
    interval = rt->spawn_fanout_wake_interval;
    if (LLAM_LIKELY(interval == 0U)) {
        return false;
    }
    depth = llam_norm_queue_depth(target);
    if (LLAM_LIKELY(depth < interval)) {
        return false;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (rt->spawn_fanout_adaptive != 0U &&
        rt->spawn_fanout_wake_interval_forced == 0U) {
        bool pressure = atomic_load_explicit(&rt->active_io_waiters, memory_order_acquire) != 0U;

        if (!pressure) {
            for (unsigned i = 0U; i < rt->active_shards; ++i) {
                if (atomic_load_explicit(&rt->shards[i].processor.timer_count, memory_order_acquire) != 0U) {
                    pressure = true;
                    break;
                }
            }
        }
        if (!pressure) {
            interval = llam_max_unsigned(256U, interval * 2U);
            if (interval > 512U) {
                interval = 512U;
            }
        }
    }
#endif
    return depth >= interval && (depth % interval) == 0U;
}

/**
 * @brief Try the same-shard spawn path without taking the shard mutex.
 *
 * @details
 * A task running on the primary scheduler thread is the single owner of the
 * shard's Chase-Lev bottom.  When tracing is disabled and the shard is not being
 * merge-paused, local spawns can publish directly to the lock-free normal lane.
 * This removes one pthread mutex pair from every child spawn in fanout-heavy
 * workloads while keeping remote spawn, debug tracing, and redirect paths on the
 * conservative locked implementation.
 *
 * @param rt          Runtime instance.
 * @param target      Current shard receiving the task.
 * @param task        Fully initialized task.
 * @param wake_fanout Set when this enqueue should kick all shards.
 * @return true if the task was published, false if the caller must use the locked path.
 */
static bool llam_spawn_try_local_unlocked(llam_runtime_t *rt,
                                          llam_shard_t *target,
                                          llam_task_t *task,
                                          bool *wake_fanout) {
    if (LLAM_UNLIKELY(rt == NULL || target == NULL || task == NULL || wake_fanout == NULL)) {
        return false;
    }
    if (LLAM_UNLIKELY(g_llam_tls_shard != target ||
                      g_llam_tls_task == NULL ||
                      !llam_tls_has_legacy_direct_handoff_context(target) ||
                      rt->trace_events_enabled != 0U ||
                      !llam_shard_accepts_new_work(target) ||
                      !llam_lockfree_normq_enabled(rt))) {
        return false;
    }

    task->state = LLAM_TASK_STATE_RUNNABLE;
    task->enqueue_hot = 0U;
    llam_runtime_note_task_live(rt, target);
    if (!llam_norm_queue_push_owner_unlocked(target, task)) {
        (void)llam_runtime_note_task_dead(rt, task);
        return false;
    }
    *wake_fanout = llam_spawn_should_wake_fanout(rt, target);
    return true;
}

/**
 * @brief Allocate a runtime task id with a local fast path for same-shard spawn.
 *
 * Local task fanout is single-writer on the primary scheduler thread.  Encoding
 * the shard id in the high bits preserves uniqueness without touching the
 * runtime-wide atomic counter on every child spawn.
 */
static uint64_t llam_spawn_next_task_id(llam_runtime_t *rt, llam_shard_t *target, bool local_spawn) {
    if (LLAM_LIKELY(target != NULL &&
                    local_spawn &&
                    llam_tls_has_legacy_direct_handoff_context(target))) {
        uint64_t seq = ++target->next_task_seq;

        return (((uint64_t)(target->id + 1U) & 0xffffULL) << 48U) |
               (seq & 0x0000ffffffffffffULL);
    }
    return (uint64_t)atomic_fetch_add_explicit(&rt->next_task_id, 1U, memory_order_relaxed) + 1U;
}

/**
 * @brief Drop the spawn-time cancellation-token reference before publication.
 *
 * Spawn increments the token refcount before stack allocation so the task can
 * safely observe the token once it becomes runnable. If a later
 * pre-publication step fails, the task is not visible to the scheduler or
 * cancellation waiter list yet, so cleanup only needs to return that retained
 * reference.
 *
 * @param task Task whose optional token reference should be released.
 */
static void llam_spawn_release_unpublished_cancel_token(llam_task_t *task) {
    if (task == NULL) return;
    if (task->cancel_token != NULL) llam_cancel_token_release_task_ref(task->cancel_token);
    if (task->group_cancel_token != NULL) llam_cancel_token_release_task_ref(task->group_cancel_token);
    task->cancel_token = NULL;
    task->group_cancel_token = NULL;
}

/** @brief Return true when @p task_class is a supported public task class. */
static bool llam_public_task_class_valid(uint32_t task_class) {
    return task_class == (uint32_t)LLAM_TASK_CLASS_LATENCY ||
           task_class == (uint32_t)LLAM_TASK_CLASS_DEFAULT ||
           task_class == (uint32_t)LLAM_TASK_CLASS_BATCH;
}

/** @brief Return true when @p stack_class is a supported public stack class. */
static bool llam_public_stack_class_valid(uint32_t stack_class) {
    return stack_class == (uint32_t)LLAM_STACK_CLASS_DEFAULT ||
           stack_class == (uint32_t)LLAM_STACK_CLASS_LARGE ||
           stack_class == (uint32_t)LLAM_STACK_CLASS_HUGE;
}

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#if defined(LLAM_ENABLE_TEST_HOOKS)
static _Atomic(llam_runtime_t *) g_windows_async_spawn_gate_runtime =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_spawn_gate_observed =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_spawn_gate_release =
    ATOMIC_VAR_INIT(NULL);

void llam_windows_async_test_host_spawn_gate(llam_runtime_t *runtime,
                                            atomic_uint *observed,
                                            atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_spawn_gate_observed, observed,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_spawn_gate_release, release,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_spawn_gate_runtime, runtime,
                          memory_order_release);
}

static void llam_windows_async_test_hold_host_spawn(llam_runtime_t *runtime) {
    atomic_uint *observed;
    atomic_uint *release;

    if (atomic_load_explicit(&g_windows_async_spawn_gate_runtime,
                             memory_order_acquire) != runtime) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_spawn_gate_observed,
                                    memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_spawn_gate_release,
                                   memory_order_acquire);
    if (observed == NULL || release == NULL) {
        return;
    }
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}
#endif

static void llam_windows_async_host_spawn_release(llam_runtime_t *rt) {
    if (pthread_mutex_lock(&rt->windows_async_config_lock) != 0) {
        abort();
    }
    if (rt->windows_async_host_spawn_admits == 0U) {
        abort();
    }
    rt->windows_async_host_spawn_admits -= 1U;
    pthread_mutex_unlock(&rt->windows_async_config_lock);
}
#endif

/**
 * @brief Return true when an ABI prefix contains a complete spawn option field.
 *
 * Size-aware public structs are copied defensively because old or foreign
 * bindings may provide a smaller prefix.  A partially present fixed-width field
 * is not a value; treating it as one would let a byte-sized prefix clobber the
 * default task class on little-endian targets.
 */
#define LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(prefix_size, field) \
    ((prefix_size) >= offsetof(llam_spawn_opts_t, field) + sizeof(((llam_spawn_opts_t *)0)->field))

/**
 * @brief Create a new runtime task and place it on a runnable queue.
 *
 * Algorithm outline:
 *  1) Validate runtime initialization and the task entry function.
 *  2) Resolve optional spawn policy into task class, stack class, flags,
 *     deadline, and cancellation-token ownership.
 *  3) Pick a home shard, allocate the task object, allocate its fiber stack,
 *     and initialize scheduler-visible state.
 *  4) Publish the task in the shard-local task list and increment live task count.
 *  5) Enqueue the task on the best queue for the current context:
 *       - opaque redirect queue when the target shard is blocked in opaque I/O,
 *       - inject queue for remote lock-free normal queue handoff,
 *       - owner normal queue for local owner-side enqueue.
 *  6) Wake the target shard or fan out wakeups when local spawn pressure
 *     warrants it.
 *
 * @param rt Runtime instance that will own the new task.
 * @param fn Task entry point. Must be non-NULL.
 * @param arg Opaque argument passed to @p fn when the task starts.
 * @param opts Optional spawn options. NULL selects default task/stack classes.
 * @param owning_group Optional task group that owns the returned borrowed
 *        diagnostics handle. NULL keeps the task as a normal join/detach handle.
 *
 * @return Newly allocated task handle on success, or NULL with errno set on
 *         failure.
 *
 * @note This is the shared implementation used by both explicit runtime
 *       handles and the legacy current/default runtime wrapper.
 */
static llam_task_t *llam_spawn_on_runtime_owned(llam_runtime_t *rt,
                                                llam_task_fn fn,
                                                void *arg,
                                                const llam_spawn_opts_t *opts,
                                                size_t opts_size,
                                                llam_task_group_t *owning_group) {
    llam_spawn_opts_t raw_opts;
    llam_spawn_opts_t opts_storage;
    llam_task_t *task;
    unsigned shard_id;
    llam_stack_class_t stack_class = LLAM_STACK_CLASS_DEFAULT;
    llam_task_class_t task_class = LLAM_TASK_CLASS_DEFAULT;
    llam_shard_t *target;
    size_t opts_copy_size;
    bool local_spawn;
    llam_processor_queue_access_t queue_access;
    bool queue_access_valid = false;
    bool task_list_eager;
    bool wake_fanout = false;
    llam_task_t *public_handle;
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    bool windows_async_host_reservation = false;
#endif

    if (opts != NULL) {
        if (LLAM_UNLIKELY(opts_size == 0U)) {
            errno = EINVAL;
            return NULL;
        }
        memset(&raw_opts, 0, sizeof(raw_opts));
        opts_copy_size = opts_size < sizeof(raw_opts) ? opts_size : sizeof(raw_opts);
        memcpy(&raw_opts, opts, opts_copy_size);

        memset(&opts_storage, 0, sizeof(opts_storage));
        opts_storage.task_class = (uint32_t)LLAM_TASK_CLASS_DEFAULT;
        opts_storage.stack_class = (uint32_t)LLAM_STACK_CLASS_DEFAULT;
        if (LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, task_class)) {
            opts_storage.task_class = raw_opts.task_class;
        }
        if (LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, stack_class)) {
            opts_storage.stack_class = raw_opts.stack_class;
        }
        if (LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, flags)) {
            opts_storage.flags = raw_opts.flags;
        }
        if (LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, deadline_ns)) {
            opts_storage.deadline_ns = raw_opts.deadline_ns;
        }
        if (LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, cancel_token)) {
            opts_storage.cancel_token = raw_opts.cancel_token;
        }
        opts = &opts_storage;
        if (LLAM_UNLIKELY(!llam_public_task_class_valid(opts->task_class) ||
                          !llam_public_stack_class_valid(opts->stack_class))) {
            errno = EINVAL;
            return NULL;
        }
    }

    if (LLAM_UNLIKELY(rt == NULL ||
                      !atomic_load_explicit(&rt->initialized, memory_order_acquire) ||
                      fn == NULL)) {
        errno = EINVAL;
        return NULL;
    }

    if (opts != NULL) {
        stack_class = (llam_stack_class_t)opts->stack_class;
        task_class = (llam_task_class_t)opts->task_class;
    }

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    /* A host/foreign spawn reserves the interval before live publication.
     * Enable checks this count under the same short gate and refuses rather
     * than waiting while a spawn owns allocator/shard locks. A task spawning
     * into its own running runtime cannot overlap pre-run configuration. */
    if (g_llam_tls_task == NULL || g_llam_tls_task->owner_runtime != rt) {
        int lock_error;

        if (!rt->windows_async_config_lock_initialized) {
            errno = EINVAL;
            return NULL;
        }
        /* Never park another runtime's managed M behind host configuration. */
        lock_error = g_llam_tls_task != NULL
                         ? pthread_mutex_trylock(&rt->windows_async_config_lock)
                         : pthread_mutex_lock(&rt->windows_async_config_lock);
        if (lock_error != 0) {
            errno = lock_error;
            return NULL;
        }
        if (rt->windows_async_host_spawn_admits == UINT_MAX) {
            pthread_mutex_unlock(&rt->windows_async_config_lock);
            errno = EOVERFLOW;
            return NULL;
        }
        rt->windows_async_host_spawn_admits += 1U;
        pthread_mutex_unlock(&rt->windows_async_config_lock);
        windows_async_host_reservation = true;
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_windows_async_test_hold_host_spawn(rt);
#endif
    }
#endif

    /* Admission owns the execution class before allocator/task-list placement.
     * A compatibility G must never be published on a portable P, and affine
     * reservations must be returned if allocation fails before publication. */
    llam_managed_admission_t admission;
    if (llam_managed_task_admit(rt, fn, opts != NULL ? opts->flags : 0U, &admission) != 0) {
        int saved_errno = errno;
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) llam_windows_async_host_spawn_release(rt);
#endif
        errno = saved_errno;
        return NULL;
    }
    if (admission.execution_worker != NULL) {
        shard_id = admission.execution_worker->compat_shard->id;
    } else {
        shard_id = llam_pick_spawn_shard(rt);
        if (admission.catalog != NULL) {
            if (rt->portable_processor_count == 0U) abort();
            shard_id %= rt->portable_processor_count;
        }
    }
    target = &rt->shards[shard_id];
    local_spawn = g_llam_tls_shard == target && g_llam_tls_task != NULL;
    if (local_spawn) {
        queue_access_valid =
            llam_processor_queue_access_snapshot(&target->processor, &queue_access) &&
            (queue_access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION ||
             queue_access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
        /* A detached M is a TLS-local observer, not the P's local producer. */
        local_spawn = queue_access_valid;
    }
    uint64_t admitted = atomic_load_explicit(&rt->admitted_tasks, memory_order_relaxed);
    int admission_error = 0;
    for (;;) {
        if (atomic_load_explicit(&rt->draining, memory_order_acquire) ||
            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
            admission_error = ECANCELED;
            break;
        }
        if (admitted == UINT64_MAX || (rt->max_tasks != 0U && admitted >= rt->max_tasks)) {
            admission_error = EAGAIN;
            break;
        }
        /* Capacity accounting only; the runnable queue publishes task storage. */
        if (atomic_compare_exchange_weak_explicit(&rt->admitted_tasks, &admitted, admitted + 1U,
                                                  memory_order_relaxed, memory_order_relaxed)) break;
    }
    if (admission_error != 0) {
        llam_managed_admission_release(&admission);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) llam_windows_async_host_spawn_release(rt);
#endif
        errno = admission_error;
        return NULL;
    }
    task = llam_task_alloc(target);
    if (LLAM_UNLIKELY(task == NULL)) {
        int saved_errno = errno;
        atomic_fetch_sub_explicit(&rt->admitted_tasks, 1U, memory_order_relaxed);
        llam_managed_admission_release(&admission);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) {
            llam_windows_async_host_spawn_release(rt);
        }
#endif
        errno = saved_errno;
        return NULL;
    }
    task->admission_counted = true;
    task->managed_catalog = admission.catalog;
    task->managed_execution_class = admission.execution_class;
    task->managed_execution_worker = admission.execution_worker;
    task->managed_execution_generation = admission.execution_generation;
    fn = admission.entry;
    task->id = llam_spawn_next_task_id(rt, target, local_spawn);
    task->state = LLAM_TASK_STATE_NEW;
    task->wait_reason = LLAM_WAIT_NONE;
    task->flags = admission.spawn_flags;
    if ((task->flags & LLAM_TASK_FLAG_LATENCY_CRITICAL) != 0U) {
        task_class = LLAM_TASK_CLASS_LATENCY;
    }
    atomic_store_explicit(&task->task_class, (unsigned)task_class, memory_order_release);
    atomic_store_explicit(&task->base_task_class, (unsigned)task_class, memory_order_release);
    task->deadline_ns = 0U;
    task->request_deadline_ns = opts != NULL ? opts->deadline_ns : 0U;
    task->cancel_token = opts != NULL ? opts->cancel_token : NULL;
    /*
     * Group-owned tasks must be marked before publication.  Marking after the
     * runnable enqueue lets a concurrently running child consume its borrowed
     * handle via llam_join/llam_detach before the group can claim ownership.
     */
    task->owning_group = owning_group;
    if (LLAM_UNLIKELY(task->cancel_token != NULL &&
                      llam_cancel_token_retain_task_ref_for_runtime(task->cancel_token, rt, &task->cancel_token) != 0)) {
        task->cancel_token = NULL;
        llam_task_allocator_free(task);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) {
            llam_windows_async_host_spawn_release(rt);
        }
#endif
        return NULL;
    }
    if (owning_group != NULL && (owning_group->flags & LLAM_TASK_GROUP_F_LINK_CANCEL) != 0U &&
        opts != NULL && opts->cancel_token != owning_group->cancel_token &&
        llam_cancel_token_retain_task_ref_for_runtime(owning_group->cancel_token, rt,
                                                      &task->group_cancel_token) != 0) {
        int saved_errno = errno;
        llam_spawn_release_unpublished_cancel_token(task);
        llam_task_allocator_free(task);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) llam_windows_async_host_spawn_release(rt);
#endif
        errno = saved_errno;
        return NULL;
    }
    task->entry = fn;
    task->arg = arg;
    task->forced_yield_budget = rt->forced_yield_every;
    atomic_store_explicit(&task->home_shard, shard_id, memory_order_relaxed);
    task->live_shard = shard_id;
    atomic_store_explicit(&task->last_shard, shard_id, memory_order_relaxed);
    atomic_store_explicit(&task->preempt_requested, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->completed, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->reclaim_ready, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->reclaim_claimed, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->join_claimed, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->detached, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->task_listed, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->scan_refs, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->join_waiter_hint, 0U, memory_order_relaxed);
    atomic_store_explicit(&task->active_io_req, NULL, memory_order_relaxed);
    task->join_waiter_count_at_exit = 0U;

    if (LLAM_UNLIKELY(llam_alloc_task_stack(task, stack_class) != 0)) {
        int saved_errno = errno;

        // Stack allocation is the only post-token failure point before the task
        // is published. Return the token reference here; otherwise destroy()
        // would see a permanently busy cancellation token after ENOMEM.
        llam_spawn_release_unpublished_cancel_token(task);
        llam_task_allocator_free(task);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (windows_async_host_reservation) {
            llam_windows_async_host_spawn_release(rt);
        }
#endif
        errno = saved_errno;
        return NULL;
    }

    task->last_runnable_ns = llam_runtime_should_stamp_runnable_latency(target) ? llam_now_ns() : 0U;
    task_list_eager = llam_task_list_eager_enabled(rt);
    /*
     * Capture the generation-bearing token before runnable publication. A
     * self-detaching child may invalidate and recycle its task immediately
     * after enqueue; the spawning thread must never re-read that container.
     */
    public_handle = llam_task_public_handle(task);

    if (!task_list_eager &&
        llam_spawn_try_local_unlocked(rt, target, task, &wake_fanout)) {
        if (wake_fanout) {
            llam_wake_all_shards(rt);
        }
    } else {
        pthread_mutex_lock(&target->lock);
        if (task_list_eager) {
            llam_add_task_to_list_locked(target, task);
        }
        llam_runtime_note_task_live(rt, target);
        if (rt->experimental_dynamic_shards != 0U &&
            llam_processor_try_set_online(&target->processor, true)) {
            llam_runtime_note_online_shards(rt,
                                          atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        }
        /* Newly spawned work is queued; only dispatch publishes worker active-G. */
        task->state = LLAM_TASK_STATE_RUNNABLE;
        if (target->opaque_redirect_active) {
            target->metrics.migrations += 1U;
            task->enqueue_hot = 0U;
            if (!llam_enqueue_opaque_redirect_task_locked(target, task, false)) {
                llam_enqueue_overflow_task(rt, task);
            }
        } else if (!local_spawn || !queue_access_valid) {
            task->enqueue_hot = 0U;
            if (llam_queue_push_bounded_locked(target,
                                               &target->processor.inject_q,
                                               LLAM_INJECT_QUEUE_CAP,
                                               task,
                                               NULL)) {
                target->metrics.inject_enqueues += 1U;
            }
        } else {
            (void)llam_norm_queue_push_owner_locked(target, &queue_access, task);
        }
        if (local_spawn) {
            wake_fanout = llam_spawn_should_wake_fanout(rt, target);
        }
        llam_trace_shard(target, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_NEW, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_NONE);
        pthread_mutex_unlock(&target->lock);
        if (!local_spawn) {
            llam_kick_shard(target);
        } else if (wake_fanout) {
            llam_wake_all_shards(rt);
        }
    }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (windows_async_host_reservation) {
        llam_windows_async_host_spawn_release(rt);
    }
#endif
    return public_handle;
}

static llam_task_t *llam_spawn_on_runtime(llam_runtime_t *rt,
                                          llam_task_fn fn,
                                          void *arg,
                                          const llam_spawn_opts_t *opts,
                                          size_t opts_size) {
    return llam_spawn_on_runtime_owned(rt, fn, arg, opts, opts_size, NULL);
}

llam_task_t *llam_runtime_spawn_ex(llam_runtime_t *runtime,
                                   llam_task_fn fn,
                                   void *arg,
                                   const llam_spawn_opts_t *opts,
                                   size_t opts_size) {
    LLAM_NATIVE_API_GUARD(NULL);

    llam_runtime_t *pinned_runtime = NULL;
    llam_task_t *task;

    if (runtime == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return NULL;
    }
    task = llam_spawn_on_runtime(pinned_runtime, fn, arg, opts, opts_size);
    llam_runtime_end_public_op(pinned_runtime);
    return task;
}

llam_task_t *llam_runtime_spawn_group_owned_ex(llam_runtime_t *runtime,
                                               llam_task_group_t *owning_group,
                                               llam_task_fn fn,
                                               void *arg,
                                               const llam_spawn_opts_t *opts,
                                               size_t opts_size) {
    llam_runtime_t *pinned_runtime = NULL;
    llam_task_t *task;

    if (runtime == NULL || owning_group == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return NULL;
    }
    task = llam_spawn_on_runtime_owned(pinned_runtime, fn, arg, opts, opts_size, owning_group);
    llam_runtime_end_public_op(pinned_runtime);
    return task;
}

/* Public callers have already checked the native domain on this same M. */
static llam_task_t *llam_spawn_current(llam_task_fn fn, void *arg,
                                      const llam_spawn_opts_t *opts, size_t opts_size) {
    llam_runtime_t *runtime = llam_runtime_current_owner();
    llam_runtime_t *pinned_runtime = NULL;
    llam_task_t *task;

    if (g_llam_tls_task != NULL || g_llam_tls_shard != NULL) {
        return llam_spawn_on_runtime(runtime, fn, arg, opts, opts_size);
    }

    /*
     * Unmanaged legacy spawn targets the default runtime.  Pin it just like the
     * explicit handle API so default shutdown/destroy cannot tear down scheduler
     * storage while this host thread is allocating and publishing a task.
     */
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return NULL;
    }
    task = llam_spawn_on_runtime(pinned_runtime, fn, arg, opts, opts_size);
    llam_runtime_end_public_op(pinned_runtime);
    return task;
}

llam_task_t *llam_spawn_ex(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts, size_t opts_size) {
    LLAM_NATIVE_API_GUARD(NULL);
    return llam_spawn_current(fn, arg, opts, opts_size);
}

llam_task_t *llam_spawn(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts) {
    LLAM_NATIVE_API_GUARD(NULL);

    return llam_spawn_current(fn, arg, opts, opts != NULL ? sizeof(*opts) : 0U);
}
