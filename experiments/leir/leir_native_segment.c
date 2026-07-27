#include "leir_native_segment.h"

#include "leir_phase0_internal.h"
#include "io/runtime_io_api_internal.h"

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_segment_linux_internal.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_RUNTIME_BACKEND_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#endif

enum {
    LEIR_NATIVE_INSTANCE_IDLE = 0U,
    LEIR_NATIVE_INSTANCE_RUNNING = 1U,
    LEIR_NATIVE_INSTANCE_BINDING = 2U,
};

struct leir_native_instance {
    const leir_phase0_program_t *program;
    leir_native_plan_t plan;
    leir_phase0_value_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_native_mode_t mode;
    atomic_uint activity;
    atomic_uint bound;
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_linux_native_segment_t segment;
#endif
};

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
           mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP;
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

static int step_length(
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
static int validate_linux_socket(llam_fd_t fd) {
    struct sockaddr_storage address;
    socklen_t address_length;
    int socket_type;

    if (!llam_fd_get_socket_type(fd, &socket_type)) {
        return fail_with_errno(
            errno != 0 ? errno : ENOTSOCK);
    }
    if (socket_type != SOCK_SEQPACKET) {
        return fail_with_errno(EPROTOTYPE);
    }

    memset(&address, 0, sizeof(address));
    address_length = (socklen_t)sizeof(address);
    if (getsockname(
            fd,
            (struct sockaddr *)&address,
            &address_length) != 0) {
        return -1;
    }
    if (address.ss_family != AF_UNIX) {
        return fail_with_errno(EAFNOSUPPORT);
    }

    memset(&address, 0, sizeof(address));
    address_length = (socklen_t)sizeof(address);
    if (getpeername(
            fd,
            (struct sockaddr *)&address,
            &address_length) != 0) {
        return fail_with_errno(
            errno != 0 ? errno : ENOTCONN);
    }
    if (address.ss_family != AF_UNIX) {
        return fail_with_errno(EAFNOSUPPORT);
    }
    return 0;
}

static llam_linux_native_segment_mode_t linux_mode(
    leir_native_mode_t mode) {
    return mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP
        ? LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP
        : LLAM_LINUX_NATIVE_SEGMENT_LINK;
}

static int configure_linux_segment(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values) {
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_fd_t checked_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned checked_count = 0U;
    unsigned i;

    memset(ops, 0, sizeof(ops));
    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        const leir_native_step_t *step =
            &instance->plan.steps[i];
        llam_fd_t fd = values[step->fd_slot].fd;
        uint32_t length;
        unsigned checked;

        if (step_length(
                instance->program,
                step,
                values,
                &length) != 0) {
            return -1;
        }
        for (checked = 0U;
             checked < checked_count;
             checked += 1U) {
            if (checked_fds[checked] == fd) {
                break;
            }
        }
        if (checked == checked_count) {
            if (validate_linux_socket(fd) != 0) {
                return -1;
            }
            checked_fds[checked_count++] = fd;
        }

        ops[i].kind =
            step->kind == LEIR_NATIVE_STEP_RECV
                ? LLAM_LINUX_NATIVE_OP_RECV
                : LLAM_LINUX_NATIVE_OP_SEND;
        ops[i].result_slot = step->result_slot;
        ops[i].fd = fd;
        ops[i].buffer =
            values[step->buffer_slot].buffer.data;
        ops[i].length = length;
    }
    return llam_linux_native_segment_configure(
        &instance->segment,
        ops,
        instance->plan.step_count,
        linux_mode(instance->mode));
}

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
    unsigned successful_operations;
    unsigned i;

    if (success) {
        successful_operations = instance->plan.step_count;
    } else if (
        instance->segment.first_error_index <
        instance->plan.step_count) {
        successful_operations =
            instance->segment.first_error_index;
    } else {
        successful_operations = 0U;
    }

    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        const llam_linux_native_op_t *op =
            &instance->segment.ops[i];

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
    unsigned expected = LEIR_NATIVE_INSTANCE_IDLE;
    int result = -1;
    int saved_errno = EINVAL;
    unsigned i;

    if (instance == NULL ||
        instance->program == NULL ||
        values == NULL ||
        value_count != instance->program->slot_count) {
        return fail_with_errno(EINVAL);
    }
    if (!atomic_compare_exchange_strong_explicit(
            &instance->activity,
            &expected,
            LEIR_NATIVE_INSTANCE_BINDING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(EBUSY);
    }
    atomic_store_explicit(
        &instance->bound, 0U, memory_order_release);

    if (validate_slot_values(
            instance->program, values) != 0) {
        saved_errno = errno;
        goto done;
    }
    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        uint32_t ignored_length;

        if (step_length(
                instance->program,
                &instance->plan.steps[i],
                values,
                &ignored_length) != 0) {
            saved_errno = errno;
            goto done;
        }
    }
