// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_aot_linux.c
 * @brief Independent Linux execution ticket for one generated AOT module.
 */

#include "leir_aot_linux.h"

#include "io/runtime_io_api_internal.h"
#include "runtime_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_segment_linux_internal.h"
#endif

#define LEIR_AOT_LINUX_TICKET_MAGIC \
    UINT64_C(0x4c454952414f5431)

enum {
    LEIR_AOT_LINUX_TICKET_INITIALIZED = 1U,
    LEIR_AOT_LINUX_TICKET_BINDING = 2U,
    LEIR_AOT_LINUX_TICKET_BOUND = 3U,
    LEIR_AOT_LINUX_TICKET_RUNNING = 4U,
    LEIR_AOT_LINUX_TICKET_CONSUMED = 5U,
    LEIR_AOT_LINUX_TICKET_DESTROYED = 6U,
};

struct leir_aot_linux_ticket {
    uint64_t magic;
    const leir_aot_module_v1_t *module;
    void *module_instance;
    size_t module_instance_size;
    uint32_t connect_error_continuation;
    uint32_t write_continuation;
    bool prepared;
    atomic_uint state;
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_linux_native_segment_t segment;
#endif
};

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static bool alignment_is_valid(size_t alignment) {
    return alignment != 0U &&
           (alignment & (alignment - 1U)) == 0U;
}

static bool module_is_valid(
    const leir_aot_module_v1_t *module) {
    return module != NULL &&
           module->abi_version == LEIR_AOT_MODULE_ABI_V1 &&
           module->struct_size >= sizeof(*module) &&
           module->backend_kind ==
               LEIR_AOT_BACKEND_LINUX_IO_URING &&
           module->semantic_digest != 0U &&
           module->instance_size != 0U &&
           alignment_is_valid(module->instance_alignment) &&
           module->slot_count != 0U &&
           module->bind != NULL &&
           module->prepare != NULL &&
           module->resume != NULL &&
           module->copy_outputs != NULL &&
           module->cancel != NULL;
}

static bool ticket_is_valid(
    const leir_aot_linux_ticket_t *ticket) {
    return ticket != NULL &&
           ticket->magic == LEIR_AOT_LINUX_TICKET_MAGIC &&
           module_is_valid(ticket->module) &&
           ticket->module_instance != NULL &&
           ticket->module_instance_size >=
               ticket->module->instance_size;
}

size_t leir_aot_linux_ticket_size(void) {
    return sizeof(leir_aot_linux_ticket_t);
}

size_t leir_aot_linux_ticket_alignment(void) {
    return _Alignof(leir_aot_linux_ticket_t);
}

int leir_aot_linux_ticket_init(
    void *ticket_storage,
    size_t ticket_storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size) {
    leir_aot_linux_ticket_t *ticket = ticket_storage;

    if (ticket_storage == NULL ||
        ticket_storage_size < sizeof(*ticket) ||
        (uintptr_t)ticket_storage %
                _Alignof(leir_aot_linux_ticket_t) !=
            0U ||
        !module_is_valid(module) ||
        module_instance == NULL ||
        module_instance_size < module->instance_size ||
        (uintptr_t)module_instance % module->instance_alignment != 0U) {
        return fail_with_errno(EINVAL);
    }
    memset(ticket, 0, sizeof(*ticket));
    ticket->magic = LEIR_AOT_LINUX_TICKET_MAGIC;
    ticket->module = module;
    ticket->module_instance = module_instance;
    ticket->module_instance_size = module_instance_size;
    atomic_init(
        &ticket->state, LEIR_AOT_LINUX_TICKET_INITIALIZED);
    return 0;
}

int leir_aot_linux_ticket_bind(
    leir_aot_linux_ticket_t *ticket,
    const leir_phase0_value_t *values,
    size_t value_count) {
    unsigned expected;
    unsigned prior;
    int result;

    if (!ticket_is_valid(ticket) || values == NULL ||
        value_count != ticket->module->slot_count) {
        return fail_with_errno(EINVAL);
    }
    expected = atomic_load_explicit(
        &ticket->state, memory_order_acquire);
    for (;;) {
        if (expected != LEIR_AOT_LINUX_TICKET_INITIALIZED &&
            expected != LEIR_AOT_LINUX_TICKET_CONSUMED) {
            return fail_with_errno(
                expected == LEIR_AOT_LINUX_TICKET_DESTROYED
                    ? EINVAL
                    : EBUSY);
        }
        prior = expected;
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state,
                &expected,
                LEIR_AOT_LINUX_TICKET_BINDING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }

    ticket->prepared = false;
    result = ticket->module->bind(
        ticket->module_instance,
        ticket->module_instance_size,
        values,
        value_count);
    atomic_store_explicit(
        &ticket->state,
        result == 0 ? LEIR_AOT_LINUX_TICKET_BOUND : prior,
        memory_order_release);
    return result;
}

#if LLAM_RUNTIME_BACKEND_LINUX

