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
#include "managed_scheduler.h"
#if defined(LLAM_ENABLE_TEST_HOOKS)
#include "runtime_test_watchdog.h"
#endif

static void llam_managed_withdraw_processor_wait(llam_managed_executor_t *worker_run) {
    pthread_mutex_lock(&worker_run->run->barrier_lock);
    worker_run->processor_wait_ticket = 0U;
    pthread_mutex_unlock(&worker_run->run->barrier_lock);
}

int llam_managed_worker_prepare(llam_managed_executor_t *worker_run) {
    llam_managed_run_t *run = worker_run->run;
    llam_worker_t *worker = worker_run->worker;

    if (worker->signal_stack == NULL) {
        size_t size = LLAM_ALTSTACK_BYTES;
        if (size < (size_t)SIGSTKSZ) size = (size_t)SIGSTKSZ;
        void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) return -1;
        worker_run->signal_stack_mapping = mapping;
        worker_run->signal_stack_mapping_size = size;
        worker->signal_stack = mapping;
        worker->signal_stack_size = size;
    }
    atomic_store_explicit(&worker->managed_slot_closing, false, memory_order_release);
    if (llam_worker_bind_registered(worker, &worker_run->scheduler_ctx) != 0) return -1;
    worker_run->bound = true;
    llam_bind_current_thread_to_cpu(llam_processor_cpu_id(worker->associated_processor));
    llam_tune_scheduler_thread(worker->compat_shard, false);
    bool adapter_installs_signal_stack = false;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    adapter_installs_signal_stack = run->ops != NULL && run->ops->native_async &&
        worker->role != LLAM_WORKER_ROLE_LEGACY_COMPAT;
#endif
    if (!adapter_installs_signal_stack) {
        if (llam_install_thread_signal_stack(worker) == 0) {
            worker_run->signal_stack_installed = true;
        } else if (errno != EAGAIN || (run->ops != NULL && run->ops->native_async)) {
            return -1;
        }
    }
    if (run->ops != NULL && run->ops->worker_start != NULL) {
        /* stop also rolls back a partially prepared native adapter. */
        worker_run->native_started = true;
        if (worker_run->restarting) {
            if (run->ops->worker_restart == NULL) { errno = ENOTSUP; return -1; }
            if (run->ops->worker_restart(worker, &worker_run->scheduler_ctx,
                                         run->adapter_data) != 0) return -1;
        } else if (run->ops->worker_start(worker, &worker_run->scheduler_ctx,
                                          run->adapter_data) != 0) return -1;
    }
    return 0;
}

void llam_managed_worker_close(llam_managed_executor_t *worker_run) {
    llam_managed_run_t *run = worker_run->run;
    llam_worker_t *worker = worker_run->worker;

    llam_managed_withdraw_processor_wait(worker_run);
    atomic_store_explicit(&worker->managed_slot_closing, true, memory_order_seq_cst);
    if (worker_run->bound) {
        if (worker_run->native_started && run->ops->worker_stop != NULL &&
            !run->ops->worker_stop(worker, run->adapter_data)) abort();
        if (worker_run->signal_stack_installed) llam_uninstall_thread_signal_stack(worker);
        llam_channel_tls_cache_drain();
        /* worker_unbind clears raw TLS before retiring the native-domain mapping. */
        if (llam_worker_unbind_registered(worker) != 0) abort();
        worker_run->bound = false;
    }
    if (worker_run->signal_stack_mapping != NULL) {
        /* Extra M slots own no shard slice. Native operations and the binding
         * have drained before this per-run M stack can be unmapped. */
        worker->signal_stack = NULL;
        worker->signal_stack_size = 0U;
        if (munmap(worker_run->signal_stack_mapping, worker_run->signal_stack_mapping_size) != 0)
            abort();
        worker_run->signal_stack_mapping = NULL;
    }
}

