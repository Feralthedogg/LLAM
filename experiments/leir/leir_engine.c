#include "leir_phase0_internal.h"

#include "io/runtime_io_api_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        leir_phase0_slot_kind_t length_kind;
        uint64_t requested;
        size_t capacity;

        switch (node->opcode) {
            case LEIR_PHASE0_OP_READ:
            case LEIR_PHASE0_OP_READ_EXACT:
            case LEIR_PHASE0_OP_WRITE:
            case LEIR_PHASE0_OP_WRITE_ALL:
                length_kind = program->slot_kinds[node->length_slot];
                if (length_kind == LEIR_PHASE0_SLOT_I64) {
                    if (values[node->length_slot].i64 < 0) {
                        break;
                    }
                    requested =
                        (uint64_t)values[node->length_slot].i64;
                } else {
                    requested = values[node->length_slot].u64;
                }
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
    atomic_init(&instance->cancel_readers, 0U);
    atomic_init(
        &instance->cancel_phase, LEIR_PHASE0_CANCEL_PHASE_IDLE);
    atomic_init(&instance->running, 0U);
    atomic_init(&instance->terminal_claimed, 0U);
    atomic_init(&instance->terminal, 0U);
    atomic_init(&instance->initial_submit_observed, 0U);
    atomic_init(&instance->test_completion_claimed, 0U);
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
        atomic_load_explicit(&instance->task, memory_order_acquire) != NULL ||
        atomic_load_explicit(
            &instance->cancel_readers, memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &instance->cancel_phase, memory_order_acquire) !=
            LEIR_PHASE0_CANCEL_PHASE_IDLE) {
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
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_IDLE,
        memory_order_release);
    atomic_store_explicit(
        &instance->terminal_claimed, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->terminal, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->initial_submit_observed, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->test_completion_claimed, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->request_generation, 0U, memory_order_release);
    (void)advance_activation_generation(instance);
    return 0;
}

static bool cancel_reader_acquire(
    leir_phase0_instance_t *instance) {
    unsigned readers = atomic_load_explicit(
        &instance->cancel_readers, memory_order_acquire);

    for (;;) {
        if (readers == UINT_MAX) {
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &instance->cancel_readers,
                &readers,
                readers + 1U,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return true;
        }
    }
}

static void cancel_reader_release(
    leir_phase0_instance_t *instance) {
    unsigned previous = atomic_fetch_sub_explicit(
        &instance->cancel_readers, 1U, memory_order_acq_rel);

    if (previous == 0U) {
        abort();
    }
}

static void unpublish_cancel_target(
    leir_phase0_instance_t *instance) {
    atomic_store_explicit(
        &instance->task, NULL, memory_order_release);
    while (atomic_load_explicit(
               &instance->cancel_readers,
               memory_order_acquire) != 0U) {
        llam_yield();
    }
}

static void cancel_wait_yield(void) {
    if (g_llam_tls_task != NULL && g_llam_tls_shard != NULL) {
        llam_yield();
    } else {
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 100000L,
        };

        (void)nanosleep(&delay, NULL);
    }
}

