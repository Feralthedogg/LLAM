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

#ifndef LLAM_MANAGED_SCHEDULER_H
#define LLAM_MANAGED_SCHEDULER_H

#include "runtime_state.h"

static inline bool llam_managed_foreign_return_pending(const llam_runtime_t *runtime) {
    return atomic_load_explicit(&runtime->managed_foreign_returns_pending,
                                 memory_order_acquire) != 0U;
}

/* Odd values are exact admission reservations; release advances to the next
 * even value. A delayed old-G release cannot clear a later reservation. */
static inline bool llam_managed_worker_reserved(const llam_worker_t *worker) {
    return (atomic_load_explicit(&worker->managed_reservation, memory_order_acquire) & 1U) != 0U;
}

/* Queue storage retains G while this predicate is evaluated. A dedicated or
 * compatibility M must never borrow another class's task, even before entry. */
static inline bool llam_managed_task_matches_worker(const llam_task_t *task,
                                                   const llam_worker_t *worker) {
    const llam_runtime_t *runtime = task->owner_runtime;
    if (runtime == NULL || !runtime->managed_policy_pools) return true;
    if (task->managed_execution_worker != NULL)
        return task->managed_execution_worker == worker;
    return worker != NULL && worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE &&
           worker->role != LLAM_WORKER_ROLE_LEGACY_COMPAT;
}

/* Installed before run and immutable until all bound Ms have joined. These
 * callbacks are internal native-service boundaries, never user callbacks.
 * A NULL table selects ordinary cooperative execution only. */
typedef struct llam_managed_scheduler_ops {
    bool native_async;
    int (*run_begin)(llam_runtime_t *runtime, void *data);
    int (*worker_start)(llam_worker_t *worker, llam_ctx_t *scheduler_ctx, void *data);
    int (*worker_restart)(llam_worker_t *worker, llam_ctx_t *scheduler_ctx, void *data);
    int (*run_commit)(llam_runtime_t *runtime, void *data);
    void (*run_abort)(llam_runtime_t *runtime, int error, void *data);
    void (*run_end)(llam_runtime_t *runtime, void *data);
    /* Service retirement/native obligations on M without holding a P lease. */
    void (*service)(llam_worker_t *worker, void *data);
    bool (*has_obligations)(llam_runtime_t *runtime, void *data);
    /* Called only with a fresh P lease. READY may remain affine to this M. */
    llam_task_t *(*take_ready)(llam_worker_t *worker, llam_processor_t *processor,
                               bool fallback, void *data);
    /* Ordinary dequeue reservation. False transfers G to adapter routing;
     * true guarantees that this M may safely restore its cooperative context. */
    bool (*accept_task)(llam_worker_t *worker, llam_processor_t *processor,
                        llam_task_t *task, void *data);
    /* Owns the final G access and DEAD retirement after active-G is cleared.
     * P remains held until return; asynchronous requeue must follow this seam. */
    void (*returned)(llam_worker_t *worker, llam_processor_t *processor,
                     llam_task_t *task, void *data);
    /* Close and drain on the original native M before stack/binding removal. */
    bool (*worker_stop)(llam_worker_t *worker, void *data);
    bool (*worker_can_exit)(llam_worker_t *worker, void *data);
    void (*worker_joined)(llam_worker_t *worker, void *data);
    /* Origin-bound runnable work must retain its P wait position. False also
     * lets an idle portable M defer contending with already occupied Ps. */
    bool (*worker_needs_processor)(llam_worker_t *worker, void *data);
} llam_managed_scheduler_ops_t;

typedef enum llam_managed_run_phase {
    LLAM_MANAGED_RUN_PREPARING,
    LLAM_MANAGED_RUN_OPEN,
    LLAM_MANAGED_RUN_ABORTED
} llam_managed_run_phase_t;

struct llam_managed_run;
typedef struct llam_managed_executor {
    struct llam_managed_run *run;
    llam_worker_t *worker;
    /* A P can move between Ms, but native return always uses this M's context. */
    _Alignas(16) llam_ctx_t scheduler_ctx;
    pthread_t thread;
    void *signal_stack_mapping;
    size_t signal_stack_mapping_size;
    size_t ordinal;
    unsigned next_processor;
    bool thread_started;
    bool bound;
    bool signal_stack_installed;
    bool native_started;
    bool finished;
    bool restarting;
    int preparation_error;
    pthread_t refill_service;
    bool refill_service_started;
    uint64_t execution_generation;
    /* Advisory FIFO position, protected by run->barrier_lock. A position
     * grants no execution authority; the canonical P lease must still commit. */
    uint64_t processor_wait_ticket;
    bool processor_claim_waiting; /* M-local hint after an unsuccessful claim. */
    llam_task_t *terminal_task;
} llam_managed_executor_t;

typedef struct llam_managed_run {
    llam_runtime_t *runtime;
    const llam_managed_scheduler_ops_t *ops;
    void *adapter_data;
    llam_managed_executor_t *executors;
    size_t executor_count;
    pthread_mutex_t barrier_lock;
    pthread_cond_t changed;
    size_t prepared;
    llam_managed_run_phase_t phase;
    int start_error;
    bool join_service_stop;
    atomic_uint terminal_obligations;
    uint64_t next_processor_wait_ticket;
} llam_managed_run_t;

/* Owns the already-claimed exec_started token; caller clears it after return. */
int llam_managed_runtime_run(llam_runtime_t *runtime);
void *llam_managed_worker_main(void *argument);
int llam_managed_worker_prepare(llam_managed_executor_t *worker_run);
void llam_managed_worker_close(llam_managed_executor_t *worker_run);
void llam_managed_scheduler_loop(llam_managed_executor_t *worker_run);

/* Ordinary dispatch bookkeeping shared with the compatibility scheduler. */
uint64_t llam_scheduler_set_task_running(llam_shard_t *shard, llam_task_t *task);
void llam_scheduler_clear_current_task(llam_shard_t *shard, uint64_t run_ns);

int llam_managed_reserve_execution(llam_runtime_t *runtime, uint32_t execution_class,
                                    llam_worker_t **out, uint64_t *generation_out);
void llam_managed_release_execution(llam_runtime_t *runtime, llam_worker_t *worker,
                                     uint32_t execution_class, uint64_t generation);
uint32_t llam_managed_affine_available(const llam_runtime_t *runtime);
bool llam_managed_mixed_available(const llam_runtime_t *runtime);
typedef struct llam_managed_foreign_token {
    llam_worker_t *worker;
    llam_task_t *task;
    llam_managed_executor_t *worker_run;
} llam_managed_foreign_token_t;
int llam_managed_foreign_release(llam_task_t *task, llam_managed_foreign_token_t *token);
void llam_managed_foreign_reacquire(llam_managed_foreign_token_t *token);
#endif /* LLAM_MANAGED_SCHEDULER_H */
