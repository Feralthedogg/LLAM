/**
 * @file src/io/linux/watch/linux_segment.c
 * @brief io_uring encoding and CQE reduction for compiled LEIR segments.
 *
 * @details
 * This file contains only value validation, SQE preparation, and completion
 * state reduction.  Queue ownership and task wakeup are layered on top so the
 * semantic core can be tested without creating a kernel ring.
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

#include "io/linux/runtime_io_segment_linux_internal.h"

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
        if (!llam_linux_native_kind_is_valid(ops[i].kind) ||
            ops[i].fd < 0 ||
            ops[i].buffer == NULL ||
            ops[i].length == 0U ||
            (i > 0U && ops[i].kind == ops[i - 1U].kind)) {
            errno = EINVAL;
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
    }
    atomic_init(&segment->state, LLAM_LINUX_NATIVE_SEGMENT_IDLE);
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
    if (op->kind == LLAM_LINUX_NATIVE_OP_RECV) {
        io_uring_prep_recv(
            sqe,
            op->fd,
            op->buffer,
            op->length,
            0);
    } else {
        io_uring_prep_send(
            sqe,
            op->fd,
            op->buffer,
            op->length,
            MSG_NOSIGNAL);
    }

    if (index + 1U < segment->op_count) {
        flags = IOSQE_IO_LINK;
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

static int llam_linux_native_result_error(
    const llam_linux_native_op_t *op,
    int result) {
    if (result < 0) {
        if (result == INT_MIN) {
            return EIO;
        }
        return -result;
    }
    if ((uint32_t)result != op->length) {
        return EMSGSIZE;
    }
    return 0;
}

static void llam_linux_native_record_error(
    llam_linux_native_segment_t *segment,
    unsigned index,
    int error) {
    if (error == 0) {
        return;
    }
    if (segment->first_error == 0 ||
        (segment->first_error == ECANCELED &&
         error != ECANCELED)) {
        segment->first_error = error;
        segment->first_error_index = index;
    }
}

static llam_linux_native_cqe_action_t
llam_linux_native_claim_terminal(
    llam_linux_native_segment_t *segment,
    int error,
    int success_result,
    int *terminal_result_out) {
    unsigned expected = 0U;

    if (!atomic_compare_exchange_strong_explicit(
            &segment->terminal_claimed,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }
    atomic_store_explicit(
        &segment->state,
        LLAM_LINUX_NATIVE_SEGMENT_TERMINAL,
        memory_order_release);
    *terminal_result_out =
        error != 0 ? -error : success_result;
    return error != 0
        ? LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR
        : LLAM_LINUX_NATIVE_CQE_COMPLETE_OK;
}

llam_linux_native_cqe_action_t
llam_linux_native_segment_apply_cqe(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_token_t *token,
    int result,
    int *terminal_result_out) {
    unsigned index;
    int error;

    if (segment == NULL ||
        token == NULL ||
        terminal_result_out == NULL ||
        token->owner != segment ||
        token->generation == 0U ||
        token->generation != segment->generation ||
        token->operation_index >= segment->op_count ||
        atomic_load_explicit(
            &segment->state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT ||
        atomic_load_explicit(
            &segment->terminal_claimed,
            memory_order_acquire) != 0U) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }

    index = token->operation_index;
    if (segment->mode == LLAM_LINUX_NATIVE_SEGMENT_LINK) {
        if (index != segment->completed_cqes) {
            return LLAM_LINUX_NATIVE_CQE_FATAL;
        }
    } else {
        /*
         * Successful non-final CQEs are suppressed in this mode.  The only
         * valid visible intermediate completion is therefore an error.
         */
        if (segment->observed_cqes != 0U ||
            (index + 1U < segment->op_count &&
             result >= 0 &&
             (uint32_t)result == segment->ops[index].length)) {
            return LLAM_LINUX_NATIVE_CQE_FATAL;
        }
    }

    error = llam_linux_native_result_error(
        &segment->ops[index], result);
    segment->observed_cqes += 1U;
    llam_linux_native_record_error(segment, index, error);

    if (segment->mode ==
        LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
        segment->suppressed_success_cqes = index;
        if (index + 1U < segment->op_count && error == 0) {
            return LLAM_LINUX_NATIVE_CQE_FATAL;
        }
        return llam_linux_native_claim_terminal(
            segment,
            error,
            result,
            terminal_result_out);
    }

    segment->completed_cqes += 1U;
    if (index + 1U < segment->op_count) {
        return LLAM_LINUX_NATIVE_CQE_CONTINUE;
    }
    return llam_linux_native_claim_terminal(
        segment,
        segment->first_error,
        result,
        terminal_result_out);
}