int leir_phase0_instance_cancel(
    leir_phase0_instance_t *instance) {
    llam_task_t *task;

    if (instance == NULL || instance->program == NULL) {
        return fail_with_errno(EINVAL);
    }
    atomic_store_explicit(
        &instance->cancel_requested, 1U, memory_order_release);
    if (!cancel_reader_acquire(instance)) {
        return -1;
    }

    task = atomic_load_explicit(
        &instance->task, memory_order_acquire);
    while (task != NULL &&
           atomic_load_explicit(
               &instance->cancel_phase, memory_order_acquire) ==
               LEIR_PHASE0_CANCEL_PHASE_SUBMITTING &&
           atomic_load_explicit(
               &instance->running, memory_order_acquire) != 0U &&
           atomic_load_explicit(
               &instance->terminal, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &instance->task, memory_order_acquire) == task) {
        llam_io_req_t *req = atomic_load_explicit(
            &instance->req, memory_order_acquire);

        if (req != NULL &&
            atomic_load_explicit(
                &req->wait_mode, memory_order_acquire) !=
                LLAM_IO_WAIT_MODE_NONE) {
            uint64_t wait_generation =
                (uint64_t)atomic_load_explicit(
                    &task->wait_generation, memory_order_acquire);

            if (wait_generation != 0U &&
                wait_generation != UINT64_MAX) {
                atomic_store_explicit(
                    &instance->initial_submit_observed,
                    1U,
                    memory_order_release);
                bool detached = llam_abort_io_wait(
                    task,
                    LLAM_IO_ABORT_CANCEL,
                    wait_generation);
                unsigned abort_reason = atomic_load_explicit(
                    &req->abort_reason, memory_order_acquire);

                if (detached ||
                    abort_reason == LLAM_IO_ABORT_CANCEL) {
                    break;
                }
            }
        }
        cancel_wait_yield();
    }
    cancel_reader_release(instance);
    return 0;
}

static bool leir_phase0_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context);

static bool node_is_read(const leir_phase0_node_desc_t *node) {
    return node->opcode == LEIR_PHASE0_OP_READ ||
           node->opcode == LEIR_PHASE0_OP_READ_EXACT;
}

static bool node_is_write(const leir_phase0_node_desc_t *node) {
    return node->opcode == LEIR_PHASE0_OP_WRITE ||
           node->opcode == LEIR_PHASE0_OP_WRITE_ALL;
}

static bool node_is_exact(const leir_phase0_node_desc_t *node) {
    return node->opcode == LEIR_PHASE0_OP_READ_EXACT ||
           node->opcode == LEIR_PHASE0_OP_WRITE_ALL;
}

static bool current_node_is_terminal(
    const leir_phase0_instance_t *instance) {
    uint16_t opcode =
        instance->program->nodes[instance->current_node].opcode;

    return opcode == LEIR_PHASE0_OP_RETURN ||
           opcode == LEIR_PHASE0_OP_FAIL;
}

static int requested_length(
    const leir_phase0_instance_t *instance,
    const leir_phase0_node_desc_t *node,
    size_t *requested_out) {
    leir_phase0_slot_kind_t length_kind;
    uint64_t requested;

    if (instance == NULL || node == NULL || requested_out == NULL ||
        (!node_is_read(node) && !node_is_write(node))) {
        return fail_with_errno(EINVAL);
    }

    length_kind =
        instance->program->slot_kinds[node->length_slot];
    if (length_kind == LEIR_PHASE0_SLOT_I64) {
        int64_t signed_length =
            instance->slots[node->length_slot].i64;

        if (signed_length < 0) {
            return fail_with_errno(EINVAL);
        }
        requested = (uint64_t)signed_length;
    } else if (length_kind == LEIR_PHASE0_SLOT_U64) {
        requested = instance->slots[node->length_slot].u64;
    } else {
        return fail_with_errno(EINVAL);
    }

    if (requested > (uint64_t)SIZE_MAX) {
        return fail_with_errno(EOVERFLOW);
    }
    *requested_out = (size_t)requested;
    return 0;
}

static int current_io_args(
    leir_phase0_instance_t *instance,
    llam_fd_t *fd_out,
    void **buffer_out,
    size_t *count_out,
    bool *write_out) {
    const leir_phase0_node_desc_t *node;
    unsigned char *data;
    size_t capacity;
    size_t requested;

    if (instance == NULL || fd_out == NULL || buffer_out == NULL ||
        count_out == NULL || write_out == NULL) {
        return fail_with_errno(EINVAL);
    }
    node = &instance->program->nodes[instance->current_node];
    if ((!node_is_read(node) && !node_is_write(node)) ||
        requested_length(instance, node, &requested) != 0) {
        return -1;
    }

    data = instance->slots[node->buffer_slot].buffer.data;
    capacity = instance->slots[node->buffer_slot].buffer.size;
    if (requested > capacity ||
        instance->node_progress > requested ||
        (requested != 0U && data == NULL)) {
        return fail_with_errno(EINVAL);
    }

    *fd_out = instance->slots[node->fd_slot].fd;
    *buffer_out =
        data != NULL ? data + instance->node_progress : NULL;
    *count_out = requested - instance->node_progress;
    *write_out = node_is_write(node);
    return 0;
}

