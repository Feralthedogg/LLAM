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

/* One private implementation; no raw magic2 entry point enters LLAM's ABI. */
#define MAGIC2_API static inline
#define MAGIC2_IMPLEMENTATION
#include "runtime_magic2.h"
#undef MAGIC2_IMPLEMENTATION
#undef MAGIC2_API

typedef struct llam_magic2_call {
    magic2_dispatch_request *request;
    int dispatch_status;
} llam_magic2_call_t;

static void *llam_magic2_blocking_dispatch(void *arg) {
    llam_magic2_call_t *call = arg;

    call->dispatch_status = magic2(call->request);
    return NULL;
}

int llam_magic2_dispatch(magic2_dispatch_request *request, int *dispatch_status) {
    llam_magic2_call_t call;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_task_t *task = g_llam_tls_task;
    void *ignored = NULL;
    int result;

    if (dispatch_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    *dispatch_status = MAGIC2_ENOTPROCESSED;
    if (request == NULL) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Unmanaged dispatch may block its native caller. Scheduler
     * code must never accidentally take that fallback, and managed code must
     * reach a parkable boundary before handing a request to the blocking pool.
     * These are read-only checks of the worker's existing publication; this
     * bridge neither grants a P ticket nor changes execution-lease authority.
     */
    if ((task == NULL && (worker != NULL || g_llam_tls_shard != NULL)) ||
        (task != NULL && (worker == NULL || g_llam_tls_shard == NULL)) ||
        (worker != NULL &&
         (atomic_load_explicit(&worker->runtime_critical_depth,
                               memory_order_acquire) != 0U ||
          atomic_load_explicit(&worker->preempt_disable_depth,
                               memory_order_acquire) != 0U)) ||
        (task != NULL &&
         atomic_load_explicit(&task->opaque_blocking_depth,
                              memory_order_acquire) != 0U)) {
        errno = EBUSY;
        return -1;
    }
    call.request = request;
    call.dispatch_status = MAGIC2_ENOTPROCESSED;
    result = llam_call_blocking_result(llam_magic2_blocking_dispatch, &call, &ignored);
    /*
     * The blocking-job completion/park protocol publishes callback writes
     * before resuming this task, including cancellation of an executing job.
     * Its drain guarantee keeps this stack record live until the final read;
     * queued cancellation leaves ENOTPROCESSED. No retry can replay writes.
     */
    *dispatch_status = call.dispatch_status;
    return result;
}

int llam_magic2_cpu_plan_create(const magic2_cpu_plan_spec *spec,
                               magic2_cpu_plan **out_plan) {
    magic2_cpu_plan *plan = NULL;
    int result;

    if (out_plan == NULL) {
        return MAGIC2_EINVAL;
    }
    if (*out_plan != NULL) {
        return MAGIC2_ESTATE;
    }
    if (spec != NULL && spec->tag == MAGIC2_TAG_CPU_PLAN &&
        spec->struct_size >= sizeof(*spec) &&
        (spec->schedule == MAGIC2_CPU_SCHEDULE_STATIC ||
         spec->schedule == MAGIC2_CPU_SCHEDULE_DYNAMIC)) {
        return MAGIC2_EUNSUPPORTED;
    }
    result = magic2_cpu_plan_create(spec, &plan);
    if (result != MAGIC2_OK) {
        return result;
    }
    if (plan->schedule != MAGIC2_CPU_SCHEDULE_CALLER) {
        magic2_cpu_plan_release(&plan);
        return MAGIC2_EUNSUPPORTED;
    }
    *out_plan = plan;
    return MAGIC2_OK;
}

int llam_magic2_cpu_plan_retain(magic2_cpu_plan *plan) {
    return magic2_cpu_plan_retain(plan);
}

void llam_magic2_cpu_plan_release(magic2_cpu_plan **plan) {
    magic2_cpu_plan_release(plan);
}

int llam_magic2_cpu_plan_get_info(const magic2_cpu_plan *plan,
                                 magic2_cpu_plan_info *info) {
    return magic2_cpu_plan_get_info(plan, info);
}

static bool llam_magic2_cpu_context_ready(void) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;

    /* Observe the scheduler's existing G/P publication; never grant a lease. */
    return shard != NULL && task != NULL && shard->runtime != NULL &&
           task->owner_runtime == shard->runtime &&
           g_llam_tls_processor == &shard->processor &&
           atomic_load_explicit(&task->opaque_blocking_depth,
                                memory_order_acquire) == 0U &&
           llam_worker_has_safe_managed_g(g_llam_tls_worker,
                                         &shard->processor, task);
}