static bool llam_managed_has_obligations(const llam_managed_run_t *run) {
    if (llam_runtime_has_live_tasks(run->runtime) ||
        atomic_load_explicit(&run->terminal_obligations, memory_order_acquire) != 0U ||
        (run->ops != NULL && run->ops->has_obligations != NULL &&
         run->ops->has_obligations(run->runtime, run->adapter_data))) return true;
    for (size_t i = 0U; i < run->runtime->managed_worker_count; ++i) {
        const llam_worker_t *worker = &run->runtime->managed_workers[i];
        if (worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE &&
            llam_managed_worker_reserved(worker) &&
            !atomic_load_explicit(&worker->managed_slot_used, memory_order_acquire)) return true;
    }
    return false;
}

static void llam_managed_return_processor(llam_shard_t *shard, llam_worker_t *worker,
                                          const llam_processor_owner_ticket_t *ticket,
                                          bool draining) {
    for (;;) {
        bool returned;
        pthread_mutex_lock(&shard->opaque_lock);
        if (atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire)) {
            if (shard->legacy_lease.state != LLAM_LEASE_STOPPING) {
                /* shutdown_requested precedes the locked STOPPING projection.
                 * An execute ticket is already revoked. Yield the lock so the
                 * destroy thread can finish publishing drain authority. */
                pthread_mutex_unlock(&shard->opaque_lock);
                const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
                (void)nanosleep(&delay, NULL);
                continue;
            }
            draining = true;
        }
        llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_BEFORE_REGISTERED_RETURN);
        returned = llam_execution_lease_return_locked(shard, worker, draining ? NULL : ticket);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (!returned) {
            /* A request can revoke the ordinary ticket after the flag read.
             * Release the gate so shutdown can publish STOPPING, then retry
             * through its separate drain authority; a stale ticket is never
             * treated as permission to return the ordinary grant. */
            if (!draining && atomic_load_explicit(&shard->runtime->shutdown_requested,
                                                   memory_order_acquire)) continue;
            abort();
        }
        return;
    }
}

/* Called under barrier_lock. FIFO positions are scheduling hints, so they may
 * be compacted without changing their order or touching any execution lease. */
static void llam_managed_compact_processor_wait(llam_managed_run_t *run) {
    uint64_t previous = 0U;
    uint64_t position = 0U;
    for (;;) {
        llam_managed_executor_t *oldest = NULL;
        uint64_t oldest_ticket = UINT64_MAX;
        for (size_t i = 0U; i < run->executor_count; ++i) {
            llam_managed_executor_t *candidate = &run->executors[i];
            if (candidate->processor_wait_ticket > previous &&
                candidate->processor_wait_ticket <= oldest_ticket) {
                oldest = candidate;
                oldest_ticket = candidate->processor_wait_ticket;
            }
        }
        if (oldest == NULL) break;
        previous = oldest_ticket;
        oldest->processor_wait_ticket = ++position;
    }
    run->next_processor_wait_ticket = position;
}

