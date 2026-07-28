#include "leir_native_segment.h"

#include "leir_phase0_internal.h"
#include "io/runtime_io_api_internal.h"

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_segment_linux_internal.h"

_Static_assert(
    LEIR_NATIVE_MAX_BATCH_SEGMENTS ==
        LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS,
    "public and Linux native batch limits must match");
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_RUNTIME_BACKEND_LINUX
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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

#if LLAM_RUNTIME_BACKEND_LINUX
typedef struct leir_native_fixed_buffer {
    void *external;
    void *scratch;
    size_t logical_size;
    size_t registered_size;
    size_t recv_copy_size;
    unsigned first_recv_operation;
    uint16_t slot;
    bool recv_written;
} leir_native_fixed_buffer_t;
#endif

struct leir_native_instance {
    const leir_phase0_program_t *program;
    leir_native_plan_t plan;
    leir_phase0_value_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_native_mode_t mode;
    atomic_uint activity;
    atomic_uint bound;
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_linux_native_segment_t segment;
    llam_fd_t
        pinned_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned pinned_fd_count;
    leir_native_fixed_buffer_t
        fixed_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned fixed_buffer_count;
    uint8_t
        op_fd_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    uint8_t
        op_buffer_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_resource_lease_t fixed_lease;
    llam_runtime_t *fixed_owner_runtime;
    uint64_t fixed_owner_runtime_id;
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
           mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
}

#if LLAM_RUNTIME_BACKEND_LINUX
static bool mode_is_fixed(leir_native_mode_t mode) {
    return mode == LEIR_NATIVE_MODE_FIXED_LINK ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
}
#endif

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
static void close_linux_pinned_fds(
    leir_native_instance_t *instance) {
    unsigned i;

    for (i = 0U; i < instance->pinned_fd_count; i += 1U) {
        if (instance->pinned_fds[i] >= 0) {
            (void)close((int)instance->pinned_fds[i]);
            instance->pinned_fds[i] = LLAM_INVALID_FD;
        }
    }
    instance->pinned_fd_count = 0U;
}

static void close_linux_fd_array(
    llam_fd_t *fds,
    unsigned count) {
    int saved_errno = errno;
    unsigned i;

    for (i = 0U; i < count; i += 1U) {
        if (fds[i] >= 0) {
            (void)close((int)fds[i]);
            fds[i] = LLAM_INVALID_FD;
        }
    }
    errno = saved_errno;
}

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
    return mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP
        ? LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP
        : LLAM_LINUX_NATIVE_SEGMENT_LINK;
}

static void free_linux_fixed_buffer_array(
    leir_native_fixed_buffer_t *buffers,
    unsigned count) {
    int saved_errno = errno;
    unsigned i;

    for (i = 0U; i < count; i += 1U) {
        free(buffers[i].scratch);
        buffers[i].scratch = NULL;
    }
    errno = saved_errno;
}

static void free_linux_fixed_buffers(
    leir_native_instance_t *instance) {
    free_linux_fixed_buffer_array(
        instance->fixed_buffers,
        instance->fixed_buffer_count);
    memset(
        instance->fixed_buffers,
        0,
        sizeof(instance->fixed_buffers));
    instance->fixed_buffer_count = 0U;
}

static int detach_linux_fixed_lease(
    leir_native_instance_t *instance) {
    llam_runtime_t *runtime;
    llam_node_t *node;
    unsigned state;

    if (!instance->fixed_lease.attached) {
        instance->fixed_owner_runtime = NULL;
        instance->fixed_owner_runtime_id = 0U;
        return 0;
    }
    state = atomic_load_explicit(
        &instance->segment.state, memory_order_acquire);
    if (state != LLAM_LINUX_NATIVE_SEGMENT_IDLE &&
        state != LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
        return fail_with_errno(EBUSY);
    }
    runtime = instance->fixed_owner_runtime;
    if (runtime == NULL ||
        runtime->runtime_id !=
            instance->fixed_owner_runtime_id ||
        runtime->nodes == NULL ||
        instance->fixed_lease.node_index >=
            runtime->active_nodes) {
        memset(
            &instance->fixed_lease,
            0,
            sizeof(instance->fixed_lease));
        instance->fixed_owner_runtime = NULL;
        instance->fixed_owner_runtime_id = 0U;
        return 0;
    }
    node = &runtime->nodes[
        instance->fixed_lease.node_index];
    if (!node->native_resource_lock_initialized ||
        !node->ring_ready ||
        (!node->native_fixed_files_registered &&
         !node->native_fixed_buffers_registered)) {
        memset(
            &instance->fixed_lease,
            0,
            sizeof(instance->fixed_lease));
        instance->fixed_owner_runtime = NULL;
        instance->fixed_owner_runtime_id = 0U;
        return 0;
    }
    if (llam_linux_native_resources_detach(
            node, &instance->fixed_lease) != 0) {
        return -1;
    }
    instance->fixed_owner_runtime = NULL;
    instance->fixed_owner_runtime_id = 0U;
    return 0;
}

