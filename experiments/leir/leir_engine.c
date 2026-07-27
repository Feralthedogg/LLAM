#include "leir_phase0_internal.h"

#include "io/runtime_io_api_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static bool fd_is_valid(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    return !LLAM_FD_IS_INVALID(fd);
#else
    return fd >= 0;
#endif
}

static bool bindings_are_valid(
    const leir_phase0_program_t *program,
    const leir_phase0_value_t *values) {
    size_t i;

    for (i = 0U; i < program->slot_count; i += 1U) {
        switch (program->slot_kinds[i]) {
            case LEIR_PHASE0_SLOT_FD:
                if (!fd_is_valid(values[i].fd)) {
                    return false;
                }
                break;
            case LEIR_PHASE0_SLOT_MUT_BUFFER:
            case LEIR_PHASE0_SLOT_CONST_BUFFER:
                if (values[i].buffer.data == NULL &&
                    values[i].buffer.size != 0U) {
                    return false;
                }
                break;
            case LEIR_PHASE0_SLOT_U64:
            case LEIR_PHASE0_SLOT_I64:
                break;
        }
    }

    for (i = 0U; i < program->node_count; i += 1U) {
        const leir_phase0_node_desc_t *node = &program->nodes[i];
        uint64_t requested;
        size_t capacity;

        switch (node->opcode) {
            case LEIR_PHASE0_OP_READ:
            case LEIR_PHASE0_OP_READ_EXACT:
            case LEIR_PHASE0_OP_WRITE:
            case LEIR_PHASE0_OP_WRITE_ALL:
                requested = values[node->length_slot].u64;
                capacity = values[node->buffer_slot].buffer.size;
                if (requested > SIZE_MAX ||
                    (size_t)requested > capacity ||
                    (requested != 0U &&
                     values[node->buffer_slot].buffer.data == NULL)) {
                    return false;
                }
                break;
            case LEIR_PHASE0_OP_RETURN:
            case LEIR_PHASE0_OP_FAIL:
                break;
            default:
                return false;
        }
    }
    return true;
}

