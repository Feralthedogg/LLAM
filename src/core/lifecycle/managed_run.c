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
#include "managed_catalog.h"
#include "managed_scheduler.h"

static bool llam_managed_adapter_complete(const llam_managed_scheduler_ops_t *ops) {
    return ops != NULL && ops->native_async && ops->run_begin != NULL &&
           ops->worker_start != NULL && ops->run_commit != NULL &&
           ops->run_abort != NULL && ops->run_end != NULL &&
           ops->service != NULL && ops->has_obligations != NULL &&
           ops->take_ready != NULL && ops->accept_task != NULL &&
           ops->returned != NULL && ops->worker_stop != NULL;
}

static void llam_managed_join_workers(llam_managed_run_t *run) {
    for (size_t i = 1U; i < run->executor_count; ++i) {
        llam_managed_executor_t *worker_run = &run->executors[i];
        if (worker_run->refill_service_started)
            continue;
        if (worker_run->thread_started) {
            if (pthread_join(worker_run->thread, NULL) != 0) abort();
            worker_run->thread_started = false;
        }
    }
}

static void *llam_managed_refill_service(void *argument) {
    llam_managed_executor_t *finished = argument;
    llam_managed_run_t *run = finished->run;
    for (;;) {
        pthread_mutex_lock(&run->barrier_lock);
        while (finished->thread_started && !finished->finished && !run->join_service_stop)
            pthread_cond_wait(&run->changed, &run->barrier_lock);
        bool started = finished->thread_started;
        finished->thread_started = false;
        pthread_mutex_unlock(&run->barrier_lock);
        if (!started) return NULL;
        /* Own no P while native TLS destructors run or block. */
        if (pthread_join(finished->thread, NULL) != 0) abort();
        if (run->ops != NULL && run->ops->worker_joined != NULL)
            run->ops->worker_joined(finished->worker, run->adapter_data);
        if (finished->terminal_task != NULL) {
            llam_task_t *task = finished->terminal_task;
            finished->terminal_task = NULL;
            llam_task_release_stack(task);
            llam_task_mark_reclaim_ready(task);
            unsigned expected = 0U;
            bool claimed = atomic_load_explicit(&task->detached, memory_order_acquire) != 0U &&
                atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                         memory_order_acq_rel, memory_order_acquire);
            /* Last borrowed G access. A consuming join/detach may free it as
             * soon as this native-lifetime pin drops, unless we own the claim. */
            if (!llam_task_scan_ref_release(run->runtime, task)) abort();
            if (claimed) llam_reclaim_claimed_task(run->runtime, task);
            atomic_fetch_sub_explicit(&run->terminal_obligations, 1U, memory_order_release);
        }
        if (finished->execution_generation != 0U) {
            llam_managed_release_execution(run->runtime, finished->worker,
                LLAM_MANAGED_EXECUTION_NATIVE_AFFINE, finished->execution_generation);
            finished->execution_generation = 0U;
        }
        /* The old G may still have a public handle. Its exact odd reservation
         * token cannot release a future generation's credit. Native TLS exit,
         * every bank/reader and the old binding have all ended before restart. */
        pthread_mutex_lock(&run->barrier_lock);
        bool refill = !run->join_service_stop && run->phase == LLAM_MANAGED_RUN_OPEN &&
            finished->preparation_error == 0 &&
            !atomic_load_explicit(&run->runtime->stop_requested, memory_order_acquire) &&
            !atomic_load_explicit(&run->runtime->shutdown_requested, memory_order_acquire) &&
            llam_runtime_has_live_tasks(run->runtime) &&
            atomic_load_explicit(&finished->worker->managed_reservation,
                                  memory_order_acquire) < UINT64_MAX - 1U;
        if (refill) {
            finished->finished = false;
            finished->restarting = true;
            finished->thread_started = true;
        }
        pthread_mutex_unlock(&run->barrier_lock);
        if (!refill) return NULL;
        /* Cold native creation must not hold the FIFO/barrier mutex needed by
         * unrelated managed Ms to acquire their next P. This service owns the
         * provisional handle until create returns and then the next join. */
        int error = pthread_create(&finished->thread, NULL, llam_managed_worker_main, finished);
        if (error != 0) {
            pthread_mutex_lock(&run->barrier_lock);
            finished->thread_started = false;
            finished->preparation_error = error;
            pthread_mutex_unlock(&run->barrier_lock);
            return NULL;
        }
    }
}

static void llam_managed_stop_join_service(llam_managed_run_t *run) {
    pthread_mutex_lock(&run->barrier_lock);
    run->join_service_stop = true;
    pthread_cond_broadcast(&run->changed);
    pthread_mutex_unlock(&run->barrier_lock);
    for (size_t i = 1U; i < run->executor_count; ++i) {
        llam_managed_executor_t *worker_run = &run->executors[i];
        if (worker_run->refill_service_started) {
            if (pthread_join(worker_run->refill_service, NULL) != 0) abort();
            worker_run->refill_service_started = false;
        }
    }
}

