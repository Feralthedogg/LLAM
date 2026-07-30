/**
 * @file src/io/linux/watch/linux_segment.c
 * @brief Native compiled segment validation and SQE encoding.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "io/linux/runtime_io_watch_linux_internal.h"

_Static_assert(
    _Alignof(llam_linux_native_token_t) >= 8U,
    "native segment tokens must keep three tag bits clear");
_Static_assert(
    sizeof(llam_linux_native_token_t) % 8U == 0U,
    "native segment token stride must preserve alignment");

static bool llam_linux_native_count_is_valid(unsigned op_count) {
    return op_count == 1U ||
           op_count == 2U ||
           op_count == 4U ||
           op_count == 8U;
}

static bool llam_linux_native_kind_is_valid(uint16_t kind) {
    return kind == LLAM_LINUX_NATIVE_OP_RECV ||
           kind == LLAM_LINUX_NATIVE_OP_SEND;
}

static bool llam_linux_native_mode_is_valid(
    llam_linux_native_segment_mode_t mode) {
    return mode == LLAM_LINUX_NATIVE_SEGMENT_LINK ||
           mode == LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP;
}

int llam_linux_native_segment_configure(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_op_t *ops,
    unsigned op_count,
    llam_linux_native_segment_mode_t mode) {
    llam_linux_native_op_t
        copied[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned i;

    if (segment == NULL ||
        ops == NULL ||
        !llam_linux_native_count_is_valid(op_count) ||
        !llam_linux_native_mode_is_valid(mode)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < op_count; i += 1U) {
        unsigned flags = ops[i].flags;

        if (!llam_linux_native_kind_is_valid(ops[i].kind) ||
            (flags &
             ~(LLAM_LINUX_NATIVE_OP_FIXED_FILE |
               LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER)) !=
                0U ||
            ((flags & LLAM_LINUX_NATIVE_OP_FIXED_FILE) ==
                 0U &&
             ops[i].fd < 0) ||
            ((flags & LLAM_LINUX_NATIVE_OP_FIXED_FILE) !=
                 0U &&
             ops[i].fixed_file_slot >=
                 LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS) ||
            ((flags &
              LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER) !=
                 0U &&
             (ops[i].kind != LLAM_LINUX_NATIVE_OP_RECV ||
              (flags & LLAM_LINUX_NATIVE_OP_FIXED_FILE) ==
                  0U ||
              ops[i].fixed_buffer_slot >=
                  LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS)) ||
            ops[i].buffer == NULL ||
            ops[i].length == 0U) {
            errno = EINVAL;
            return -1;
        }
        if (ops[i].kind == LLAM_LINUX_NATIVE_OP_RECV &&
            i + 1U < op_count &&
            (flags &
             LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER) == 0U) {
            /*
             * A positive short RECV is soft-link success, so the
             * kernel may execute a successor before user space can
             * enforce READ_EXACT. A direct-buffer continuation needs
             * a semantic barrier and is not one native segment.
             */
            errno = ENOTSUP;
            return -1;
        }
    }

    memcpy(copied, ops, op_count * sizeof(ops[0]));
    memset(segment, 0, sizeof(*segment));
    memcpy(segment->ops, copied, op_count * sizeof(copied[0]));
    segment->op_count = op_count;
    segment->first_error_index = UINT_MAX;
    segment->mode = mode;
    for (i = 0U; i < op_count; i += 1U) {
        segment->tokens[i].owner = segment;
        segment->tokens[i].operation_index = (uint16_t)i;
        segment->cancel_tokens[i].owner = segment;
        segment->cancel_tokens[i].operation_index =
            (uint16_t)i;
    }
    atomic_init(&segment->state, LLAM_LINUX_NATIVE_SEGMENT_IDLE);
    atomic_init(&segment->semantic_claimed, 0U);
    atomic_init(&segment->target_retired, 0U);
    atomic_init(&segment->terminal_claimed, 0U);
    return 0;
}

void llam_linux_native_segment_prepare_sqe(
    const llam_linux_native_segment_t *segment,
    unsigned index,
    struct io_uring_sqe *sqe) {
    const llam_linux_native_op_t *op;
    unsigned flags = 0U;

    if (segment == NULL ||
        sqe == NULL ||
        index >= segment->op_count) {
        return;
    }
    op = &segment->ops[index];
    if ((op->flags &
         LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER) != 0U) {
        io_uring_prep_read_fixed(
            sqe,
            (int)op->fixed_file_slot,
            op->buffer,
            op->length,
            UINT64_MAX,
            (int)op->fixed_buffer_slot);
    } else if (op->kind == LLAM_LINUX_NATIVE_OP_RECV) {
        io_uring_prep_recv(
            sqe,
            (op->flags &
             LLAM_LINUX_NATIVE_OP_FIXED_FILE) != 0U
                ? (int)op->fixed_file_slot
                : op->fd,
            op->buffer,
            op->length,
            0);
    } else {
        io_uring_prep_send(
            sqe,
            (op->flags &
             LLAM_LINUX_NATIVE_OP_FIXED_FILE) != 0U
                ? (int)op->fixed_file_slot
                : op->fd,
            op->buffer,
            op->length,
            MSG_NOSIGNAL);
    }

    if ((op->flags &
         LLAM_LINUX_NATIVE_OP_FIXED_FILE) != 0U) {
        flags |= IOSQE_FIXED_FILE;
    }
    if (index + 1U < segment->op_count) {
        flags |= IOSQE_IO_LINK;
        if (segment->mode ==
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
            flags |= IOSQE_CQE_SKIP_SUCCESS;
        }
    }
    io_uring_sqe_set_flags(sqe, flags);
    io_uring_sqe_set_data64(
        sqe,
        llam_io_udata_encode(
            (void *)&segment->tokens[index],
            LLAM_IO_UDATA_NATIVE_SEGMENT));
}
