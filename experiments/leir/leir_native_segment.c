// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_native_segment.c
 * @brief Bind and execute backend-native LEIR effect segments.
 *
 * @details
 * The instance activity state serializes bind, run, and destroy. A successful
 * Linux bind duplicates descriptor authority and, in fixed mode, owns scratch
 * buffers while caller slot pointers remain borrowed. Batch run claims every
 * instance before publishing any segment and rolls back partial claims on
 * failure. It releases requests and returns instances to @c IDLE only after
 * each segment is @c RETIRED or was never submitted; destroy first detaches
 * registered-resource leases, then closes and frees owned resources before
 * publishing @c DESTROYED.
 */

#include "leir_native_segment_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    LEIR_NATIVE_INSTANCE_IDLE = 0U,
    LEIR_NATIVE_INSTANCE_RUNNING = 1U,
    LEIR_NATIVE_INSTANCE_BINDING = 2U,
    LEIR_NATIVE_INSTANCE_DESTROYED = 3U,
};

static leir_native_test_hook_fn
    leir_native_bind_before_claim_hook;
static void *leir_native_bind_before_claim_context;

void leir_native_test_set_bind_before_claim_hook(
    leir_native_test_hook_fn hook,
    void *context) {
    leir_native_bind_before_claim_context = context;
    leir_native_bind_before_claim_hook = hook;
}

static void run_bind_before_claim_hook(void) {
    leir_native_test_hook_fn hook =
        leir_native_bind_before_claim_hook;

    if (hook != NULL) {
        hook(leir_native_bind_before_claim_context);
    }
}

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static bool operation_count_is_supported(unsigned count) {
    return count == 1U || count == 2U ||
           count == 4U || count == 8U;
}

static bool mode_is_supported(leir_native_mode_t mode) {
    return mode == LEIR_NATIVE_MODE_LINK ||
           mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
}

static bool plans_are_equal(
    const leir_native_plan_t *left,
    const leir_native_plan_t *right) {
    unsigned i;

    if (left->step_count != right->step_count ||
        left->return_node != right->return_node ||
        left->result_slot != right->result_slot) {
        return false;
    }
    for (i = 0U; i < left->step_count; i += 1U) {
        if (left->steps[i].kind != right->steps[i].kind ||
            left->steps[i].fd_slot != right->steps[i].fd_slot ||
            left->steps[i].buffer_slot !=
                right->steps[i].buffer_slot ||
            left->steps[i].length_slot !=
                right->steps[i].length_slot ||
            left->steps[i].result_slot !=
                right->steps[i].result_slot) {
            return false;
        }
    }
    return true;
}

static bool fd_is_valid(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    return !LLAM_FD_IS_INVALID(fd);
#else
    return fd >= 0;
#endif
}

static int validate_slot_values(
    const leir_phase0_program_t *program,
    const leir_phase0_value_t *values) {
    unsigned i;

    for (i = 0U; i < program->slot_count; i += 1U) {
        switch (program->slot_kinds[i]) {
            case LEIR_PHASE0_SLOT_FD:
                if (!fd_is_valid(values[i].fd)) {
                    return fail_with_errno(EINVAL);
                }
                break;
            case LEIR_PHASE0_SLOT_MUT_BUFFER:
            case LEIR_PHASE0_SLOT_CONST_BUFFER:
                if (values[i].buffer.data == NULL &&
                    values[i].buffer.size != 0U) {
                    return fail_with_errno(EINVAL);
                }
                break;
            case LEIR_PHASE0_SLOT_U64:
            case LEIR_PHASE0_SLOT_I64:
                break;
            default:
                return fail_with_errno(EINVAL);
        }
    }
    return 0;
}