static bool llam_magic2_cpu_cancelled(void) {
    llam_task_t *task = g_llam_tls_task;
    llam_runtime_t *runtime = task->owner_runtime;

    /* The executing G owns the runtime lifetime and retains its cancel token. */
    return atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
           atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
           llam_task_cancel_token_is_cancelled(task);
}

static int llam_magic2_cpu_domain_check(const magic2_cpu_plan *plan) {
    magic2_hardware_info hardware;

    memset(&hardware, 0, sizeof(hardware));
    magic2_internal_detect_hardware(&hardware, 0U);
    if ((plan->required_cpu_features & ~hardware.detected_features) != 0U ||
        (plan->forbidden_cpu_features & hardware.detected_features) != 0U) {
        return MAGIC2_EUNSUPPORTED;
    }
    return magic2_internal_cpu_worker_domain_check(plan);
}

static int llam_magic2_cpu_execute(magic2_cpu_plan *plan,
                                  const magic2_region *regions, size_t region_count,
                                  void *workspace, size_t workspace_bytes,
                                  magic2_cpu_run_status *status, bool *cancelled) {
    void **ports = NULL;
    unsigned char *scratch = NULL;
    size_t completed_tiles = 0U;
    size_t failed_tile = SIZE_MAX;
    int implementation_status = 0;
    int result;

    result = magic2_cpu_plan_retain(plan);
    if (result != MAGIC2_OK) {
        return result;
    }
    if (plan->schedule != MAGIC2_CPU_SCHEDULE_CALLER || plan->worker_limit != 1U) {
        result = MAGIC2_EUNSUPPORTED;
        goto release;
    }
    /*
     * Reuse the supplied implementation's span, alias, and scratch validation.
     * Progress is local to one G, including after migration. There is no
     * native executor job, cross-worker completion state, or new publication
     * protocol; the runtime's ordinary context switch owns execution.
     */
    result = magic2_internal_cpu_plan_bind(plan, regions, region_count,
                                           workspace, workspace_bytes,
                                           &ports, &scratch);
    if (result != MAGIC2_OK) {
        goto release;
    }
    result = magic2_internal_cpu_status_preflight(plan, NULL, ports,
                                                 workspace, status);
    if (result != MAGIC2_OK) {
        goto release;
    }
    result = llam_magic2_cpu_domain_check(plan);
    if (result != MAGIC2_OK) {
        goto release;
    }
    if (llam_magic2_cpu_cancelled()) {
        *cancelled = true;
        result = MAGIC2_ENOTPROCESSED;
        goto release;
    }
    for (uint32_t index = 0U; index < plan->shape.port_count; ++index) {
        const magic2_port *port = &plan->ports[index];

        if (port->present != 0U && port->initialization == MAGIC2_ZERO &&
            port->bytes != 0U) {
            memset(ports[index], 0, port->bytes);
        }
    }
    for (size_t tile = 0U; tile < plan->tile_count; ++tile) {
        if (llam_magic2_cpu_cancelled()) {
            *cancelled = true;
            break;
        }
        if (tile != 0U) {
            /* Reacquire all worker/P observations after this G may migrate. */
            llam_yield_internal();
            if (!llam_magic2_cpu_context_ready()) {
                result = MAGIC2_ESTATE;
                break;
            }
            if (llam_magic2_cpu_cancelled()) {
                *cancelled = true;
                break;
            }
            result = llam_magic2_cpu_domain_check(plan);
            if (result != MAGIC2_OK) {
                break;
            }
        }
        /* Match magic2's half-open tiles; guard multiplication before calling. */
        if (tile > SIZE_MAX / plan->grain || tile * plan->grain > plan->shape.count) {
            result = MAGIC2_ESTATE;
            break;
        }
        size_t begin = tile * plan->grain;
        size_t end = plan->shape.count - begin < plan->grain ?
                     plan->shape.count : begin + plan->grain;

        implementation_status = plan->tile(plan->environment, ports, begin, end,
                                           0U, plan->per_slot_scratch.bytes != 0U ?
                                           scratch : NULL);
        if (implementation_status != 0) {
            failed_tile = tile;
            result = MAGIC2_EIMPLEMENTATION;
            break;
        }
        completed_tiles += 1U;
    }
    /* Completion drains the only slot before observing cancellation/results. */
    *cancelled = *cancelled || llam_magic2_cpu_cancelled();
    if (result == MAGIC2_OK && *cancelled) {
        result = MAGIC2_ECANCELLED;
    }
    if (status != NULL) {
        status->state = result == MAGIC2_OK ? MAGIC2_CPU_RUN_COMPLETE :
                        result == MAGIC2_ECANCELLED ? MAGIC2_CPU_RUN_CANCELLED :
                        MAGIC2_CPU_RUN_FAILED;
        status->used_workers = plan->active_workers;
        status->completed_tiles = completed_tiles;
        status->total_tiles = plan->tile_count;
        status->failed_tile = failed_tile;
        status->runtime_status = result;
        status->implementation_status = implementation_status;
    }
release:
    magic2_cpu_plan_release(&plan);
    return result;
}