static llam_shard_t *llam_managed_claim_processor(llam_managed_executor_t *worker_run,
                                                llam_processor_owner_ticket_t *ticket,
                                                bool *draining) {
    llam_runtime_t *runtime = worker_run->run->runtime;
    llam_worker_t *worker = worker_run->worker;
    uint64_t incarnation = atomic_load_explicit(&worker->binding_incarnation,
                                                 memory_order_acquire);
    llam_managed_run_t *run = worker_run->run;

    unsigned first = 0U;
    unsigned count = runtime->active_shards;
    if (runtime->managed_policy_pools) {
        if (worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT) {
            first = runtime->portable_processor_count;
            count = runtime->compatibility_processor_count;
        } else count = runtime->portable_processor_count;
    }
    /* Serialize only the short claim attempt. Keeping an unsuccessful waiter's
     * position prevents a just-preempted affine M from repeatedly reclaiming a
     * free P before an older portable waiter wakes. Compatibility Ps have their
     * own lane. Neither this queue nor its ticket is a P execution grant. */
    pthread_mutex_lock(&run->barrier_lock);
    if (worker_run->processor_wait_ticket == 0U) {
        if (run->next_processor_wait_ticket == UINT64_MAX)
            llam_managed_compact_processor_wait(run);
        worker_run->processor_wait_ticket = ++run->next_processor_wait_ticket;
    }
    for (size_t i = 0U; i < run->executor_count; ++i) {
        const llam_managed_executor_t *other = &run->executors[i];
        if (other->processor_wait_ticket == 0U ||
            other->processor_wait_ticket >= worker_run->processor_wait_ticket) continue;
        if (runtime->managed_policy_pools &&
            ((other->worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT) !=
             (worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT))) continue;
        pthread_mutex_unlock(&run->barrier_lock);
        return NULL;
    }
    for (unsigned i = 0U; i < count; ++i) {
        unsigned index = first + (worker_run->next_processor + i) % count;
        llam_shard_t *shard = &runtime->shards[index];
        bool claimed;
        bool revoked = false;
        pthread_mutex_lock(&shard->opaque_lock);
        *draining = atomic_load_explicit(&runtime->shutdown_requested,
                                          memory_order_acquire);
        claimed = *draining
            ? llam_execution_lease_claim_drain_locked(shard, worker, incarnation)
            : llam_execution_lease_claim_locked(shard, worker, incarnation);
        if (claimed && !*draining) {
            pthread_mutex_lock(&shard->lock);
            if (!llam_processor_owner_ticket_snapshot_pair_locked(shard, ticket)) {
                revoked = atomic_load_explicit(&runtime->shutdown_requested,
                                                memory_order_acquire);
                if (!revoked) abort();
            }
            pthread_mutex_unlock(&shard->lock);
        }
        pthread_mutex_unlock(&shard->opaque_lock);
        if (claimed && revoked) {
            /* No G is selected in the request-before-projection gap. Rejoin
             * scheduling through a fresh STOPPING drain claim instead. */
            worker_run->processor_wait_ticket = 0U;
            pthread_mutex_unlock(&run->barrier_lock);
            llam_managed_return_processor(shard, worker, NULL, true);
            return NULL;
        }
        if (claimed) {
            worker_run->next_processor = (index - first + 1U) % count;
            worker_run->processor_wait_ticket = 0U;
            pthread_mutex_unlock(&run->barrier_lock);
            return shard;
        }
    }
    pthread_mutex_unlock(&run->barrier_lock);
    return NULL;
}

static void llam_managed_wait_dispatch_authority(llam_shard_t *shard) {
    for (;;) {
        llam_processor_queue_access_t access;
        if (llam_processor_queue_access_snapshot(&shard->processor, &access) &&
            (access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION ||
             access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN)) return;
        if (!atomic_load_explicit(&shard->runtime->shutdown_requested,
                                   memory_order_acquire)) abort();
        /* Destroy revokes ordinary execution before publishing STOPPING. This
         * reserved P/G remains local until the separate drain grant is visible. */
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
}

int llam_managed_foreign_release(llam_task_t *task, llam_managed_foreign_token_t *token) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_processor_owner_ticket_t ticket = {0};
    llam_processor_queue_access_t access;
    if (task == NULL || token == NULL || task != g_llam_tls_task || shard == NULL ||
        worker == NULL || worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE ||
        task->managed_execution_worker != worker ||
        !llam_worker_registered_for(worker, task->owner_runtime) ||
        shard->runtime != task->owner_runtime ||
        g_llam_tls_processor != &shard->processor ||
        !llam_worker_can_own_processor(worker, &shard->processor) ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor ||
        llam_worker_load_active_g(worker) != task ||
        atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_RUNNING ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) != LLAM_WORKER_EXEC_RUNTIME_UNSAFE ||
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 1U ||
        atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U ||
        g_llam_tls_scheduler_ctx == NULL ||
        !llam_processor_queue_access_snapshot(&shard->processor, &access)) {
        errno = ENOTSUP;
        return -1;
    }
    bool draining = access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN;
    if (!draining && !llam_processor_owner_ticket_snapshot(&shard->processor, &ticket)) {
        errno = EBUSY;
        return -1;
    }
    token->worker = worker;
    token->task = task;
    token->worker_run = (llam_managed_executor_t *)((unsigned char *)g_llam_tls_scheduler_ctx -
                                                    offsetof(llam_managed_executor_t, scheduler_ctx));
    if (token->worker_run->worker != worker || token->worker_run->run->runtime != task->owner_runtime)
        abort();
    uint64_t started = atomic_load_explicit(&worker->last_run_started_ns, memory_order_acquire);
    llam_task_sample_live_stack(task);
    llam_scheduler_clear_current_task(shard, started != 0U ? llam_now_ns() - started : 0U);
    llam_managed_return_processor(shard, worker, &ticket, draining);
    atomic_store_explicit(&worker->execution_state, LLAM_WORKER_EXEC_FOREIGN, memory_order_release);
    return 0;
}