static int allocate_linux_fixed_buffer(
    leir_native_fixed_buffer_t *buffer,
    uint16_t slot,
    void *external,
    size_t logical_size) {
    long page_value = sysconf(_SC_PAGESIZE);
    size_t page_size =
        page_value > 0 ? (size_t)page_value : 4096U;
    size_t rounded_size;
    void *scratch = NULL;
    int result;

    if (logical_size == 0U ||
        logical_size > SIZE_MAX - (page_size - 1U)) {
        return fail_with_errno(EOVERFLOW);
    }
    rounded_size =
        ((logical_size + page_size - 1U) / page_size) *
        page_size;
    result = posix_memalign(
        &scratch, page_size, rounded_size);
    if (result != 0) {
        return fail_with_errno(result);
    }
    memset(scratch, 0, rounded_size);
    memset(buffer, 0, sizeof(*buffer));
    buffer->external = external;
    buffer->scratch = scratch;
    buffer->logical_size = logical_size;
    buffer->registered_size = rounded_size;
    buffer->first_recv_operation = UINT_MAX;
    buffer->slot = slot;
    return 0;
}

static int configure_linux_segment(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values) {
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_fd_t
        source_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_fd_t
        pinned_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    leir_native_fixed_buffer_t
        fixed_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    uint8_t
        op_fd_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    uint8_t
        op_buffer_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned pinned_count = 0U;
    unsigned fixed_buffer_count = 0U;
    bool fixed = mode_is_fixed(instance->mode);
    unsigned i;

    memset(ops, 0, sizeof(ops));
    memset(fixed_buffers, 0, sizeof(fixed_buffers));
    memset(op_fd_indices, 0, sizeof(op_fd_indices));
    memset(op_buffer_indices, 0, sizeof(op_buffer_indices));
    for (i = 0U;
         i < LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS;
         i += 1U) {
        source_fds[i] = LLAM_INVALID_FD;
        pinned_fds[i] = LLAM_INVALID_FD;
    }
    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        const leir_native_step_t *step =
            &instance->plan.steps[i];
        llam_fd_t fd = values[step->fd_slot].fd;
        uint32_t length;
        unsigned pinned;
        unsigned buffer_index = 0U;

        if (step_length(
                instance->program,
                step,
                values,
                &length) != 0) {
            goto fail;
        }
        for (pinned = 0U;
             pinned < pinned_count;
             pinned += 1U) {
            if (source_fds[pinned] == fd) {
                break;
            }
        }
        if (pinned == pinned_count) {
            int duplicate = fcntl(
                (int)fd, F_DUPFD_CLOEXEC, 0);

            if (duplicate < 0) {
                goto fail;
            }
            pinned_fds[pinned_count] =
                (llam_fd_t)duplicate;
            source_fds[pinned_count] = fd;
            pinned = pinned_count;
            pinned_count += 1U;
            if (validate_linux_socket(
                    pinned_fds[pinned]) != 0) {
                goto fail;
            }
        }

        if (fixed) {
            for (buffer_index = 0U;
                 buffer_index < fixed_buffer_count;
                 buffer_index += 1U) {
                if (fixed_buffers[buffer_index].slot ==
                    step->buffer_slot) {
                    break;
                }
            }
            if (buffer_index == fixed_buffer_count) {
                if (allocate_linux_fixed_buffer(
                        &fixed_buffers[
                            fixed_buffer_count],
                        step->buffer_slot,
                        values[step->buffer_slot]
                            .buffer.data,
                        values[step->buffer_slot]
                            .buffer.size) != 0) {
                    goto fail;
                }
                buffer_index = fixed_buffer_count;
                fixed_buffer_count += 1U;
            }
            if (step->kind == LEIR_NATIVE_STEP_RECV) {
                leir_native_fixed_buffer_t *buffer =
                    &fixed_buffers[buffer_index];

                buffer->recv_written = true;
                if (buffer->first_recv_operation == UINT_MAX) {
                    buffer->first_recv_operation = i;
                }
                if (length > buffer->recv_copy_size) {
                    buffer->recv_copy_size = length;
                }
            }
        }

        ops[i].kind =
            step->kind == LEIR_NATIVE_STEP_RECV
                ? LLAM_LINUX_NATIVE_OP_RECV
                : LLAM_LINUX_NATIVE_OP_SEND;
        ops[i].result_slot = step->result_slot;
        ops[i].fd = pinned_fds[pinned];
        ops[i].buffer = fixed
            ? fixed_buffers[buffer_index].scratch
            : values[step->buffer_slot].buffer.data;
        ops[i].length = length;
        op_fd_indices[i] = (uint8_t)pinned;
        op_buffer_indices[i] =
            (uint8_t)buffer_index;
    }
    if (detach_linux_fixed_lease(instance) != 0) {
        goto fail;
    }
    if (llam_linux_native_segment_configure(
            &instance->segment,
            ops,
            instance->plan.step_count,
            linux_mode(instance->mode)) != 0) {
        goto fail;
    }
    close_linux_pinned_fds(instance);
    free_linux_fixed_buffers(instance);
    memcpy(
        instance->pinned_fds,
        pinned_fds,
        pinned_count * sizeof(pinned_fds[0]));
    memcpy(
        instance->fixed_buffers,
        fixed_buffers,
        fixed_buffer_count * sizeof(fixed_buffers[0]));
    memcpy(
        instance->op_fd_indices,
        op_fd_indices,
        sizeof(op_fd_indices));
    memcpy(
        instance->op_buffer_indices,
        op_buffer_indices,
        sizeof(op_buffer_indices));
    instance->pinned_fd_count = pinned_count;
    instance->fixed_buffer_count = fixed_buffer_count;
    return 0;

