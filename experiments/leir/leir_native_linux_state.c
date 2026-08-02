// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_native_linux_state.c
 * @brief Owns lazy Linux bind state for backend-native LEIR segments.
 */

#include "leir_native_segment_internal.h"

#if LLAM_RUNTIME_BACKEND_LINUX

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static bool mode_is_fixed(leir_native_mode_t mode) {
    return mode == LEIR_NATIVE_MODE_FIXED_LINK ||
           mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
}

static void close_pinned_fds(
    leir_native_linux_state_t *state) {
    unsigned i;

    if (state == NULL) {
        return;
    }
    for (i = 0U; i < state->pinned_fd_count; i += 1U) {
        if (state->pinned_fds[i] >= 0) {
            (void)close((int)state->pinned_fds[i]);
            state->pinned_fds[i] = LLAM_INVALID_FD;
        }
    }
    state->pinned_fd_count = 0U;
}

static int validate_socket(llam_fd_t fd) {
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

static void free_fixed_buffer_array(
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

static void free_fixed_state(
    leir_native_linux_state_t *state) {
    leir_native_linux_fixed_state_t *fixed;

    if (state == NULL || state->fixed == NULL) {
        return;
    }
    fixed = state->fixed;
    free_fixed_buffer_array(
        fixed->buffers,
        fixed->buffer_count);
    memset(fixed, 0, sizeof(*fixed));
    free(fixed);
    state->fixed = NULL;
}

int leir_native_linux_state_detach_fixed(
    leir_native_linux_state_t *state) {
    leir_native_linux_fixed_state_t *fixed;
    llam_runtime_t *runtime;
    llam_node_t *node;
    unsigned segment_state;

    if (state == NULL || state->fixed == NULL) {
        return 0;
    }
    fixed = state->fixed;
    if (!fixed->lease.attached) {
        fixed->owner_runtime = NULL;
        fixed->owner_runtime_id = 0U;
        return 0;
    }
    segment_state = atomic_load_explicit(
        &state->segment.state, memory_order_acquire);
    if (segment_state != LLAM_LINUX_NATIVE_SEGMENT_IDLE &&
        segment_state != LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
        return fail_with_errno(EBUSY);
    }
    runtime = fixed->owner_runtime;
    if (runtime == NULL ||
        runtime->runtime_id !=
            fixed->owner_runtime_id ||
        runtime->nodes == NULL ||
        fixed->lease.node_index >=
            runtime->active_nodes) {
        memset(&fixed->lease, 0, sizeof(fixed->lease));
        fixed->owner_runtime = NULL;
        fixed->owner_runtime_id = 0U;
        return 0;
    }
    node = &runtime->nodes[fixed->lease.node_index];
    if (!node->native_resource_lock_initialized ||
        !node->ring_ready ||
        (!node->native_fixed_files_registered &&
         !node->native_fixed_buffers_registered)) {
        memset(&fixed->lease, 0, sizeof(fixed->lease));
        fixed->owner_runtime = NULL;
        fixed->owner_runtime_id = 0U;
        return 0;
    }
    if (llam_linux_native_resources_detach(
            node, &fixed->lease) != 0) {
        return -1;
    }
    fixed->owner_runtime = NULL;
    fixed->owner_runtime_id = 0U;
    return 0;
}

void leir_native_linux_state_free(
    leir_native_linux_state_t *state) {
    if (state == NULL) {
        return;
    }
    close_pinned_fds(state);
    free_fixed_state(state);
    memset(state, 0, sizeof(*state));
    free(state);
}

static int allocate_fixed_buffer(
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

int leir_native_linux_state_build(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values,
    leir_native_linux_state_t **state_out) {
    leir_native_linux_state_t *state;
    leir_native_linux_fixed_state_t *fixed_state = NULL;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_fd_t
        source_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    bool fixed = mode_is_fixed(instance->mode);
    unsigned i;

    if (state_out == NULL) {
        return fail_with_errno(EINVAL);
    }
    *state_out = NULL;
    state = calloc(1U, sizeof(*state));
    if (state == NULL) {
        return fail_with_errno(ENOMEM);
    }
    if (fixed) {
        fixed_state = calloc(1U, sizeof(*fixed_state));
        if (fixed_state == NULL) {
            free(state);
            return fail_with_errno(ENOMEM);
        }
        state->fixed = fixed_state;
    }
    memset(ops, 0, sizeof(ops));
    for (i = 0U;
         i < LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS;
         i += 1U) {
        source_fds[i] = LLAM_INVALID_FD;
        state->pinned_fds[i] = LLAM_INVALID_FD;
    }
    for (i = 0U; i < instance->plan.step_count; i += 1U) {
        const leir_native_step_t *step =
            &instance->plan.steps[i];
        llam_fd_t fd = values[step->fd_slot].fd;
        uint32_t length;
        unsigned pinned;
        unsigned buffer_index = 0U;

        if (leir_native_step_length(
                instance->program,
                step,
                values,
                &length) != 0) {
            goto fail;
        }
        for (pinned = 0U;
             pinned < state->pinned_fd_count;
             pinned += 1U) {
            if (source_fds[pinned] == fd) {
                break;
            }
        }
        if (pinned == state->pinned_fd_count) {
            int duplicate = fcntl(
                (int)fd, F_DUPFD_CLOEXEC, 0);

            if (duplicate < 0) {
                goto fail;
            }
            state->pinned_fds[state->pinned_fd_count] =
                (llam_fd_t)duplicate;
            source_fds[state->pinned_fd_count] = fd;
            pinned = state->pinned_fd_count;
            state->pinned_fd_count += 1U;
            if (validate_socket(
                    state->pinned_fds[pinned]) != 0) {
                goto fail;
            }
        }

        if (fixed) {
            for (buffer_index = 0U;
                 buffer_index < fixed_state->buffer_count;
                 buffer_index += 1U) {
                if (fixed_state->buffers[buffer_index].slot ==
                    step->buffer_slot) {
                    break;
                }
            }
            if (buffer_index == fixed_state->buffer_count) {
                if (allocate_fixed_buffer(
                        &fixed_state->buffers[
                            fixed_state->buffer_count],
                        step->buffer_slot,
                        values[step->buffer_slot]
                            .buffer.data,
                        values[step->buffer_slot]
                            .buffer.size) != 0) {
                    goto fail;
                }
                buffer_index = fixed_state->buffer_count;
                fixed_state->buffer_count += 1U;
            }
            if (step->kind == LEIR_NATIVE_STEP_RECV) {
                leir_native_fixed_buffer_t *buffer =
                    &fixed_state->buffers[buffer_index];

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
        if (fixed) {
            ops[i].flags = LLAM_LINUX_NATIVE_OP_FIXED_FILE;
            if (step->kind == LEIR_NATIVE_STEP_RECV) {
                ops[i].flags |=
                    LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER;
            }
        }
        ops[i].fd = state->pinned_fds[pinned];
        ops[i].buffer = fixed
            ? fixed_state->buffers[buffer_index].scratch
            : values[step->buffer_slot].buffer.data;
        ops[i].length = length;
        state->op_fd_indices[i] = (uint8_t)pinned;
        if (fixed) {
            fixed_state->op_buffer_indices[i] =
                (uint8_t)buffer_index;
        }
    }
    if (llam_linux_native_segment_configure(
            &state->segment,
            ops,
            instance->plan.step_count,
            linux_mode(instance->mode)) != 0) {
        goto fail;
    }
    *state_out = state;
    return 0;

fail:
    {
        int saved_errno = errno != 0 ? errno : EINVAL;

        leir_native_linux_state_free(state);
        errno = saved_errno;
    }
    return -1;
}

int leir_native_linux_state_attach_fixed(
    leir_native_instance_t *instance,
    llam_runtime_t *runtime,
    llam_node_t *node) {
    leir_native_linux_state_t *state =
        instance->linux_state;
    leir_native_linux_fixed_state_t *fixed_state =
        state != NULL ? state->fixed : NULL;
    struct iovec
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    int fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned i;

    if (!mode_is_fixed(instance->mode)) {
        return 0;
    }
    if (state == NULL || fixed_state == NULL) {
        return fail_with_errno(EPROTO);
    }
    if (fixed_state->lease.attached) {
        if (fixed_state->owner_runtime != runtime ||
            fixed_state->owner_runtime_id !=
                runtime->runtime_id ||
            fixed_state->lease.node_index !=
                node->index) {
            return fail_with_errno(EXDEV);
        }
    } else {
        if (!node->supports_native_fixed_files ||
            !node->supports_native_fixed_buffers ||
            state->pinned_fd_count == 0U ||
            fixed_state->buffer_count == 0U) {
            return fail_with_errno(ENOTSUP);
        }
        for (i = 0U;
             i < state->pinned_fd_count;
             i += 1U) {
            fds[i] = (int)state->pinned_fds[i];
        }
        for (i = 0U;
             i < fixed_state->buffer_count;
             i += 1U) {
            buffers[i].iov_base =
                fixed_state->buffers[i].scratch;
            buffers[i].iov_len =
                fixed_state->buffers[i]
                    .registered_size;
        }
        if (llam_linux_native_resources_attach(
                node,
                fds,
                state->pinned_fd_count,
                buffers,
                fixed_state->buffer_count,
                &fixed_state->lease) != 0) {
            return -1;
        }
        fixed_state->owner_runtime = runtime;
        fixed_state->owner_runtime_id =
            runtime->runtime_id;
    }

    for (i = 0U; i < state->segment.op_count; i += 1U) {
        llam_linux_native_op_t *op =
            &state->segment.ops[i];
        unsigned file_index =
            state->op_fd_indices[i];
        unsigned buffer_index =
            fixed_state->op_buffer_indices[i];

        if (file_index >=
                fixed_state->lease.file_count ||
            buffer_index >=
                fixed_state->lease.buffer_count) {
            return fail_with_errno(EPROTO);
        }
        op->flags = LLAM_LINUX_NATIVE_OP_FIXED_FILE;
        op->fixed_file_slot = (uint16_t)
            fixed_state->lease.file_slots[file_index];
        op->buffer =
            fixed_state->buffers[
                buffer_index].scratch;
        if (op->kind == LLAM_LINUX_NATIVE_OP_RECV) {
            op->flags |=
                LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER;
            op->fixed_buffer_slot = (uint16_t)
                fixed_state->lease
                    .buffer_slots[buffer_index];
        }
    }
    return 0;
}

void leir_native_linux_state_copy_fixed_inputs(
    leir_native_instance_t *instance) {
    leir_native_linux_fixed_state_t *fixed_state =
        instance->linux_state != NULL
            ? instance->linux_state->fixed
            : NULL;
    unsigned i;

    if (!mode_is_fixed(instance->mode) ||
        fixed_state == NULL) {
        return;
    }
    for (i = 0U;
         i < fixed_state->buffer_count;
         i += 1U) {
        leir_native_fixed_buffer_t *buffer =
            &fixed_state->buffers[i];

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

void leir_native_linux_state_copy_fixed_outputs(
    leir_native_instance_t *instance,
    bool activated) {
    leir_native_linux_state_t *state =
        instance->linux_state;
    leir_native_linux_fixed_state_t *fixed_state =
        state != NULL ? state->fixed : NULL;
    unsigned successful_operations;
    unsigned i;

    if (!activated || !mode_is_fixed(instance->mode) ||
        state == NULL || fixed_state == NULL) {
        return;
    }
    if (state->segment.first_error == 0) {
        successful_operations =
            state->segment.op_count;
    } else if (
        state->segment.first_error_index <
        state->segment.op_count) {
        successful_operations =
            state->segment.first_error_index;
    } else {
        successful_operations = 0U;
    }
    for (i = 0U;
         i < fixed_state->buffer_count;
         i += 1U) {
        leir_native_fixed_buffer_t *buffer =
            &fixed_state->buffers[i];

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

#else
typedef int leir_native_linux_state_translation_unit_is_not_applicable;
#endif