void llam_managed_foreign_reacquire(llam_managed_foreign_token_t *token) {
    if (token == NULL || token->worker != g_llam_tls_worker || token->task == NULL ||
        g_llam_tls_task != NULL || g_llam_tls_processor != NULL ||
        atomic_load_explicit(&token->worker->native_domain_depth, memory_order_acquire) != 0U) abort();
    llam_processor_owner_ticket_t ticket;
    bool draining;
    llam_shard_t *shard;
    llam_runtime_t *runtime = token->worker->registry_runtime;
    /* A foreign continuation has no ordinary/READY queue node. Publish its
     * accepted work before waiting, so AUTO can preempt a CPU-only peer rather
     * than suppressing progress for lack of visible queue pressure. */
    if (atomic_fetch_add_explicit(&runtime->managed_foreign_returns_pending, 1U,
                                  memory_order_acq_rel) == UINT_MAX) abort();
    atomic_store_explicit(&token->worker->execution_state, LLAM_WORKER_EXEC_SCHEDULER,
                          memory_order_release);
    while ((shard = llam_managed_claim_processor(token->worker_run, &ticket, &draining)) == NULL) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    llam_managed_wait_dispatch_authority(shard);
    /* No ordinary runnable queue publication occurred during native work. */
    token->task->last_runnable_ns = 0U;
    (void)llam_scheduler_set_task_running(shard, token->task);
    if (atomic_fetch_sub_explicit(&runtime->managed_foreign_returns_pending, 1U,
                                  memory_order_release) == 0U) abort();
    token->task = NULL;
}

static llam_task_t *llam_managed_take_task(llam_managed_executor_t *worker_run,
                                          llam_shard_t *shard) {
    llam_managed_run_t *run = worker_run->run;
    llam_task_t *task = NULL;
    bool pressure = llam_runtime_pressure_signal(run->runtime);

    llam_allocator_quiescent(shard);
    llam_drain_inject_queue(shard);
    llam_fire_expired_timers(shard);
    if (run->ops != NULL && run->ops->take_ready != NULL)
        task = run->ops->take_ready(worker_run->worker, &shard->processor, false,
                                    run->adapter_data);
    if (task != NULL) return task;
    task = llam_take_local_task_with_pressure(shard, pressure);
    if (task == NULL && pressure) task = llam_take_overflow_task(run->runtime);
    if (task == NULL) task = llam_try_steal_task(run->runtime, shard);
    if (task == NULL) task = llam_take_overflow_task(run->runtime);
    if (task != NULL) {
        if (run->ops != NULL && run->ops->accept_task != NULL &&
            !run->ops->accept_task(worker_run->worker, &shard->processor, task,
                                   run->adapter_data)) return NULL;
        return task;
    }
    if (run->ops != NULL && run->ops->take_ready != NULL)
        task = run->ops->take_ready(worker_run->worker, &shard->processor, true,
                                    run->adapter_data);
    return task;
}

static bool llam_managed_reserve_task(llam_shard_t *shard, llam_worker_t *worker,
                                       llam_task_t *task) {
    llam_worker_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&task->managed_dispatch_owner,
                                                 &expected, worker,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) return true;
    /* A native READY source can race ordinary admission. Preserve its current
     * shard and execution reservation; no task state or context is modified.
     * The source return releases the reservation, after which inject is safe. */
    unsigned origin = atomic_load_explicit(&task->last_shard, memory_order_relaxed);
    llam_shard_t *target = origin < shard->runtime->active_shards
        ? &shard->runtime->shards[origin] : shard;
    pthread_mutex_lock(&target->lock);
    if (llam_queue_push_bounded_locked(target, &target->processor.inject_q,
                                       LLAM_INJECT_QUEUE_CAP, task, NULL))
        target->metrics.inject_enqueues += 1U;
    pthread_mutex_unlock(&target->lock);
    llam_kick_shard(target);
    return false;
}