static int prepare_request(
    leir_phase0_instance_t *instance,
    llam_io_req_t *req) {
    llam_fd_t fd;
    void *buffer;
    size_t count;
    bool write_op;

    if (req == NULL ||
        current_io_args(
            instance, &fd, &buffer, &count, &write_op) != 0) {
        return -1;
    }
    req->kind =
        write_op ? LLAM_IO_KIND_WRITE : LLAM_IO_KIND_READ;
    req->fd = fd;
    req->buf = buffer;
    req->count = count;
    req->completion_sink = leir_phase0_completion_sink;
    req->completion_sink_context = instance;
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

static bool cancellation_is_requested(
    const leir_phase0_instance_t *instance) {
    return atomic_load_explicit(
               &instance->cancel_requested,
               memory_order_acquire) != 0U;
}

static void mark_current_io_cancelled(
    leir_phase0_instance_t *instance) {
    const leir_phase0_node_desc_t *node =
        &instance->program->nodes[instance->current_node];

    if (node_is_read(node) || node_is_write(node)) {
        instance->slots[node->result_slot].i64 = -1;
    }
    instance->terminal_error = ECANCELED;
}

static bool claim_terminal(
    leir_phase0_instance_t *instance) {
    unsigned expected = 0U;

    return atomic_compare_exchange_strong_explicit(
        &instance->terminal_claimed,
        &expected,
        1U,
        memory_order_acq_rel,
        memory_order_acquire);
}

static void finish_terminal_publication(
    leir_phase0_instance_t *instance) {
    instance->metrics.terminal_publications += 1U;
    atomic_store_explicit(
        &instance->terminal, 1U, memory_order_release);
    atomic_store_explicit(
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_IDLE,
        memory_order_release);
}

static bool publish_engine_error(
    leir_phase0_instance_t *instance,
    int error_code) {
    if (!claim_terminal(instance)) {
        return false;
    }
    instance->terminal_error =
        error_code != 0 ? error_code : EIO;
    finish_terminal_publication(instance);
    return false;
}

static bool publish_cancellation(
    leir_phase0_instance_t *instance) {
    if (!claim_terminal(instance)) {
        return false;
    }
    mark_current_io_cancelled(instance);
    finish_terminal_publication(instance);
    return false;
}

static bool publish_current_terminal(
    leir_phase0_instance_t *instance) {
    const leir_phase0_node_desc_t *terminal_node =
        &instance->program->nodes[instance->current_node];

    if (!claim_terminal(instance)) {
        return false;
    }
    if (terminal_node->opcode == LEIR_PHASE0_OP_RETURN) {
        instance->terminal_error = 0;
    } else if (terminal_node->opcode == LEIR_PHASE0_OP_FAIL) {
        if (instance->terminal_error == 0) {
            instance->terminal_error = EIO;
        }
    } else {
        instance->terminal_error = EPROTO;
    }
    finish_terminal_publication(instance);
    return false;
}

static int leir_phase0_apply_result(
    leir_phase0_instance_t *instance,
    ssize_t result,
    int error_code) {
    const leir_phase0_node_desc_t *node =
        &instance->program->nodes[instance->current_node];
    size_t requested;
    size_t remaining;

    if ((!node_is_read(node) && !node_is_write(node)) ||
        requested_length(instance, node, &requested) != 0 ||
        instance->node_progress > requested) {
        instance->terminal_error =
            errno != 0 ? errno : EPROTO;
        return -1;
    }
    remaining = requested - instance->node_progress;

    if (error_code != 0 || result < 0) {
        instance->slots[node->result_slot].i64 = -1;
        instance->terminal_error =
            error_code != 0 ? error_code : EIO;
        instance->current_node = node->on_error;
        instance->node_progress = 0U;
        return 0;
    }

    if (result == 0) {
        if (remaining == 0U) {
            instance->slots[node->result_slot].i64 =
                (int64_t)instance->node_progress;
            instance->terminal_error = 0;
            instance->current_node = node->on_success;
        } else if (node_is_read(node)) {
            instance->slots[node->result_slot].i64 =
                (int64_t)instance->node_progress;
            instance->terminal_error = EPIPE;
            instance->current_node = node->on_eof;
        } else {
            instance->slots[node->result_slot].i64 = -1;
            instance->terminal_error = EIO;
            instance->current_node = node->on_error;
        }
        instance->node_progress = 0U;
        return 0;
    }

    if ((uint64_t)result > (uint64_t)remaining ||
        instance->node_progress + (size_t)result >
            (size_t)INT64_MAX) {
        instance->slots[node->result_slot].i64 = -1;
        instance->terminal_error =
            (uint64_t)result > (uint64_t)remaining
                ? EPROTO
                : EOVERFLOW;
        instance->current_node = node->on_error;
        instance->node_progress = 0U;
        return 0;
    }

    instance->node_progress += (size_t)result;
    instance->slots[node->result_slot].i64 =
        (int64_t)instance->node_progress;
    instance->terminal_error = 0;
    if (!node_is_exact(node) ||
        instance->node_progress == requested) {
        instance->current_node = node->on_success;
        instance->node_progress = 0U;
    }
    return 0;
}

static bool request_completion_identity_is_live(
    leir_phase0_instance_t *instance,
    llam_io_req_t *req) {
    uint64_t expected_generation = (uint64_t)atomic_load_explicit(
        &instance->request_generation, memory_order_acquire);
    uint64_t observed_generation = req != NULL
        ? (uint64_t)atomic_load_explicit(
              &req->operation_generation, memory_order_acquire)
        : 0U;
    bool live =
        atomic_load_explicit(
            &instance->running, memory_order_acquire) == 1U &&
        atomic_load_explicit(
            &instance->terminal_claimed, memory_order_acquire) == 0U &&
        atomic_load_explicit(
            &instance->terminal, memory_order_acquire) == 0U;

    if (live &&
        atomic_load_explicit(
            &instance->req, memory_order_acquire) == req &&
        expected_generation != 0U &&
        observed_generation == expected_generation) {
        return true;
    }

    instance->metrics.stale_completions += 1U;
    if (live) {
        (void)publish_engine_error(instance, EPROTO);
    }
    return false;
}

bool leir_phase0_test_inject_completion(
    leir_phase0_instance_t *instance,
    uint64_t activation_generation,
    ssize_t result,
    int error_code) {
    uint64_t current_generation;
    bool live;

    if (instance == NULL || instance->program == NULL ||
        activation_generation == 0U ||
        activation_generation == UINT64_MAX) {
        errno = EINVAL;
        return false;
    }

    current_generation = (uint64_t)atomic_load_explicit(
        &instance->activation_generation, memory_order_acquire);
    live =
        atomic_load_explicit(
            &instance->running, memory_order_acquire) == 1U &&
        atomic_load_explicit(
            &instance->terminal_claimed, memory_order_acquire) == 0U &&
        atomic_load_explicit(
            &instance->terminal, memory_order_acquire) == 0U;
    if (!live || current_generation != activation_generation) {
        instance->metrics.stale_completions += 1U;
        if (live) {
            (void)publish_engine_error(instance, EPROTO);
        }
        return false;
    }
    if (atomic_exchange_explicit(
            &instance->test_completion_claimed,
            1U,
            memory_order_acq_rel) != 0U) {
        instance->metrics.stale_completions += 1U;
        return false;
    }

    instance->metrics.effect_completions += 1U;
    if (leir_phase0_apply_result(
            instance, result, error_code) != 0) {
        (void)publish_engine_error(
            instance,
            instance->terminal_error != 0
                ? instance->terminal_error
                : EPROTO);
        return true;
    }
    if (current_node_is_terminal(instance)) {
        (void)publish_current_terminal(instance);
    }
    return true;
}

static leir_phase0_advance_result_t leir_phase0_advance_direct(
    leir_phase0_instance_t *instance) {
    for (;;) {
        llam_fd_t fd;
        void *buffer;
        size_t count;
        bool write_op;
        ssize_t direct_result = -1;
        int direct_rc;
        int direct_error;

        if (cancellation_is_requested(instance)) {
            mark_current_io_cancelled(instance);
            return LEIR_PHASE0_ADVANCE_ERROR;
        }
        if (current_node_is_terminal(instance)) {
            (void)publish_current_terminal(instance);
            return LEIR_PHASE0_ADVANCE_TERMINAL;
        }
        if (instance->opts.force_backend) {
            return LEIR_PHASE0_ADVANCE_NEEDS_BACKEND;
        }
        if (instance->inline_left == 0U) {
            instance->metrics.fairness_resubmits += 1U;
            return LEIR_PHASE0_ADVANCE_NEEDS_BACKEND;
        }
        if (current_io_args(
                instance,
                &fd,
                &buffer,
                &count,
                &write_op) != 0) {
            instance->terminal_error =
                errno != 0 ? errno : EPROTO;
            return LEIR_PHASE0_ADVANCE_ERROR;
        }

        errno = 0;
        direct_rc = llam_try_direct_rw(
            fd,
            buffer,
            count,
            write_op,
            false,
            0,
            &direct_result,
            NULL);
        direct_error = errno;
        if (cancellation_is_requested(instance)) {
            mark_current_io_cancelled(instance);
            return LEIR_PHASE0_ADVANCE_ERROR;
        }
        if (direct_rc == 0) {
            return LEIR_PHASE0_ADVANCE_NEEDS_BACKEND;
        }

        instance->inline_left -= 1U;
        instance->metrics.effect_completions += 1U;
        instance->metrics.direct_completions += 1U;
        if (leir_phase0_apply_result(
                instance,
                direct_rc > 0 ? direct_result : -1,
                direct_rc > 0
                    ? 0
                    : (direct_error != 0 ? direct_error : EIO)) != 0) {
            return LEIR_PHASE0_ADVANCE_ERROR;
        }
        if (current_node_is_terminal(instance)) {
            (void)publish_current_terminal(instance);
            return LEIR_PHASE0_ADVANCE_TERMINAL;
        }
        instance->metrics.task_resumes_avoided += 1U;
    }
}

static bool leir_phase0_resubmit(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    leir_phase0_instance_t *instance) {
    llam_runtime_t *runtime =
        req != NULL ? req->owner_runtime : NULL;
    int saved_errno;

    if (node == NULL || req == NULL || instance == NULL ||
        runtime == NULL || node->runtime != runtime ||
        completion_owner >= runtime->active_shards ||
        node->index >= runtime->active_nodes) {
        instance->terminal_error = EPROTO;
        errno = EPROTO;
        return false;
    }
    if (cancellation_is_requested(instance)) {
        mark_current_io_cancelled(instance);
        errno = ECANCELED;
        return false;
    }
    if (prepare_request(instance, req) != 0) {
        instance->terminal_error =
            errno != 0 ? errno : EPROTO;
        return false;
    }

    req->result = -1;
    req->error_code = 0;
    atomic_store_explicit(
        &req->owner_shard, completion_owner, memory_order_release);
    atomic_store_explicit(
        &req->attached_node_index,
        node->index,
        memory_order_release);
    atomic_store_explicit(
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_SUBMITTING,
        memory_order_release);
    if (cancellation_is_requested(instance)) {
        atomic_store_explicit(
            &instance->cancel_phase,
            LEIR_PHASE0_CANCEL_PHASE_CALLBACK,
            memory_order_release);
        req->result = -1;
        req->error_code = ECANCELED;
        mark_current_io_cancelled(instance);
        errno = ECANCELED;
        return false;
    }
    atomic_store_explicit(
        &req->wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    req->submit_ts_ns = llam_now_ns();

    if (!llam_node_submit_io_req(node, req)) {
        saved_errno = errno != 0 ? errno : EIO;
        atomic_store_explicit(
            &req->wait_mode,
            LLAM_IO_WAIT_MODE_NONE,
            memory_order_release);
        req->result = -1;
        req->error_code = saved_errno;
        instance->terminal_error = saved_errno;
        atomic_store_explicit(
            &instance->cancel_phase,
            LEIR_PHASE0_CANCEL_PHASE_CALLBACK,
            memory_order_release);
        errno = saved_errno;
        return false;
    }

    instance->metrics.backend_submits += 1U;
    llam_kick_node(node);
    return true;
}

static bool leir_phase0_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    leir_phase0_instance_t *instance = context;
    leir_phase0_advance_result_t advance_result;

    if (instance == NULL || req == NULL || wake_reason == NULL) {
        return false;
    }
    if (!request_completion_identity_is_live(instance, req)) {
        return false;
    }
    atomic_store_explicit(
        &instance->initial_submit_observed,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_CALLBACK,
        memory_order_release);
    if (cancellation_is_requested(instance)) {
        req->result = -1;
        req->error_code = ECANCELED;
        *wake_reason = LLAM_WAIT_CANCEL;
        return publish_cancellation(instance);
    }
    if (*wake_reason != LLAM_WAIT_IO) {
        if (*wake_reason == LLAM_WAIT_CANCEL) {
            return publish_cancellation(instance);
        }
        return publish_engine_error(
            instance, completion_error(req, *wake_reason));
    }

    instance->metrics.effect_completions += 1U;
    instance->inline_left =
        instance->opts.inline_budget > 0U
            ? instance->opts.inline_budget - 1U
            : 0U;
    if (leir_phase0_apply_result(
            instance, req->result, req->error_code) != 0) {
        return publish_engine_error(
            instance,
            instance->terminal_error != 0
                ? instance->terminal_error
                : EPROTO);
    }
    if (cancellation_is_requested(instance)) {
        req->result = -1;
        req->error_code = ECANCELED;
        *wake_reason = LLAM_WAIT_CANCEL;
        return publish_cancellation(instance);
    }
    if (current_node_is_terminal(instance)) {
        return publish_current_terminal(instance);
    }

    instance->metrics.task_resumes_avoided += 1U;
    advance_result = leir_phase0_advance_direct(instance);
    if (advance_result == LEIR_PHASE0_ADVANCE_TERMINAL) {
        return false;
    }
    if (advance_result == LEIR_PHASE0_ADVANCE_ERROR) {
        if (instance->terminal_error == ECANCELED) {
            req->result = -1;
            req->error_code = ECANCELED;
            *wake_reason = LLAM_WAIT_CANCEL;
            return publish_cancellation(instance);
        }
        return publish_engine_error(
            instance,
            instance->terminal_error != 0
                ? instance->terminal_error
                : EPROTO);
    }
    if (leir_phase0_resubmit(
            node, req, completion_owner, instance)) {
        return true;
    }
    if (instance->terminal_error == ECANCELED) {
        req->result = -1;
        req->error_code = ECANCELED;
        *wake_reason = LLAM_WAIT_CANCEL;
        return publish_cancellation(instance);
    }
    return publish_engine_error(
        instance,
        instance->terminal_error != 0
            ? instance->terminal_error
            : EIO);
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

static bool embedded_request_is_reusable(const llam_task_t *task) {
    const llam_io_req_t *req;

    if (task == NULL ||
        llam_task_active_io_req_load(task) != NULL) {
        return false;
    }
    req = &task->embedded_io_req;
    return atomic_load_explicit(
               &req->lifetime_refs, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->cancel_queued, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->cancel_submitted, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->free_after_cancel, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->backend_event_refs, memory_order_acquire) == 0U &&
           atomic_load_explicit(
               &req->release_after_event, memory_order_acquire) == 0U;
}

static llam_io_req_t *acquire_embedded_request(
    llam_shard_t *shard,
    llam_task_t *task) {
    /*
     * A backend may make the task runnable before dropping its event-batch
     * lifetime pin. Generic task I/O may use an allocator-backed request in
     * that narrow window, but LEIR's Phase 0A contract forbids hot-path
     * request allocation. Cooperatively wait for the task-owned slot instead;
     * the measured scheduling cost remains visible in the paired benchmark.
     */
    while (!embedded_request_is_reusable(task)) {
        llam_yield();
    }
    return llam_api_io_req_acquire(shard);
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
    if (atomic_load_explicit(
            &instance->cancel_requested, memory_order_acquire) != 0U) {
        (void)publish_cancellation(instance);
        copy_outputs(instance, values_out, metrics_out);
        atomic_store_explicit(
            &instance->running, 0U, memory_order_release);
        errno = ECANCELED;
        return -1;
    }

    req = acquire_embedded_request(g_llam_tls_shard, task);
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
    atomic_store_explicit(
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_SUBMITTING,
        memory_order_release);
    atomic_store_explicit(&instance->task, task, memory_order_release);
    if (atomic_load_explicit(
            &instance->cancel_requested, memory_order_acquire) != 0U) {
        saved_errno = ECANCELED;
        req->result = -1;
        req->error_code = ECANCELED;
        (void)publish_cancellation(instance);
        unpublish_cancel_target(instance);
        atomic_store_explicit(
            &instance->req, NULL, memory_order_release);
        llam_api_io_req_release(g_llam_tls_shard, req);
        copy_outputs(instance, values_out, metrics_out);
        atomic_store_explicit(
            &instance->running, 0U, memory_order_release);
        errno = saved_errno;
        return -1;
    }
    instance->metrics.backend_submits += 1U;
    instance->metrics.task_parks += 1U;

    issue_result = llam_issue_io(
        req,
        instance->opts.has_deadline,
        instance->opts.has_deadline
            ? instance->opts.deadline_ns
            : 0U);
    saved_errno = errno;
    if (issue_result == 0) {
        atomic_store_explicit(
            &instance->initial_submit_observed,
            1U,
            memory_order_release);
    }
    atomic_store_explicit(
        &instance->cancel_phase,
        LEIR_PHASE0_CANCEL_PHASE_IDLE,
        memory_order_release);
    if (atomic_load_explicit(
            &instance->initial_submit_observed,
            memory_order_acquire) == 0U) {
        instance->metrics.backend_submits -= 1U;
        instance->metrics.task_parks -= 1U;
    }
    if (atomic_load_explicit(
            &instance->terminal, memory_order_acquire) == 0U) {
        int terminal_error = issue_result != 0
            ? (saved_errno != 0 ? saved_errno : EIO)
            : EPROTO;

        if (terminal_error == ECANCELED ||
            cancellation_is_requested(instance)) {
            (void)publish_cancellation(instance);
        } else {
            (void)publish_engine_error(instance, terminal_error);
        }
    }

    req->completion_sink = NULL;
    req->completion_sink_context = NULL;
    atomic_store_explicit(&instance->req, NULL, memory_order_release);
    unpublish_cancel_target(instance);
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