fail:
    close_linux_fd_array(pinned_fds, pinned_count);
    free_linux_fixed_buffer_array(
        fixed_buffers, fixed_buffer_count);
    return -1;
}

static int attach_linux_fixed_instance(
    leir_native_instance_t *instance,
    llam_runtime_t *runtime,
    llam_node_t *node) {
    struct iovec
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    int fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned i;

    if (!mode_is_fixed(instance->mode)) {
        return 0;
    }
    if (instance->fixed_lease.attached) {
        if (instance->fixed_owner_runtime != runtime ||
            instance->fixed_owner_runtime_id !=
                runtime->runtime_id ||
            instance->fixed_lease.node_index !=
                node->index) {
            return fail_with_errno(EXDEV);
        }
    } else {
        if (!node->supports_native_fixed_files ||
            !node->supports_native_fixed_buffers ||
            instance->pinned_fd_count == 0U ||
            instance->fixed_buffer_count == 0U) {
            return fail_with_errno(ENOTSUP);
        }
        for (i = 0U;
             i < instance->pinned_fd_count;
             i += 1U) {
            fds[i] = (int)instance->pinned_fds[i];
        }
        for (i = 0U;
             i < instance->fixed_buffer_count;
             i += 1U) {
            buffers[i].iov_base =
                instance->fixed_buffers[i].scratch;
            buffers[i].iov_len =
                instance->fixed_buffers[i]
                    .registered_size;
        }
        if (llam_linux_native_resources_attach(
                node,
                fds,
                instance->pinned_fd_count,
                buffers,
                instance->fixed_buffer_count,
                &instance->fixed_lease) != 0) {
            return -1;
        }
        instance->fixed_owner_runtime = runtime;
        instance->fixed_owner_runtime_id =
            runtime->runtime_id;
    }

    for (i = 0U; i < instance->segment.op_count; i += 1U) {
        llam_linux_native_op_t *op =
            &instance->segment.ops[i];
        unsigned file_index =
            instance->op_fd_indices[i];
        unsigned buffer_index =
            instance->op_buffer_indices[i];

        if (file_index >=
                instance->fixed_lease.file_count ||
            buffer_index >=
                instance->fixed_lease.buffer_count) {
            return fail_with_errno(EPROTO);
        }
        op->flags = LLAM_LINUX_NATIVE_OP_FIXED_FILE;
        op->fixed_file_slot = (uint16_t)
            instance->fixed_lease.file_slots[file_index];
        op->buffer =
            instance->fixed_buffers[
                buffer_index].scratch;
        if (op->kind == LLAM_LINUX_NATIVE_OP_RECV) {
            op->flags |=
                LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER;
            op->fixed_buffer_slot = (uint16_t)
                instance->fixed_lease
                    .buffer_slots[buffer_index];
        }
    }
    return 0;
}