int llam_magic2_cpu_run(magic2_cpu_plan *plan,
                       const magic2_region *regions, size_t region_count,
                       void *workspace, size_t workspace_bytes,
                       magic2_cpu_run_status *status, int *run_status) {
    bool cancelled = false;

    if (run_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    *run_status = MAGIC2_ENOTPROCESSED;
    if (!llam_magic2_cpu_context_ready()) {
        errno = EBUSY;
        return -1;
    }
    if (llam_magic2_cpu_cancelled()) {
        errno = ECANCELED;
        return -1;
    }
    *run_status = llam_magic2_cpu_execute(plan, regions, region_count,
                                         workspace, workspace_bytes,
                                         status, &cancelled);
    if (cancelled) {
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

struct llam_magic2_cpu_work {
    atomic_uint refs;
    atomic_bool family_active;
    magic2_cpu_family *family;
    llam_magic2_cpu_baseline_fn baseline;
    uint64_t estimated_cost_ns;
    uint64_t minimum_cost_ns;
};

int llam_magic2_cpu_work_create(const llam_magic2_cpu_work_spec_t *spec,
                                llam_magic2_cpu_work_t **out_work,
                                magic2_storage *workspace) {
    magic2_cpu_plan *plans[8] = {0};
    magic2_cpu_family_config config = MAGIC2_CPU_FAMILY_CONFIG_INIT;
    llam_magic2_cpu_work_t *work;
    int result = MAGIC2_OK;

    if (workspace != NULL) {
        *workspace = (magic2_storage){0};
    }
    if (spec == NULL || out_work == NULL || workspace == NULL ||
        spec->plans == NULL || spec->baseline == NULL || spec->plan_count == 0U ||
        spec->plan_count > 8U ||
        (spec->kind != LLAM_MAGIC2_CPU_WORK_TRANSFORM &&
         spec->kind != LLAM_MAGIC2_CPU_WORK_ZERO)) {
        return MAGIC2_EINVAL;
    }
    if (*out_work != NULL) {
        return MAGIC2_ESTATE;
    }
    work = calloc(1U, sizeof(*work));
    if (work == NULL) {
        return MAGIC2_ENOMEM;
    }
    for (size_t index = 0U; index < spec->plan_count; ++index) {
        const magic2_cpu_plan_spec *plan_spec = &spec->plans[index];
        const magic2_shape *shape;

        if (plan_spec->tag != MAGIC2_TAG_CPU_PLAN || plan_spec->struct_size < sizeof(*plan_spec)) {
            result = MAGIC2_EABI;
            goto cleanup;
        }
        shape = &plan_spec->shape;
        if ((shape->operation_id.low == 0U && shape->operation_id.high == 0U) ||
            (shape->semantic_id.low == 0U && shape->semantic_id.high == 0U)) {
            result = MAGIC2_ECONTRACT;
            goto cleanup;
        }
        result = llam_magic2_cpu_plan_create(&spec->plans[index], &plans[index]);
        if (result != MAGIC2_OK) {
            goto cleanup;
        }
    }
    config.bucket_count = 3U;
    result = magic2_cpu_family_create(&config, plans, spec->plan_count, &work->family);
    if (result != MAGIC2_OK) {
        goto cleanup;
    }
    atomic_init(&work->refs, 1U);
    atomic_init(&work->family_active, false);
    work->baseline = spec->baseline;
    work->estimated_cost_ns = spec->estimated_cost_ns;
    work->minimum_cost_ns = spec->minimum_cost_ns != 0U ?
                            spec->minimum_cost_ns : UINT64_C(50000);
    *workspace = work->family->workspace;
    *out_work = work;
cleanup:
    for (size_t index = 0U; index < spec->plan_count; ++index) {
        magic2_cpu_plan_release(&plans[index]);
    }
    if (result != MAGIC2_OK) {
        free(work);
    }
    return result;
}

static int llam_magic2_cpu_work_retain(llam_magic2_cpu_work_t *work) {
    unsigned refs;

    if (work == NULL) {
        return MAGIC2_EINVAL;
    }
    refs = atomic_load_explicit(&work->refs, memory_order_acquire);
    for (;;) {
        if (refs == 0U || refs == UINT_MAX) {
            return refs == 0U ? MAGIC2_ECLOSING : MAGIC2_ECAPACITY;
        }
        if (atomic_compare_exchange_weak_explicit(&work->refs, &refs, refs + 1U,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return MAGIC2_OK;
        }
    }
}

void llam_magic2_cpu_work_release(llam_magic2_cpu_work_t **pointer) {
    llam_magic2_cpu_work_t *work;

    if (pointer == NULL || *pointer == NULL) {
        return;
    }
    work = *pointer;
    *pointer = NULL;
    if (atomic_fetch_sub_explicit(&work->refs, 1U, memory_order_acq_rel) == 1U) {
        /* No activation can still own the family reservation at the last ref. */
        int result = magic2_cpu_family_destroy(&work->family);

        assert(result == MAGIC2_OK);
        (void)result;
        free(work);
    }
}

static uint32_t llam_magic2_cpu_pressure_bucket(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_runtime_t *runtime = shard->runtime;

    /* Advisory snapshots only: neither queue mutation nor P authority changes. */
    if (atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime->active_io_waiters, memory_order_acquire) != 0U) {
        return 2U;
    }
    return llam_norm_queue_depth(shard) != 0U ||
           atomic_load_explicit(&shard->processor.inject_depth, memory_order_acquire) != 0U ||
           llam_runtime_overflow_depth(runtime) != 0U ? 1U : 0U;
}

int llam_magic2_cpu_auto(llam_magic2_cpu_work_t *work, void *baseline_arg,
                        const magic2_region *regions, size_t region_count,
                        void *workspace, size_t workspace_bytes,
                        magic2_cpu_run_status *status, int *run_status,
                        llam_magic2_cpu_auto_info_t *info) {
    llam_magic2_cpu_auto_info_t local = {0};
    bool inactive = false;
    uint32_t multiplier;
    uint32_t selected;
    uint64_t begin;
    uint64_t end;
    int result;
    int saved_errno;

    local.selected_index = UINT32_MAX;
    local.bucket = UINT32_MAX;
    local.slot_budget = 1U;
    if (info != NULL) {
        *info = local;
    }
    if (run_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    *run_status = MAGIC2_ENOTPROCESSED;
    if (work == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!llam_magic2_cpu_context_ready()) {
        errno = EBUSY;
        return -1;
    }
    if (llam_magic2_cpu_cancelled()) {
        errno = ECANCELED;
        return -1;
    }
    if (work->estimated_cost_ns < work->minimum_cost_ns) {
        llam_magic2_cpu_baseline_fn baseline = work->baseline;

        /*
         * Borrow the internal producer's descriptor for the small-work path.
         * The producer keeps descriptor/baseline state live through return.
         * Do not touch work after the callback, which may yield. No family
         * state or native plan participates, so retain/pressure work is avoided.
         */
        local.minimum_cost_ns = work->minimum_cost_ns;
        if (info != NULL) {
            *info = local;
        }
        return baseline(baseline_arg, status, run_status);
    }
    result = llam_magic2_cpu_work_retain(work);
    if (result != MAGIC2_OK) {
        *run_status = result;
        return 0;
    }
    local.bucket = llam_magic2_cpu_pressure_bucket();
    multiplier = 1U << local.bucket;
    local.minimum_cost_ns = work->minimum_cost_ns > UINT64_MAX / multiplier ?
                            UINT64_MAX : work->minimum_cost_ns * multiplier;
    if (work->estimated_cost_ns < local.minimum_cost_ns) {
        result = work->baseline(baseline_arg, status, run_status);
        goto done;
    }
    /*
     * This is a nonblocking admission reservation, not a scheduler mutex.
     * Losers immediately execute the baseline; no task waits on this flag.
     * Acquire/release publishes family evidence between its sole observers,
     * while an activation reference keeps both descriptor and plans alive.
     */
    if (!atomic_compare_exchange_strong_explicit(&work->family_active, &inactive, true,
                                                 memory_order_acquire,
                                                 memory_order_relaxed)) {
        local.route = LLAM_MAGIC2_CPU_AUTO_BUSY;
        local.admission_status = MAGIC2_EBUSY;
        result = work->baseline(baseline_arg, status, run_status);
        goto done;
    }
    /* Only this reservation may touch mutable family state; no native pool runs. */
    selected = magic2_internal_cpu_family_choose_locked(work->family, NULL, local.bucket, 1U);
    if (selected == UINT32_MAX) {
        atomic_store_explicit(&work->family_active, false, memory_order_release);
        local.route = LLAM_MAGIC2_CPU_AUTO_NO_CANDIDATE;
        local.admission_status = MAGIC2_EUNSUPPORTED;
        result = work->baseline(baseline_arg, status, run_status);
        goto done;
    }
    local.route = LLAM_MAGIC2_CPU_AUTO_FAMILY;
    local.selected_index = selected;
    begin = llam_now_ns();
    result = llam_magic2_cpu_run(work->family->plans[selected], regions, region_count,
                                workspace, workspace_bytes, status, run_status);
    saved_errno = errno;
    end = llam_now_ns();
    if (result == 0 && *run_status == MAGIC2_OK && begin != 0U && end >= begin) {
        local.observation_valid = 1U;
        local.elapsed_ns = end - begin;
    }
    magic2_internal_cpu_family_observe_locked(work->family, local.bucket, selected,
                                             *run_status, local.observation_valid,
                                             local.elapsed_ns);
    atomic_store_explicit(&work->family_active, false, memory_order_release);
    errno = saved_errno;
done:
    saved_errno = errno;
    if (info != NULL) {
        *info = local;
    }
    llam_magic2_cpu_work_release(&work);
    errno = saved_errno;
    return result;
}