#if LLAM_RUNTIME_BACKEND_LINUX
    if (configure_linux_segment(instance, values) != 0) {
        saved_errno = errno;
        goto done;
    }
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

int leir_native_instance_run(
    leir_native_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_native_metrics_t *metrics_out) {
    unsigned expected = LEIR_NATIVE_INSTANCE_IDLE;

    if (instance == NULL ||
        instance->program == NULL ||
        values_out == NULL ||
        metrics_out == NULL ||
        value_count != instance->program->slot_count ||
        atomic_load_explicit(
            &instance->bound,
            memory_order_acquire) == 0U) {
        return fail_with_errno(EINVAL);
    }
    if (!atomic_compare_exchange_strong_explicit(
            &instance->activity,
            &expected,
            LEIR_NATIVE_INSTANCE_RUNNING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return fail_with_errno(EBUSY);
    }

#if LLAM_RUNTIME_BACKEND_LINUX
    {
        llam_task_t *task = g_llam_tls_task;
        llam_io_req_t *req;
        unsigned segment_state;
        uint64_t generation;
        int issue_result;
        int saved_errno;

        if (task == NULL ||
            g_llam_tls_shard == NULL ||
            task->owner_runtime == NULL ||
            g_llam_tls_shard->runtime !=
                task->owner_runtime) {
            atomic_store_explicit(
                &instance->activity,
                LEIR_NATIVE_INSTANCE_IDLE,
                memory_order_release);
            return fail_with_errno(EINVAL);
        }

        req = acquire_embedded_request(task);
        if (req == NULL) {
            saved_errno = errno != 0 ? errno : ENOMEM;
            atomic_store_explicit(
                &instance->activity,
                LEIR_NATIVE_INSTANCE_IDLE,
                memory_order_release);
            return fail_with_errno(saved_errno);
        }
        if (req != &task->embedded_io_req) {
            instance->segment.hot_allocations += 1U;
            llam_api_io_req_release(g_llam_tls_shard, req);
            atomic_store_explicit(
                &instance->activity,
                LEIR_NATIVE_INSTANCE_IDLE,
                memory_order_release);
            return fail_with_errno(EAGAIN);
        }

        prepare_request(
            req,
            &instance->segment.ops[
                instance->segment.op_count - 1U]);
        generation = instance->segment.generation;
        generation =
            generation == UINT64_MAX
                ? UINT64_C(1)
                : generation + UINT64_C(1);
        instance->segment.generation = generation;
        instance->segment.owner_runtime =
            task->owner_runtime;

        issue_result = llam_issue_linux_native_segment(
            &instance->segment, req);
        saved_errno = errno;
        segment_state = atomic_load_explicit(
            &instance->segment.state,
            memory_order_acquire);
        if (segment_state !=
                LLAM_LINUX_NATIVE_SEGMENT_TERMINAL &&
            segment_state !=
                LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
            /*
             * A returned call cannot leave kernel or queue ownership live.
             * Releasing either the request or this instance would otherwise
             * permit a backend use-after-free, so fail closed.
             */
            abort();
        }
        publish_results(
            instance,
            issue_result == 0);
        copy_linux_metrics(
            &instance->segment, metrics_out);
        memcpy(
            values_out,
            instance->slots,
            value_count * sizeof(values_out[0]));

        instance->segment.req = NULL;
        instance->segment.owner_node = NULL;
        instance->segment.next = NULL;
        atomic_store_explicit(
            &instance->segment.state,
            LLAM_LINUX_NATIVE_SEGMENT_IDLE,
            memory_order_release);
        llam_api_io_req_release(g_llam_tls_shard, req);
        atomic_store_explicit(
            &instance->activity,
            LEIR_NATIVE_INSTANCE_IDLE,
            memory_order_release);
        errno = saved_errno;
        return issue_result;
    }
#else
    memset(metrics_out, 0, sizeof(*metrics_out));
    metrics_out->first_error_operation = UINT16_MAX;
    memcpy(
        values_out,
        instance->slots,
        value_count * sizeof(values_out[0]));
    atomic_store_explicit(
        &instance->activity,
        LEIR_NATIVE_INSTANCE_IDLE,
        memory_order_release);
    return fail_with_errno(ENOTSUP);
#endif
}