static void llam_managed_release_task(llam_worker_t *worker, llam_task_t *task) {
    llam_worker_t *expected = worker;
    /* Publishes completed context saving and every return-callback write to
     * the next queue consumer. active-G clearing alone does not do that. */
    if (!atomic_compare_exchange_strong_explicit(&task->managed_dispatch_owner,
                                                  &expected, NULL,
                                                  memory_order_release,
                                                  memory_order_relaxed)) abort();
}

#if LLAM_PLATFORM_DARWIN
/* Apple Clang otherwise folds the idle scan into every dispatch iteration. */
__attribute__((noinline, cold))
#endif
static bool llam_managed_defer_processor_claim(llam_managed_executor_t *worker_run) {
    const llam_managed_run_t *run = worker_run->run;
    const llam_runtime_t *runtime = run->runtime;
    llam_worker_t *worker = worker_run->worker;
    if (!worker_run->processor_claim_waiting || run->ops == NULL || !run->ops->native_async ||
        run->ops->worker_needs_processor == NULL ||
        worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE ||
        worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT ||
        atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) return false;

    if (run->ops->worker_needs_processor(worker, run->adapter_data)) return false;
    unsigned online = 0U, occupied = 0U;
    uint64_t now_ns = llam_now_ns();
    unsigned count = runtime->managed_policy_pools ? runtime->portable_processor_count
                                                  : runtime->active_shards;
    for (unsigned i = 0U; i < count; ++i)
        if (llam_processor_online(&runtime->shards[i].processor)) ++online;
    if (online == 0U) return false;
    for (size_t i = 0U; i < run->executor_count; ++i) {
        const llam_worker_t *other = run->executors[i].worker;
        /* Affine/compatibility Ms cannot consume ordinary portable queues.
         * Their P occupancy must not withdraw the only portable waiter. */
        if (other->role == LLAM_WORKER_ROLE_NATIVE_AFFINE ||
            other->role == LLAM_WORKER_ROLE_LEGACY_COMPAT) continue;
        const llam_processor_t *processor = atomic_load_explicit(&other->current_p, memory_order_acquire);
        if (processor != NULL && processor->id < count && llam_processor_online(processor)) {
            ++occupied;
        } else if (other != worker) {
            uint64_t last = atomic_load_explicit(&other->last_safepoint_ns, memory_order_acquire);
            /* Keep a recently progressing M through its brief P return gap.
             * This is the existing 100us retry interval, not a new quantum. */
            if (last != 0U && now_ns >= last && now_ns - last < UINT64_C(100000)) ++occupied;
        }
    }
    /* These independently sampled mirrors can only postpone a claim. They
     * never grant P or dereference a G; run lifetime retains all M/P storage.
     * An idle portable M needs no FIFO position while Ps have active or
     * recently progressing Ms. An idle hint expires within one retry interval.
     * Origin READY and dedicated affine Ms retain theirs, so a cooperative
     * owner cannot starve a continuation that must return on another M. */
    return occupied >= online;
}

static LLAM_NOINLINE bool llam_managed_compatibility_idle(llam_managed_executor_t *worker_run) {
    const llam_managed_run_t *run = worker_run->run;
    const llam_runtime_t *runtime = run->runtime;
    if (!runtime->managed_policy_pools ||
        atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
        llam_managed_catalog_has_legacy_references(runtime) ||
        (run->ops != NULL && run->ops->worker_needs_processor != NULL &&
         run->ops->worker_needs_processor(worker_run->worker, run->adapter_data))) return false;
    for (unsigned i = 0U; i < runtime->compatibility_processor_count; ++i) {
        const llam_shard_t *shard = &runtime->shards[runtime->portable_processor_count + i];
        if (atomic_load_explicit(&shard->processor.allocator.remote_free_pending, memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->processor.timer_callbacks_active, memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->inflight_io_waiters, memory_order_acquire) != 0U) return false;
    }
    return true;
}