static void copy_linux_fixed_inputs(
    leir_native_instance_t *instance) {
    unsigned i;

    if (!mode_is_fixed(instance->mode)) {
        return;
    }
    for (i = 0U;
         i < instance->fixed_buffer_count;
         i += 1U) {
        leir_native_fixed_buffer_t *buffer =
            &instance->fixed_buffers[i];

        /*
         * A fixed buffer outlives one activation. Snapshot the caller's
         * current buffer every time so a short or failed receive can only
         * expose bytes from this activation, matching direct-buffer
         * untouched-byte semantics.
         */
        memcpy(
            buffer->scratch,
            buffer->external,
            buffer->logical_size);
    }
}

static void copy_linux_fixed_outputs(
    leir_native_instance_t *instance,
    bool activated) {
    unsigned successful_operations;
    unsigned i;

    if (!activated || !mode_is_fixed(instance->mode)) {
        return;
    }
    if (instance->segment.first_error == 0) {
        successful_operations =
            instance->segment.op_count;
    } else if (
        instance->segment.first_error_index <
        instance->segment.op_count) {
        successful_operations =
            instance->segment.first_error_index;
    } else {
        successful_operations = 0U;
    }
    for (i = 0U;
         i < instance->fixed_buffer_count;
         i += 1U) {
        leir_native_fixed_buffer_t *buffer =
            &instance->fixed_buffers[i];

        if (buffer->recv_written &&
            buffer->first_recv_operation <
                successful_operations) {
            memcpy(
                buffer->external,
                buffer->scratch,
                buffer->recv_copy_size);
        }
    }
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
#if LLAM_RUNTIME_BACKEND_LINUX
    {
        unsigned i;

        for (i = 0U;
             i < LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS;
             i += 1U) {
            instance->pinned_fds[i] = LLAM_INVALID_FD;
        }
    }
#endif
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

        if (step_length(
                program,
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
    if (detach_linux_fixed_lease(instance) != 0) {
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
    close_linux_pinned_fds(instance);
    free_linux_fixed_buffers(instance);
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
            instance->mode != mode) {
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
            if (attach_linux_fixed_instance(
                    instances[i], runtime, node) != 0) {
                saved_errno =
                    errno != 0 ? errno : ENOTSUP;
                release_running_instances(
                    instances, instance_count);
                return fail_with_errno(saved_errno);
            }
            copy_linux_fixed_inputs(instances[i]);
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
            uint64_t generation;

            copy_linux_metrics(
                &instances[i]->segment, &before[i]);
            generation =
                instances[i]->segment.generation;
            generation =
                generation == UINT64_MAX
                    ? UINT64_C(1)
                    : generation + UINT64_C(1);
            instances[i]->segment.generation =
                generation;
            instances[i]->segment.owner_runtime =
                task->owner_runtime;
            batch.segments[i] =
                &instances[i]->segment;
        }
        req = acquire_embedded_request(task);
        if (req == NULL) {
            saved_errno = errno != 0 ? errno : ENOMEM;
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(saved_errno);
        }
        if (req != &task->embedded_io_req) {
            instances[0]->segment.hot_allocations += 1U;
            batch_metrics_out->hot_allocations = 1U;
            llam_api_io_req_release(g_llam_tls_shard, req);
            release_running_instances(
                instances, instance_count);
            return fail_with_errno(EAGAIN);
        }

        prepare_request(
            req,
            &instances[instance_count - 1U]
                 ->segment.ops[
                     instances[instance_count - 1U]
                         ->segment.op_count -
                     1U]);
        issue_result = llam_issue_linux_native_batch(
            &batch, req);
        saved_errno = errno;
        for (i = 0U; i < instance_count; i += 1U) {
            llam_linux_native_segment_t *segment =
                &instances[i]->segment;
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
            copy_linux_fixed_outputs(
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
                &instances[i]->segment;

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
