/**
 * @file src/io/linux/watch/linux_segment_reducer.c
 * @brief Pure CQE reduction for native compiled segments.
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
    bool current_is_cancel;
    bool error_is_cancel;

    if (error == 0) {
        return;
    }
    current_is_cancel = segment->first_error == ECANCELED;
    error_is_cancel = error == ECANCELED;
    if (segment->first_error == 0 ||
        (current_is_cancel && !error_is_cancel) ||
        (current_is_cancel == error_is_cancel &&
         index < segment->first_error_index)) {
        segment->first_error = error;
        segment->first_error_index = index;
    }
}

static unsigned llam_linux_native_missing_cqes(
    uint64_t observed_mask) {
    unsigned highest = 0U;
    unsigned missing = 0U;
    unsigned i;

    if (observed_mask == 0U) {
        return 0U;
    }
    for (i = 0U; i < LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS; i += 1U) {
        if ((observed_mask & (UINT64_C(1) << i)) != 0U) {
            highest = i;
        }
    }
    for (i = 0U; i < highest; i += 1U) {
        if ((observed_mask & (UINT64_C(1) << i)) == 0U) {
            missing += 1U;
        }
    }
    return missing;
}

llam_linux_native_cqe_action_t
llam_linux_native_segment_apply_cqe(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_token_t *token,
    int result,
    int *terminal_result_out) {
    llam_linux_native_cqe_action_t action =
        LLAM_LINUX_NATIVE_CQE_CONTINUE;
    uint64_t bit;
    uint64_t old_mask;
    unsigned old_missing = 0U;
    unsigned new_missing = 0U;
    unsigned state;
    unsigned index;
    int error;

    if (segment == NULL ||
        token == NULL ||
        terminal_result_out == NULL ||
        token->owner != segment ||
        token->generation == 0U ||
        token->generation != segment->generation ||
        token->operation_index >= segment->op_count) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }

    state = atomic_load_explicit(
        &segment->state, memory_order_acquire);
    if (state != LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT &&
        state != LLAM_LINUX_NATIVE_SEGMENT_RETIRING) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }
    index = token->operation_index;
    bit = UINT64_C(1) << index;
    old_mask = segment->observed_operation_mask;
    if ((old_mask & bit) != 0U) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }
    if (segment->mode ==
        LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
        old_missing =
            llam_linux_native_missing_cqes(old_mask);
    }
    segment->observed_operation_mask = old_mask | bit;
    if (segment->mode ==
        LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
        new_missing = llam_linux_native_missing_cqes(
            segment->observed_operation_mask);
        if (new_missing >= old_missing) {
            segment->suppressed_success_cqes +=
                new_missing - old_missing;
        } else {
            segment->suppressed_success_cqes -=
                old_missing - new_missing;
        }
    }

    error = llam_linux_native_result_error(
        &segment->ops[index], result);
    segment->observed_cqes += 1U;
    segment->completed_cqes += 1U;
    llam_linux_native_record_error(segment, index, error);

    if (error != 0) {
        unsigned was_claimed = atomic_exchange_explicit(
            &segment->semantic_claimed,
            1U,
            memory_order_acq_rel);

        segment->semantic_result = -segment->first_error;
        *terminal_result_out = segment->semantic_result;
        if (was_claimed == 0U) {
            atomic_store_explicit(
                &segment->state,
                LLAM_LINUX_NATIVE_SEGMENT_RETIRING,
                memory_order_release);
            action = LLAM_LINUX_NATIVE_CQE_SEMANTIC;
        }
    }

    if (error != 0 &&
        segment->mode ==
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP &&
        segment->atomic_submission &&
        index + 1U < segment->op_count) {
        unsigned expected = 0U;

        /*
         * For a soft-linked chain, Linux omits every later CQE when
         * a CQE_SKIP_SUCCESS request fails. The error CQE therefore
         * proves that the remaining targets have been retired by the
         * kernel; waiting for the tail would wait forever.
         */
        if (!atomic_compare_exchange_strong_explicit(
                &segment->terminal_claimed,
                &expected,
                1U,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return LLAM_LINUX_NATIVE_CQE_FATAL;
        }
        atomic_store_explicit(
            &segment->target_retired,
            1U,
            memory_order_release);
        atomic_store_explicit(
            &segment->state,
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED,
            memory_order_release);
        return LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR;
    }

    if (index + 1U == segment->op_count) {
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
            &segment->target_retired,
            1U,
            memory_order_release);
        if (atomic_exchange_explicit(
                &segment->semantic_claimed,
                1U,
                memory_order_acq_rel) == 0U) {
            segment->semantic_result =
                segment->first_error != 0
                    ? -segment->first_error
                    : result;
        } else if (segment->first_error != 0) {
            segment->semantic_result =
                -segment->first_error;
        }
        *terminal_result_out = segment->semantic_result;
        atomic_store_explicit(
            &segment->state,
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED,
            memory_order_release);
        return segment->first_error != 0
            ? LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR
            : LLAM_LINUX_NATIVE_CQE_RETIRED_OK;
    }
    return action;
}