void llam_managed_scheduler_loop(llam_managed_executor_t *worker_run) {
    llam_managed_run_t *run = worker_run->run;
    llam_runtime_t *runtime = run->runtime;

    for (;;) {
        llam_processor_owner_ticket_t ticket = {0};
        llam_shard_t *shard;
        llam_task_t *task;
        bool draining = false;
        uint64_t started_ns;

        if (run->ops != NULL && run->ops->service != NULL)
            run->ops->service(worker_run->worker, run->adapter_data);
        if (worker_run->worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE &&
            (worker_run->terminal_task != NULL ||
             (run->ops != NULL && run->ops->worker_can_exit != NULL &&
              run->ops->worker_can_exit(worker_run->worker, run->adapter_data)))) break;
        if (!llam_managed_has_obligations(run)) {
            if (worker_run->worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE) {
                atomic_store_explicit(&worker_run->worker->managed_slot_closing, true, memory_order_seq_cst);
                if ((atomic_load_explicit(&worker_run->worker->managed_reservation, memory_order_seq_cst) & 1U) != 0U &&
                    !atomic_load_explicit(&worker_run->worker->managed_slot_used, memory_order_acquire)) {
                    atomic_store_explicit(&worker_run->worker->managed_slot_closing, false, memory_order_release);
                    continue;
                }
            }
            llam_request_stop(runtime);
            break;
        }
        if (worker_run->worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT &&
            llam_managed_compatibility_idle(worker_run)) {
            /* No admitted legacy G can use this P. Keep native service above
             * active and recheck within the existing 100us claim retry period.
             * Late admission publishes its reference before queueing work. */
            if (worker_run->processor_claim_waiting) {
                llam_managed_withdraw_processor_wait(worker_run);
                worker_run->processor_claim_waiting = false;
            }
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire))
            llam_runtime_cancel_parked_waiters(runtime);
        if (worker_run->worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE &&
            !llam_managed_worker_reserved(worker_run->worker)) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (runtime->experimental_dynamic_shards != 0U &&
            worker_run->worker->role == LLAM_WORKER_ROLE_PRIMARY &&
            !llam_processor_online(worker_run->worker->associated_processor) &&
            !atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) &&
            !atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
            (run->ops == NULL || run->ops->worker_needs_processor == NULL ||
             !run->ops->worker_needs_processor(worker_run->worker, run->adapter_data))) {
            /* A soft-parked M stays bound: cooperative C frames may retain
             * native TLS addresses. Service runs above without P; origin
             * READY work bypasses this delay and can lease another online P. */
            llam_managed_withdraw_processor_wait(worker_run);
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (worker_run->processor_claim_waiting && llam_managed_defer_processor_claim(worker_run)) {
            llam_managed_withdraw_processor_wait(worker_run);
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        shard = llam_managed_claim_processor(worker_run, &ticket, &draining);
        worker_run->processor_claim_waiting = shard == NULL;
        if (shard == NULL) {
            /* More Ms than Ps is normal. This bounded scheduler wait neither
             * holds P nor prevents its native service callbacks from running. */
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        llam_managed_wait_dispatch_authority(shard);
        task = llam_managed_take_task(worker_run, shard);
        if (task != NULL && !llam_managed_reserve_task(shard, worker_run->worker, task))
            task = NULL;
        if (task == NULL) {
            llam_managed_return_processor(shard, worker_run->worker, &ticket, draining);
            /* READY can be affine to a different M even when this P's ordinary
             * queues are empty. Never sleep retaining the P that M needs. */
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_watchdog_test_scheduler_point(shard, task, LLAM_WATCHDOG_TEST_DEQUEUE);
#endif
        llam_managed_wait_dispatch_authority(shard);
        if (worker_run->worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE) {
            if (task->managed_execution_generation == 0U ||
                task->managed_execution_generation != atomic_load_explicit(
                    &worker_run->worker->managed_reservation, memory_order_acquire)) abort();
            worker_run->execution_generation = task->managed_execution_generation;
            atomic_store_explicit(&worker_run->worker->managed_slot_used, true, memory_order_release);
        }
        started_ns = llam_scheduler_set_task_running(shard, task);
        shard->metrics.ctx_switches += 1U;
        llam_switch_scheduler_to_task(&worker_run->scheduler_ctx, task);
        task = g_llam_tls_task != NULL ? g_llam_tls_task : task;
        shard = g_llam_tls_shard;
        if (shard == NULL || g_llam_tls_processor != &shard->processor) abort();
        if (!atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
            !llam_processor_owner_ticket_snapshot(&shard->processor, &ticket)) {
            if (!atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) abort();
        }
        llam_scheduler_clear_current_task(shard,
            started_ns != 0U ? llam_now_ns() - task->last_started_ns : 0U);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_watchdog_test_scheduler_point(shard, task, LLAM_WATCHDOG_TEST_RETURN);
#endif
        bool dead = atomic_load_explicit(&task->state, memory_order_acquire) ==
                    LLAM_TASK_STATE_DEAD;
        if (dead) llam_managed_release_task(worker_run->worker, task);
        if (run->ops != NULL && run->ops->returned != NULL) {
            run->ops->returned(worker_run->worker, &shard->processor, task,
                               run->adapter_data);
        } else if (dead && worker_run->worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE) {
            if (!llam_task_scan_ref_try_acquire(runtime, task)) abort();
            worker_run->terminal_task = task;
            atomic_fetch_add_explicit(&run->terminal_obligations, 1U, memory_order_release);
        } else if (dead) {
            if (!llam_task_scan_ref_try_acquire(runtime, task)) abort();
            llam_task_release_stack(task);
            llam_task_mark_reclaim_ready(task);
            unsigned expected = 0U;
            bool claimed = atomic_load_explicit(&task->detached, memory_order_acquire) != 0U &&
                atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                         memory_order_acq_rel, memory_order_acquire);
            if (!llam_task_scan_ref_release(runtime, task)) abort();
            if (claimed) llam_reclaim_claimed_task(runtime, task);
        }
        if (!dead) llam_managed_release_task(worker_run->worker, task);
        /* No task access after DEAD retirement. Every restoration
         * returns through a new P claim and a new lease epoch. */
        llam_managed_return_processor(shard, worker_run->worker, &ticket, draining);
    }
}

void *llam_managed_worker_main(void *argument) {
    llam_managed_executor_t *worker_run = argument;
    llam_managed_run_t *run = worker_run->run;
    bool restarting = worker_run->restarting;
    int error = llam_managed_worker_prepare(worker_run) == 0 ? 0 : (errno != 0 ? errno : EIO);
    bool dispatch;

    pthread_mutex_lock(&run->barrier_lock);
    worker_run->preparation_error = error;
    if (!restarting) {
        ++run->prepared;
        if (error != 0 && run->start_error == 0) run->start_error = error;
    }
    pthread_cond_broadcast(&run->changed);
    while (run->phase == LLAM_MANAGED_RUN_PREPARING)
        pthread_cond_wait(&run->changed, &run->barrier_lock);
    dispatch = error == 0 && run->phase == LLAM_MANAGED_RUN_OPEN;
    if (restarting) {
        dispatch = dispatch &&
            !atomic_load_explicit(&run->runtime->stop_requested, memory_order_acquire) &&
            !atomic_load_explicit(&run->runtime->shutdown_requested, memory_order_acquire);
        if (dispatch) {
            worker_run->restarting = false;
            atomic_store_explicit(&worker_run->worker->managed_slot_used, false, memory_order_release);
            atomic_store_explicit(&worker_run->worker->managed_slot_closing, false, memory_order_seq_cst);
        }
    }
    pthread_mutex_unlock(&run->barrier_lock);
    if (dispatch) llam_managed_scheduler_loop(worker_run);
    llam_managed_worker_close(worker_run);
    pthread_mutex_lock(&run->barrier_lock);
    worker_run->finished = true;
    pthread_cond_broadcast(&run->changed);
    pthread_mutex_unlock(&run->barrier_lock);
    return NULL;
}