static bool range_is_inside_module_instance(
    const leir_aot_linux_ticket_t *ticket,
    const void *pointer,
    size_t length) {
    uintptr_t base = (uintptr_t)ticket->module_instance;
    uintptr_t candidate = (uintptr_t)pointer;
    size_t offset;

    if (pointer == NULL || candidate < base) {
        return false;
    }
    offset = (size_t)(candidate - base);
    return offset <= ticket->module_instance_size &&
           length <= ticket->module_instance_size - offset;
}

static int prepare_connect_write(
    void *context,
    llam_fd_t fd,
    const struct sockaddr *address,
    socklen_t address_length,
    const void *payload,
    size_t payload_length,
    uint64_t generation,
    uint32_t connect_error_continuation,
    uint32_t write_continuation) {
    leir_aot_linux_ticket_t *ticket = context;
    llam_linux_native_op_t ops[2];

    if (!ticket_is_valid(ticket) ||
        atomic_load_explicit(
            &ticket->state, memory_order_acquire) !=
            LEIR_AOT_LINUX_TICKET_RUNNING ||
        ticket->prepared ||
        LLAM_FD_IS_INVALID(fd) ||
        address_length == 0U ||
        address_length > sizeof(struct sockaddr_storage) ||
        !range_is_inside_module_instance(
            ticket, address, (size_t)address_length) ||
        payload_length > UINT32_MAX ||
        (payload_length != 0U && payload == NULL) ||
        generation == 0U ||
        connect_error_continuation == write_continuation) {
        return fail_with_errno(EINVAL);
    }

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_LINUX_NATIVE_OP_CONNECT;
    ops[0].fd = fd;
    ops[0].buffer = (void *)(uintptr_t)address;
    ops[0].length = (uint32_t)address_length;
    ops[1].kind = LLAM_LINUX_NATIVE_OP_SEND;
    ops[1].flags = LLAM_LINUX_NATIVE_OP_PARTIAL_OK;
    ops[1].fd = fd;
    ops[1].buffer = (void *)(uintptr_t)payload;
    ops[1].length = (uint32_t)payload_length;
    if (llam_linux_native_segment_configure(
            &ticket->segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        return -1;
    }
    ticket->segment.generation = generation;
    ticket->connect_error_continuation =
        connect_error_continuation;
    ticket->write_continuation = write_continuation;
    ticket->prepared = true;
    return 0;
}

static void prepare_request(
    llam_io_req_t *request,
    const llam_linux_native_op_t *write) {
    request->kind = LLAM_IO_KIND_WRITE;
    request->fd = write->fd;
    request->buf = write->buffer;
    request->count = write->length;
    request->use_recv_op = false;
    request->recv_flags = 0;
    request->use_send_op = true;
    request->completion_sink = NULL;
    request->completion_sink_context = NULL;
}

static void copy_metrics(
    const leir_aot_linux_ticket_t *ticket,
    leir_aot_linux_metrics_t *metrics) {
    const llam_linux_native_segment_t *segment =
        &ticket->segment;

    metrics->activations = segment->activations;
    metrics->logical_operations = segment->logical_operations;
    metrics->queue_publications = segment->queue_publications;
    metrics->prepared_sqes = segment->prepared_sqes;
    metrics->observed_cqes = segment->observed_cqes;
    metrics->suppressed_success_cqes =
        segment->suppressed_success_cqes;
    metrics->task_parks = segment->task_parks;
    metrics->terminal_wakes = segment->terminal_wakes;
    metrics->hot_allocations = segment->hot_allocations;
    metrics->first_error_operation =
        segment->first_error_index <= UINT32_MAX
            ? (uint32_t)segment->first_error_index
            : UINT32_MAX;
}

static void release_segment_ownership(
    llam_linux_native_segment_t *segment) {
    segment->req = NULL;
    segment->owner_node = NULL;
    segment->next = NULL;
    segment->batch = NULL;
    atomic_store_explicit(
        &segment->state,
        LLAM_LINUX_NATIVE_SEGMENT_IDLE,
        memory_order_release);
}

#endif

int leir_aot_linux_ticket_run(
    leir_aot_linux_ticket_t *ticket,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out,
    leir_aot_linux_metrics_t *metrics_out) {
    unsigned expected = LEIR_AOT_LINUX_TICKET_BOUND;

    if (!ticket_is_valid(ticket) || values_out == NULL ||
        resume_out == NULL || metrics_out == NULL ||
        value_count != ticket->module->slot_count) {
        return fail_with_errno(EINVAL);
    }
    if (!atomic_compare_exchange_strong_explicit(
            &ticket->state,
            &expected,
            LEIR_AOT_LINUX_TICKET_RUNNING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(
            expected == LEIR_AOT_LINUX_TICKET_DESTROYED
                ? EINVAL
                : EBUSY);
    }
    memset(resume_out, 0, sizeof(*resume_out));
    memset(metrics_out, 0, sizeof(*metrics_out));

#if LLAM_RUNTIME_BACKEND_LINUX
    {
        leir_aot_backend_v1_t backend = {
            .abi_version = LEIR_AOT_MODULE_ABI_V1,
            .struct_size = sizeof(backend),
            .backend_kind = LEIR_AOT_BACKEND_LINUX_IO_URING,
            .context = ticket,
            .prepare_connect_write = prepare_connect_write,
        };
        llam_task_t *task = g_llam_tls_task;
        llam_io_req_t *request = NULL;
        unsigned segment_state;
        uint32_t continuation;
        int64_t semantic_result;
        int issue_result;
        int saved_errno;
        int result = -1;

        ticket->prepared = false;
        if (task == NULL || g_llam_tls_shard == NULL ||
            task->owner_runtime == NULL ||
            g_llam_tls_shard->runtime != task->owner_runtime) {
            saved_errno = EINVAL;
            goto finish;
        }
        if (ticket->module->prepare(
                ticket->module_instance, &backend) != 0 ||
            !ticket->prepared) {
            saved_errno = errno != 0 ? errno : EPROTO;
            goto finish;
        }
        ticket->segment.owner_runtime = task->owner_runtime;
        request = llam_api_io_req_acquire(g_llam_tls_shard);
        if (request == NULL) {
            saved_errno = errno != 0 ? errno : ENOMEM;
            goto finish;
        }
        if (request != &task->embedded_io_req) {
            ticket->segment.hot_allocations += 1U;
            llam_api_io_req_release(g_llam_tls_shard, request);
            request = NULL;
            saved_errno = EAGAIN;
            goto finish;
        }
        prepare_request(request, &ticket->segment.ops[1]);
        issue_result = llam_issue_linux_native_segment(
            &ticket->segment, request);
        saved_errno = errno;
        segment_state = atomic_load_explicit(
            &ticket->segment.state, memory_order_acquire);
        if (segment_state != LLAM_LINUX_NATIVE_SEGMENT_IDLE &&
            segment_state != LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
            abort();
        }
        copy_metrics(ticket, metrics_out);

        if (ticket->segment.first_error_index <
                ticket->segment.op_count) {
            continuation =
                ticket->segment.first_error_index == 0U
                    ? ticket->connect_error_continuation
                    : ticket->write_continuation;
            semantic_result = ticket->segment.semantic_result;
        } else if (issue_result == 0 &&
                   segment_state ==
                       LLAM_LINUX_NATIVE_SEGMENT_RETIRED &&
                   ticket->segment.first_error == 0) {
            continuation = ticket->write_continuation;
            semantic_result = ticket->segment.semantic_result;
        } else {
            release_segment_ownership(&ticket->segment);
            llam_api_io_req_release(g_llam_tls_shard, request);
            request = NULL;
            goto finish;
        }

        metrics_out->resumed_continuation = continuation;
        release_segment_ownership(&ticket->segment);
        llam_api_io_req_release(g_llam_tls_shard, request);
        request = NULL;
        if (ticket->module->resume(
                ticket->module_instance,
                continuation,
                semantic_result,
                resume_out) != 0 ||
            ticket->module->copy_outputs(
                ticket->module_instance,
                values_out,
                value_count) != 0) {
            saved_errno = errno != 0 ? errno : EPROTO;
            goto finish;
        }
        result = 0;
        saved_errno = 0;

finish:
        if (request != NULL) {
            llam_api_io_req_release(g_llam_tls_shard, request);
        }
        ticket->prepared = false;
        atomic_store_explicit(
            &ticket->state,
            LEIR_AOT_LINUX_TICKET_CONSUMED,
            memory_order_release);
        errno = saved_errno;
        return result;
    }
#else
    atomic_store_explicit(
        &ticket->state,
        LEIR_AOT_LINUX_TICKET_CONSUMED,
        memory_order_release);
    return fail_with_errno(ENOTSUP);
#endif
}

int leir_aot_linux_ticket_destroy(
    leir_aot_linux_ticket_t *ticket) {
    unsigned state;

    if (!ticket_is_valid(ticket)) {
        return fail_with_errno(EINVAL);
    }
    state = atomic_load_explicit(
        &ticket->state, memory_order_acquire);
    for (;;) {
        if (state == LEIR_AOT_LINUX_TICKET_RUNNING ||
            state == LEIR_AOT_LINUX_TICKET_BINDING) {
            return fail_with_errno(EBUSY);
        }
        if (state == LEIR_AOT_LINUX_TICKET_DESTROYED) {
            return fail_with_errno(EINVAL);
        }
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state,
                &state,
                LEIR_AOT_LINUX_TICKET_DESTROYED,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }
    if (state == LEIR_AOT_LINUX_TICKET_BOUND) {
        ticket->module->cancel(ticket->module_instance);
    }
    ticket->prepared = false;
    ticket->module = NULL;
    ticket->module_instance = NULL;
    ticket->module_instance_size = 0U;
    ticket->magic = 0U;
    errno = 0;
    return 0;
}