int leir_native_step_length(
    const leir_phase0_program_t *program,
    const leir_native_step_t *step,
    const leir_phase0_value_t *values,
    uint32_t *length_out) {
    uint64_t requested;

    if (program->slot_kinds[step->length_slot] ==
        LEIR_PHASE0_SLOT_I64) {
        int64_t signed_length = values[step->length_slot].i64;

        if (signed_length < 0) {
            return fail_with_errno(EINVAL);
        }
        requested = (uint64_t)signed_length;
    } else {
        requested = values[step->length_slot].u64;
    }

    if (requested == 0U) {
        return fail_with_errno(EINVAL);
    }
    if (requested > UINT_MAX) {
        return fail_with_errno(EOVERFLOW);
    }
    if (values[step->buffer_slot].buffer.data == NULL ||
        requested >
            values[step->buffer_slot].buffer.size) {
        return fail_with_errno(EINVAL);
    }
    *length_out = (uint32_t)requested;
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX
static bool embedded_request_is_reusable(
    const llam_task_t *task) {
    const llam_io_req_t *req;

    if (task == NULL ||
        llam_task_active_io_req_load(task) != NULL) {
        return false;
    }
    req = &task->embedded_io_req;
    return atomic_load_explicit(
               &req->lifetime_refs,
               memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->cancel_queued,
               memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->cancel_submitted,
               memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->free_after_cancel,
               memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->backend_event_refs,
               memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->release_after_event,
               memory_order_acquire) == 0U;
}

static llam_io_req_t *acquire_embedded_request(
    llam_task_t *task) {
    while (!embedded_request_is_reusable(task)) {
        llam_yield();
    }
    return llam_api_io_req_acquire(g_llam_tls_shard);
}

static void prepare_request(
    llam_io_req_t *req,
    const llam_linux_native_op_t *last) {
    bool receive =
        last->kind == LLAM_LINUX_NATIVE_OP_RECV;

    req->kind =
        receive ? LLAM_IO_KIND_READ : LLAM_IO_KIND_WRITE;
    req->fd = last->fd;
    req->buf = last->buffer;
    req->count = last->length;
    req->use_recv_op = receive;
    req->recv_flags = 0;
    req->use_send_op = !receive;
    req->completion_sink = NULL;
    req->completion_sink_context = NULL;
}

static void publish_results(
    leir_native_instance_t *instance,
    bool success) {
    const llam_linux_native_segment_t *segment =
        &instance->linux_state->segment;
    unsigned successful_operations;
    unsigned i;

    if (success) {
        successful_operations = instance->plan.step_count;
    } else if (
        segment->first_error_index <
        instance->plan.step_count) {
        successful_operations =
            segment->first_error_index;
    } else {
        successful_operations = 0U;
    }

    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        const llam_linux_native_op_t *op =
            &segment->ops[i];

        instance->slots[op->result_slot].i64 =
            i < successful_operations
                ? (int64_t)op->length
                : -1;
    }
}

static void copy_linux_metrics(
    const llam_linux_native_segment_t *segment,
    leir_native_metrics_t *metrics) {
    metrics->activations = segment->activations;
    metrics->logical_operations =
        segment->logical_operations;
    metrics->queue_publications =
        segment->queue_publications;
    metrics->prepared_sqes = segment->prepared_sqes;
    metrics->observed_cqes = segment->observed_cqes;
    metrics->suppressed_success_cqes =
        segment->suppressed_success_cqes;
    metrics->task_parks = segment->task_parks;
    metrics->terminal_wakes = segment->terminal_wakes;
    metrics->hot_allocations = segment->hot_allocations;
    metrics->first_error_operation =
        segment->first_error_index <= UINT16_MAX
            ? (uint16_t)segment->first_error_index
            : UINT16_MAX;
}
#endif

size_t leir_native_instance_size(void) {
    return sizeof(leir_native_instance_t);
}

size_t leir_native_test_linux_core_size(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    return sizeof(leir_native_linux_state_t);
#else
    return 0U;
#endif
}

size_t leir_native_test_linux_fixed_size(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    return sizeof(leir_native_linux_fixed_state_t);
#else
    return 0U;
#endif
}

const void *leir_native_test_linux_state_address(
    const leir_native_instance_t *instance) {
#if LLAM_RUNTIME_BACKEND_LINUX
    return instance != NULL ? instance->linux_state : NULL;
#else
    (void)instance;
    return NULL;
#endif
}