static uint64_t advance_activation_generation(
    leir_phase0_instance_t *instance) {
    uint_fast64_t current = atomic_load_explicit(
        &instance->activation_generation, memory_order_acquire);

    for (;;) {
        uint_fast64_t next =
            current == UINT64_MAX ? 1U : current + 1U;

        if (atomic_compare_exchange_weak_explicit(
                &instance->activation_generation,
                &current,
                next,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return (uint64_t)next;
        }
    }
}

size_t leir_phase0_instance_size(void) {
    return sizeof(leir_phase0_instance_t);
}

int leir_phase0_instance_init(
    void *storage,
    size_t storage_size,
    const leir_phase0_program_t *program) {
    leir_phase0_instance_t *instance;

    if (storage == NULL || program == NULL ||
        storage_size < sizeof(leir_phase0_instance_t) ||
        (uintptr_t)storage % _Alignof(leir_phase0_instance_t) != 0U) {
        return fail_with_errno(EINVAL);
    }

    memset(storage, 0, sizeof(leir_phase0_instance_t));
    instance = storage;
    instance->program = program;
    atomic_init(&instance->req, NULL);
    atomic_init(&instance->task, NULL);
    atomic_init(&instance->cancel_requested, 0U);
    atomic_init(&instance->running, 0U);
    atomic_init(&instance->terminal, 0U);
    atomic_init(&instance->activation_generation, 0U);
    atomic_init(&instance->request_generation, 0U);
    instance->current_node = program->entry_node;
    return 0;
}

int leir_phase0_instance_bind(
    leir_phase0_instance_t *instance,
    const leir_phase0_value_t *values,
    size_t value_count,
    const leir_phase0_run_opts_t *opts) {
    const leir_phase0_program_t *program;

    if (instance == NULL || values == NULL || opts == NULL) {
        return fail_with_errno(EINVAL);
    }
    program = instance->program;
    if (program == NULL || value_count != program->slot_count) {
        return fail_with_errno(EINVAL);
    }
    if (atomic_load_explicit(
            &instance->running, memory_order_acquire) != 0U ||
        atomic_load_explicit(&instance->req, memory_order_acquire) != NULL ||
        atomic_load_explicit(&instance->task, memory_order_acquire) != NULL) {
        return fail_with_errno(EBUSY);
    }
    if (!bindings_are_valid(program, values)) {
        return fail_with_errno(EINVAL);
    }

    memcpy(
        instance->slots,
        values,
        value_count * sizeof(instance->slots[0]));
    instance->opts = *opts;
    memset(&instance->metrics, 0, sizeof(instance->metrics));
    instance->metrics.activations = 1U;
    instance->current_node = program->entry_node;
    instance->node_progress = 0U;
    instance->inline_left = opts->inline_budget;
    instance->terminal_error = 0;
    atomic_store_explicit(
        &instance->cancel_requested, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->terminal, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->request_generation, 0U, memory_order_release);
    (void)advance_activation_generation(instance);
    return 0;
}

static int completion_error(
    const llam_io_req_t *req,
    llam_wait_reason_t wake_reason) {
    if (req->error_code != 0) {
        return req->error_code;
    }
    if (wake_reason == LLAM_WAIT_CANCEL) {
        return ECANCELED;
    }
    if (wake_reason == LLAM_WAIT_TIMEOUT) {
        return ETIMEDOUT;
    }
    return EIO;
}

static bool publish_terminal(
    leir_phase0_instance_t *instance,
    const leir_phase0_node_desc_t *terminal_node,
    int failure_error) {
    instance->terminal_error =
        terminal_node->opcode == LEIR_PHASE0_OP_RETURN
            ? 0
            : (failure_error != 0 ? failure_error : EIO);
    instance->metrics.terminal_publications += 1U;
    atomic_store_explicit(
        &instance->terminal, 1U, memory_order_release);
    return false;
}

static bool leir_phase0_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    leir_phase0_instance_t *instance = context;
    const leir_phase0_program_t *program;
    const leir_phase0_node_desc_t *node_desc;
    const leir_phase0_node_desc_t *next;
    uint64_t expected_generation;
    uint64_t observed_generation;
    uint16_t edge;
    int failure_error = 0;

    (void)node;
    (void)completion_owner;
    if (instance == NULL || req == NULL || wake_reason == NULL) {
        return false;
    }
    expected_generation = (uint64_t)atomic_load_explicit(
        &instance->request_generation, memory_order_acquire);
    observed_generation = (uint64_t)atomic_load_explicit(
        &req->operation_generation, memory_order_acquire);
    if (atomic_load_explicit(&instance->req, memory_order_acquire) != req ||
        expected_generation == 0U ||
        observed_generation != expected_generation) {
        instance->metrics.stale_completions += 1U;
        return false;
    }

    program = instance->program;
    node_desc = &program->nodes[instance->current_node];
    instance->metrics.effect_completions += 1U;
    if (*wake_reason != LLAM_WAIT_IO || req->error_code != 0 ||
        req->result < 0) {
        failure_error = completion_error(req, *wake_reason);
        instance->slots[node_desc->result_slot].i64 = -1;
        edge = node_desc->on_error;
    } else if (req->result == 0) {
        instance->slots[node_desc->result_slot].i64 = 0;
        edge = node_desc->on_eof;
        failure_error = EPIPE;
    } else {
        instance->node_progress += (size_t)req->result;
        instance->slots[node_desc->result_slot].i64 =
            (int64_t)instance->node_progress;
        edge = node_desc->on_success;
    }

    instance->current_node = edge;
    next = &program->nodes[edge];
    if (next->opcode == LEIR_PHASE0_OP_RETURN ||
        next->opcode == LEIR_PHASE0_OP_FAIL) {
        return publish_terminal(instance, next, failure_error);
    }

    instance->terminal_error = ENOTSUP;
    instance->metrics.terminal_publications += 1U;
    atomic_store_explicit(
        &instance->terminal, 1U, memory_order_release);
    return false;
}