int llam_managed_runtime_run(llam_runtime_t *runtime) {
    llam_managed_run_t run = {0};
    bool begun = false;
    bool catalog_frozen = false;
    int error = 0;
    int rc;

    if (runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED ||
        runtime->active_shards == 0U || runtime->shards == NULL ||
        runtime->managed_worker_count > SIZE_MAX - runtime->active_shards) {
        errno = ENOTSUP;
        return -1;
    }
    run.runtime = runtime;
    run.ops = runtime->managed_scheduler_ops;
    run.adapter_data = runtime->managed_scheduler_data;
    run.executor_count = runtime->active_shards + runtime->managed_worker_count;
    atomic_init(&run.terminal_obligations, 0U);
    if ((run.ops != NULL && run.ops->native_async &&
         !llam_managed_adapter_complete(run.ops)) ||
        (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) != 0U &&
         !llam_managed_adapter_complete(run.ops))) {
        errno = ENOTSUP;
        return -1;
    }
    if (runtime->managed_catalog != NULL) {
        if (llam_managed_catalog_run_begin(runtime) != 0) return -1;
        catalog_frozen = true;
    } else if (!llam_managed_adapter_complete(run.ops)) {
        /* Internal expert transport fixtures may supply a fully qualified
         * adapter without a catalog. A bare A1 registry is not runnable. */
        errno = ENOTSUP;
        return -1;
    }
    if (run.executor_count > SIZE_MAX / sizeof(*run.executors)) {
        error = EOVERFLOW;
        goto out_catalog;
    }
    run.executors = calloc(run.executor_count, sizeof(*run.executors));
    if (run.executors == NULL) {
        error = ENOMEM;
        goto out_catalog;
    }
    rc = pthread_mutex_init(&run.barrier_lock, NULL);
    if (rc != 0) {
        error = rc;
        goto out_storage;
    }
    rc = pthread_cond_init(&run.changed, NULL);
    if (rc != 0) {
        error = rc;
        goto out_lock;
    }
    for (size_t i = 0U; i < run.executor_count; ++i) {
        run.executors[i].run = &run;
        run.executors[i].worker = i < runtime->active_shards
            ? &runtime->shards[i].primary_worker
            : &runtime->managed_workers[i - runtime->active_shards];
        run.executors[i].ordinal = i;
        run.executors[i].next_processor = (unsigned)(i % runtime->active_shards);
    }
    if (run.ops != NULL && run.ops->run_begin != NULL) {
        begun = true;
        if (run.ops->run_begin(runtime, run.adapter_data) != 0) {
            error = errno != 0 ? errno : EIO;
            goto abort_start;
        }
    }
    if (llam_managed_worker_prepare(&run.executors[0]) != 0) {
        error = errno != 0 ? errno : EIO;
        goto abort_start;
    }
    run.prepared = 1U;
    for (size_t i = 1U; i < run.executor_count; ++i) {
        rc = pthread_create(&run.executors[i].thread, NULL,
                            llam_managed_worker_main, &run.executors[i]);
        if (rc != 0) {
            error = rc;
            goto abort_start;
        }
        run.executors[i].thread_started = true;
    }
    pthread_mutex_lock(&run.barrier_lock);
    while (run.prepared != run.executor_count && run.start_error == 0)
        pthread_cond_wait(&run.changed, &run.barrier_lock);
    error = run.start_error;
    pthread_mutex_unlock(&run.barrier_lock);
    if (error != 0) goto abort_start;
    if (atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->stop_requested, memory_order_acquire)) {
        error = ECANCELED;
        goto abort_start;
    }
    if (run.ops != NULL && run.ops->run_commit != NULL &&
        run.ops->run_commit(runtime, run.adapter_data) != 0) {
        error = errno != 0 ? errno : EIO;
        goto abort_start;
    }
    if (catalog_frozen && (run.ops == NULL || !run.ops->native_async) &&
        llam_managed_catalog_run_commit(runtime, false, 0U) != 0) {
        error = errno != 0 ? errno : ENOTSUP;
        goto abort_start;
    }
    if (runtime->managed_policy_pools && runtime->managed_worker_count != 0U) {
        for (size_t i = 1U; i < run.executor_count; ++i) {
            if (run.executors[i].worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE) continue;
            rc = pthread_create(&run.executors[i].refill_service, NULL,
                                 llam_managed_refill_service, &run.executors[i]);
            if (rc != 0) { error = rc; goto abort_start; }
            run.executors[i].refill_service_started = true;
        }
    }
    /* Acquiring this mutex publishes every native probe result together.
     * No user G can execute while any M is still preparing or has failed. */
    pthread_mutex_lock(&run.barrier_lock);
    run.phase = LLAM_MANAGED_RUN_OPEN;
    pthread_cond_broadcast(&run.changed);
    pthread_mutex_unlock(&run.barrier_lock);
    llam_managed_scheduler_loop(&run.executors[0]);
    llam_managed_worker_close(&run.executors[0]);
    llam_managed_join_workers(&run);
    llam_managed_stop_join_service(&run);
    error = atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire);
    goto out_adapter;

abort_start:
    /* NEW Gs remain untouched. Aborted startup never enters a drain loop
     * that would wait forever for tasks whose first dispatch is forbidden. */
    if (begun && run.ops->run_abort != NULL)
        run.ops->run_abort(runtime, error, run.adapter_data);
    pthread_mutex_lock(&run.barrier_lock);
    run.phase = LLAM_MANAGED_RUN_ABORTED;
    pthread_cond_broadcast(&run.changed);
    pthread_mutex_unlock(&run.barrier_lock);
    llam_managed_join_workers(&run);
    llam_managed_stop_join_service(&run);
    llam_managed_worker_close(&run.executors[0]);
out_adapter:
    if (begun && run.ops->run_end != NULL)
        run.ops->run_end(runtime, run.adapter_data);
    for (size_t i = 0U; i < runtime->managed_worker_count; ++i)
        atomic_store_explicit(&runtime->managed_workers[i].managed_slot_used, false,
                              memory_order_release);
    pthread_cond_destroy(&run.changed);
out_lock:
    pthread_mutex_destroy(&run.barrier_lock);
out_storage:
    free(run.executors);
out_catalog:
    if (catalog_frozen) llam_managed_catalog_run_end(runtime);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}