const void *leir_native_test_linux_fixed_address(
    const leir_native_instance_t *instance) {
#if LLAM_RUNTIME_BACKEND_LINUX
    return instance != NULL && instance->linux_state != NULL
        ? instance->linux_state->fixed
        : NULL;
#else
    (void)instance;
    return NULL;
#endif
}

int leir_native_instance_init(
    void *storage,
    size_t storage_size,
    const leir_phase0_program_t *program,
    const leir_native_plan_t *plan,
    leir_native_mode_t mode) {
    leir_native_plan_t compiled;
    leir_native_instance_t *instance;

    if (storage == NULL ||
        program == NULL ||
        plan == NULL ||
        storage_size < sizeof(leir_native_instance_t) ||
        (uintptr_t)storage %
                _Alignof(leir_native_instance_t) !=
            0U ||
        !mode_is_supported(mode) ||
        leir_native_plan_compile(program, &compiled) != 0 ||
        !operation_count_is_supported(compiled.step_count) ||
        !plans_are_equal(plan, &compiled)) {
        return fail_with_errno(EINVAL);
    }

    memset(storage, 0, sizeof(leir_native_instance_t));
    instance = storage;
    instance->program = program;
    instance->plan = compiled;
    instance->mode = mode;
    atomic_init(
        &instance->activity,
        LEIR_NATIVE_INSTANCE_IDLE);
    atomic_init(&instance->bound, 0U);
    return 0;
}

int leir_native_instance_bind(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values,
    size_t value_count) {
    const leir_phase0_program_t *program;
    unsigned expected = LEIR_NATIVE_INSTANCE_IDLE;
    int result = -1;
    int saved_errno = EINVAL;
    unsigned i;
#if LLAM_RUNTIME_BACKEND_LINUX
    leir_native_linux_state_t *candidate = NULL;
    leir_native_linux_state_t *prior;
#endif

    if (instance == NULL || values == NULL) {
        return fail_with_errno(EINVAL);
    }
    run_bind_before_claim_hook();
    if (!atomic_compare_exchange_strong_explicit(
            &instance->activity,
            &expected,
            LEIR_NATIVE_INSTANCE_BINDING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(
            expected == LEIR_NATIVE_INSTANCE_DESTROYED
                ? EINVAL
                : EBUSY);
    }
    program = instance->program;
    if (program == NULL ||
        value_count != program->slot_count) {
        saved_errno = EINVAL;
        goto done;
    }

    if (validate_slot_values(program, values) != 0) {
        saved_errno = errno;
        goto done;
    }
    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        uint32_t ignored_length;

        if (leir_native_step_length(
                program,
                &instance->plan.steps[i],
                values,
                &ignored_length) != 0) {
            saved_errno = errno;
            goto done;
        }
    }
#if LLAM_RUNTIME_BACKEND_LINUX
    if (leir_native_linux_state_build(
            instance, values, &candidate) != 0) {
        saved_errno = errno;
        goto done;
    }
    prior = instance->linux_state;
    if (leir_native_linux_state_detach_fixed(prior) != 0) {
        saved_errno = errno;
        leir_native_linux_state_free(candidate);
        candidate = NULL;
        goto done;
    }
    /*
     * The instance is exclusively BINDING here. Publish the fully validated
     * candidate in one pointer store, then retire the detached prior state.
     * No fallible operation follows the commit.
     */
    instance->linux_state = candidate;
    candidate = NULL;
    leir_native_linux_state_free(prior);
#endif
    memcpy(
        instance->slots,
        values,
        value_count * sizeof(values[0]));
    atomic_store_explicit(
        &instance->bound, 1U, memory_order_release);
    result = 0;
    saved_errno = 0;

done:
    atomic_store_explicit(
        &instance->activity,
        LEIR_NATIVE_INSTANCE_IDLE,
        memory_order_release);
    errno = saved_errno;
    return result;
}