static int prepare_request(
    leir_phase0_instance_t *instance,
    llam_io_req_t *req) {
    const leir_phase0_node_desc_t *node =
        &instance->program->nodes[instance->current_node];
    uint64_t requested = instance->slots[node->length_slot].u64;
    unsigned char *data =
        instance->slots[node->buffer_slot].buffer.data;

    switch (node->opcode) {
        case LEIR_PHASE0_OP_READ:
        case LEIR_PHASE0_OP_READ_EXACT:
            req->kind = LLAM_IO_KIND_READ;
            break;
        case LEIR_PHASE0_OP_WRITE:
        case LEIR_PHASE0_OP_WRITE_ALL:
            req->kind = LLAM_IO_KIND_WRITE;
            break;
        default:
            return fail_with_errno(EINVAL);
    }
    req->fd = instance->slots[node->fd_slot].fd;
    req->buf =
        data != NULL ? data + instance->node_progress : NULL;
    req->count = (size_t)requested - instance->node_progress;
    req->completion_sink = leir_phase0_completion_sink;
    req->completion_sink_context = instance;
    return 0;
}

static void copy_outputs(
    const leir_phase0_instance_t *instance,
    leir_phase0_value_t *values_out,
    leir_phase0_metrics_t *metrics_out) {
    memcpy(
        values_out,
        instance->slots,
        instance->program->slot_count * sizeof(values_out[0]));
    *metrics_out = instance->metrics;
}

int leir_phase0_instance_run(
    leir_phase0_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_phase0_metrics_t *metrics_out) {
    llam_io_req_t *req;
    llam_task_t *task = g_llam_tls_task;
    unsigned expected_running = 0U;
    uint64_t request_generation;
    int issue_result;
    int saved_errno;
    int result;

    if (instance == NULL || instance->program == NULL ||
        values_out == NULL || metrics_out == NULL ||
        value_count != instance->program->slot_count ||
        g_llam_tls_shard == NULL || task == NULL ||
        atomic_load_explicit(
            &instance->activation_generation,
            memory_order_acquire) == 0U) {
        return fail_with_errno(EINVAL);
    }
    if (!atomic_compare_exchange_strong_explicit(
            &instance->running,
            &expected_running,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(EBUSY);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        atomic_store_explicit(
            &instance->running, 0U, memory_order_release);
        return -1;
    }
    if (req != &task->embedded_io_req) {
        instance->metrics.heap_requests += 1U;
        instance->metrics.hot_allocations += 1U;
    }
    request_generation = (uint64_t)atomic_load_explicit(
        &req->operation_generation, memory_order_acquire);
    if (request_generation == 0U ||
        prepare_request(instance, req) != 0) {
        saved_errno = request_generation == 0U ? EIO : errno;
        llam_api_io_req_release(g_llam_tls_shard, req);
        atomic_store_explicit(
            &instance->running, 0U, memory_order_release);
        return fail_with_errno(saved_errno);
    }

    atomic_store_explicit(
        &instance->request_generation,
        request_generation,
        memory_order_release);
    atomic_store_explicit(&instance->req, req, memory_order_release);
    atomic_store_explicit(&instance->task, task, memory_order_release);
    instance->metrics.backend_submits += 1U;
    instance->metrics.task_parks += 1U;

    issue_result = llam_issue_io(
        req,
        instance->opts.has_deadline,
        instance->opts.has_deadline
            ? instance->opts.deadline_ns
            : 0U);
    saved_errno = errno;
    if (instance->metrics.effect_completions == 0U) {
        instance->metrics.backend_submits -= 1U;
        instance->metrics.task_parks -= 1U;
    }

    req->completion_sink = NULL;
    req->completion_sink_context = NULL;
    atomic_store_explicit(&instance->req, NULL, memory_order_release);
    atomic_store_explicit(&instance->task, NULL, memory_order_release);
    llam_api_io_req_release(g_llam_tls_shard, req);

    if (atomic_load_explicit(
            &instance->terminal, memory_order_acquire) != 0U) {
        result = instance->terminal_error == 0 ? 0 : -1;
        saved_errno = instance->terminal_error;
    } else if (issue_result != 0) {
        result = -1;
    } else {
        result = -1;
        saved_errno = EPROTO;
    }
    copy_outputs(instance, values_out, metrics_out);
    atomic_store_explicit(
        &instance->running, 0U, memory_order_release);
    errno = saved_errno;
    return result;
}