int leir_native_instance_destroy(
    leir_native_instance_t *instance) {
    unsigned expected = LEIR_NATIVE_INSTANCE_IDLE;
#if LLAM_RUNTIME_BACKEND_LINUX
    unsigned was_bound;
#endif

    if (instance == NULL) {
        return fail_with_errno(EINVAL);
    }
    if (!atomic_compare_exchange_strong_explicit(
            &instance->activity,
            &expected,
            LEIR_NATIVE_INSTANCE_BINDING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(
            expected == LEIR_NATIVE_INSTANCE_DESTROYED
                ? EINVAL
                : EBUSY);
    }
    if (instance->program == NULL) {
        atomic_store_explicit(
            &instance->activity,
            LEIR_NATIVE_INSTANCE_DESTROYED,
            memory_order_release);
        return fail_with_errno(EINVAL);
    }

#if LLAM_RUNTIME_BACKEND_LINUX
    was_bound = atomic_exchange_explicit(
        &instance->bound, 0U, memory_order_acq_rel);
    if (leir_native_linux_state_detach_fixed(
            instance->linux_state) != 0) {
        int saved_errno = errno;

        atomic_store_explicit(
            &instance->bound,
            was_bound,
            memory_order_release);
        atomic_store_explicit(
            &instance->activity,
            LEIR_NATIVE_INSTANCE_IDLE,
            memory_order_release);
        return fail_with_errno(saved_errno);
    }
    {
        leir_native_linux_state_t *state =
            instance->linux_state;

        instance->linux_state = NULL;
        leir_native_linux_state_free(state);
    }
#else
    atomic_store_explicit(
        &instance->bound, 0U, memory_order_release);
#endif
    memset(&instance->plan, 0, sizeof(instance->plan));
    memset(instance->slots, 0, sizeof(instance->slots));
    instance->program = NULL;
    atomic_store_explicit(
        &instance->activity,
        LEIR_NATIVE_INSTANCE_DESTROYED,
        memory_order_release);
    errno = 0;
    return 0;
}

static void release_running_instances(
    leir_native_instance_t *const *instances,
    size_t count) {
    while (count > 0U) {
        count -= 1U;
        atomic_store_explicit(
            &instances[count]->activity,
            LEIR_NATIVE_INSTANCE_IDLE,
            memory_order_release);
    }
}

static int validate_batch_container(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    leir_native_batch_metrics_t *batch_metrics_out) {
    size_t i;
    size_t j;

    if (instances == NULL ||
        values_out == NULL ||
        value_counts == NULL ||
        metrics_out == NULL ||
        batch_metrics_out == NULL ||
        instance_count == 0U ||
        instance_count >
            LEIR_NATIVE_MAX_BATCH_SEGMENTS) {
        return EINVAL;
    }
    for (i = 0U; i < instance_count; i += 1U) {
        leir_native_instance_t *instance = instances[i];

        if (instance == NULL ||
            values_out[i] == NULL) {
            return EINVAL;
        }
        for (j = 0U; j < i; j += 1U) {
            if (instances[j] == instance) {
                return EINVAL;
            }
        }
    }
    return 0;
}

static int validate_acquired_batch(
    leir_native_instance_t *const *instances,
    const size_t *value_counts,
    size_t instance_count) {
    size_t i;
    leir_native_mode_t mode = instances[0]->mode;

    for (i = 0U; i < instance_count; i += 1U) {
        leir_native_instance_t *instance = instances[i];

        if (instance->program == NULL ||
            value_counts[i] !=
                instance->program->slot_count ||
            atomic_load_explicit(
                &instance->bound,
                memory_order_acquire) == 0U ||
            instance->mode != mode
#if LLAM_RUNTIME_BACKEND_LINUX
            || instance->linux_state == NULL
#endif
        ) {
            return EINVAL;
        }
    }
    return 0;
}

static int acquire_running_instances(
    leir_native_instance_t *const *instances,
    size_t instance_count) {
    size_t acquired = 0U;

    while (acquired < instance_count) {
        unsigned expected = LEIR_NATIVE_INSTANCE_IDLE;

        if (!atomic_compare_exchange_strong_explicit(
                &instances[acquired]->activity,
                &expected,
                LEIR_NATIVE_INSTANCE_RUNNING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            release_running_instances(
                instances, acquired);
            return expected ==
                    LEIR_NATIVE_INSTANCE_DESTROYED
                ? EINVAL
                : EBUSY;
        }
        acquired += 1U;
    }
    return 0;
}

int leir_native_batch_run(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    leir_native_batch_metrics_t *batch_metrics_out) {
    int error = validate_batch_container(
        instances,
        values_out,
        value_counts,
        metrics_out,
        instance_count,
        batch_metrics_out);

    if (error != 0) {
        return fail_with_errno(error);
    }
    memset(
        batch_metrics_out,
        0,
        sizeof(*batch_metrics_out));
    error = acquire_running_instances(
        instances, instance_count);
    if (error != 0) {
        return fail_with_errno(error);
    }
    error = validate_acquired_batch(
        instances, value_counts, instance_count);
    if (error != 0) {
        release_running_instances(
            instances, instance_count);
        return fail_with_errno(error);
    }

#if LLAM_RUNTIME_BACKEND_LINUX
    {
        llam_task_t *task = g_llam_tls_task;
        llam_runtime_t *runtime;
        llam_node_t *node;
        llam_linux_native_batch_t batch;
        llam_io_req_t *req;
        leir_native_metrics_t
            before[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
        size_t i;
        int issue_result;
        int saved_errno;

        if (task == NULL ||
            g_llam_tls_shard == NULL ||
            task->owner_runtime == NULL ||
            g_llam_tls_shard->runtime !=
                task->owner_runtime) {
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(EINVAL);
        }
        runtime = task->owner_runtime;
        if (g_llam_tls_shard->io_node_index >=
                runtime->active_nodes ||
            runtime->nodes == NULL) {
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(EINVAL);
        }
        node = &runtime->nodes[
            g_llam_tls_shard->io_node_index];
        for (i = 0U; i < instance_count; i += 1U) {
            if (leir_native_linux_state_attach_fixed(
                    instances[i], runtime, node) != 0) {
                saved_errno =
                    errno != 0 ? errno : ENOTSUP;
                release_running_instances(
                    instances, instance_count);
                return fail_with_errno(saved_errno);
            }
            leir_native_linux_state_copy_fixed_inputs(
                instances[i]);
        }

        memset(&batch, 0, sizeof(batch));
        batch.owner_runtime = runtime;
        batch.segment_count = (unsigned)instance_count;
        atomic_init(
            &batch.state,
            LLAM_LINUX_NATIVE_BATCH_IDLE);
        atomic_init(&batch.terminal_claimed, 0U);
        atomic_init(
            &batch.cancel_state,
            LLAM_LINUX_NATIVE_CANCEL_NONE);
        atomic_init(&batch.cancel_requested, 0U);
        for (i = 0U; i < instance_count; i += 1U) {
            llam_linux_native_segment_t *segment =
                &instances[i]->linux_state->segment;
            uint64_t generation;

            copy_linux_metrics(segment, &before[i]);
            generation = segment->generation;
            generation =
                generation == UINT64_MAX
                    ? UINT64_C(1)
                    : generation + UINT64_C(1);
            segment->generation = generation;
            segment->owner_runtime =
                task->owner_runtime;
            batch.segments[i] = segment;
        }
        req = acquire_embedded_request(task);
        if (req == NULL) {
            saved_errno = errno != 0 ? errno : ENOMEM;
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(saved_errno);
        }
        if (req != &task->embedded_io_req) {
            instances[0]->linux_state->segment
                .hot_allocations += 1U;
            batch_metrics_out->hot_allocations = 1U;
            llam_api_io_req_release(g_llam_tls_shard, req);
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(EAGAIN);
        }

        prepare_request(
            req,
            &instances[instance_count - 1U]
                 ->linux_state->segment.ops[
                     instances[instance_count - 1U]
                         ->linux_state->segment.op_count -
                     1U]);
        issue_result = llam_issue_linux_native_batch(
            &batch, req);
        saved_errno = errno;
        for (i = 0U; i < instance_count; i += 1U) {
            llam_linux_native_segment_t *segment =
                &instances[i]->linux_state->segment;
            leir_native_metrics_t *after =
                &metrics_out[i];
            unsigned segment_state =
                atomic_load_explicit(
                    &segment->state,
                    memory_order_acquire);
            bool activated;

            if (segment_state !=
                    LLAM_LINUX_NATIVE_SEGMENT_RETIRED &&
                segment_state !=
                    LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
                /*
                 * A returned call cannot leave kernel or queue ownership
                 * live. Releasing either the request or an instance would
                 * otherwise permit a backend use-after-free.
                 */
                abort();
            }
            copy_linux_metrics(segment, after);
            activated =
                after->activations >
                before[i].activations;
            leir_native_linux_state_copy_fixed_outputs(
                instances[i], activated);
            publish_results(
                instances[i],
                activated &&
                    segment->first_error == 0 &&
                    segment->semantic_result >= 0);
            memcpy(
                values_out[i],
                instances[i]->slots,
                value_counts[i] *
                    sizeof(values_out[i][0]));

            batch_metrics_out->segments +=
                after->activations -
                before[i].activations;
            batch_metrics_out->operation_sqes +=
                after->prepared_sqes -
                before[i].prepared_sqes;
            batch_metrics_out->operation_cqes +=
                after->observed_cqes -
                before[i].observed_cqes;
            batch_metrics_out->hot_allocations +=
                after->hot_allocations -
                before[i].hot_allocations;
        }
        batch_metrics_out->activations =
            metrics_out[0].activations -
            before[0].activations;
        batch_metrics_out->queue_publications =
            metrics_out[0].queue_publications -
            before[0].queue_publications;
        batch_metrics_out->task_parks =
            metrics_out[0].task_parks -
            before[0].task_parks;
        batch_metrics_out->terminal_wakes =
            metrics_out[0].terminal_wakes -
            before[0].terminal_wakes;
        batch_metrics_out->cancel_sqes =
            batch.cancel_sqes_prepared;
        batch_metrics_out->cancel_cqes =
            batch.cancel_cqes_observed;

        for (i = 0U; i < instance_count; i += 1U) {
            llam_linux_native_segment_t *segment =
                &instances[i]->linux_state->segment;

            segment->req = NULL;
            segment->owner_node = NULL;
            segment->next = NULL;
            segment->batch = NULL;
            atomic_store_explicit(
                &segment->state,
                LLAM_LINUX_NATIVE_SEGMENT_IDLE,
                memory_order_release);
        }
        llam_api_io_req_release(g_llam_tls_shard, req);
        release_running_instances(
            instances, instance_count);
        errno = saved_errno;
        return issue_result;
    }
#else
    {
        size_t i;

        for (i = 0U; i < instance_count; i += 1U) {
            memset(
                &metrics_out[i],
                0,
                sizeof(metrics_out[i]));
            metrics_out[i].first_error_operation =
                UINT16_MAX;
            memcpy(
                values_out[i],
                instances[i]->slots,
                value_counts[i] *
                    sizeof(values_out[i][0]));
        }
    }
    release_running_instances(
        instances, instance_count);
    return fail_with_errno(ENOTSUP);
#endif
}

int leir_native_instance_run(
    leir_native_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_native_metrics_t *metrics_out) {
    leir_native_instance_t *instances[1] = {instance};
    leir_phase0_value_t *outputs[1] = {values_out};
    size_t value_counts[1] = {value_count};
    leir_native_batch_metrics_t ignored_batch_metrics;

    return leir_native_batch_run(
        instances,
        outputs,
        value_counts,
        metrics_out,
        1U,
        &ignored_batch_metrics);
}
