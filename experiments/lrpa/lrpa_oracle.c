/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

#include <string.h>

static lrpa_status_t
oracle_failure(lrpa_context_t *context, lrpa_failure_t *failure,
               lrpa_oracle_id_t oracle, uint64_t expected, uint64_t actual,
               uint32_t cell_index)
{
    lrpa_completion_cell_t *cell = &context->cells[cell_index];

    memset(failure, 0, sizeof(*failure));
    failure->oracle = oracle;
    failure->expected = expected;
    failure->actual = actual;
    failure->generation_first =
        atomic_load_explicit(&cell->generation, memory_order_acquire);
    failure->generation_second = failure->generation_first;
    failure->object_id = cell->object_id;
    failure->timestamp_ns = lrpa_platform_monotonic_ns();
    failure->debug_address = (uintptr_t)cell;
    return LRPA_STATUS_ORACLE_FAILURE;
}

lrpa_status_t
lrpa_select_verify_round(lrpa_context_t *context, uint32_t round,
                         lrpa_failure_t *failure, lrpa_result_t *result)
{
    uint64_t round_winners = 0U;
    uint64_t round_discards = 0U;
    uint64_t lane_winners = 0U;
    uint64_t lane_terminals = 0U;
    uint32_t cell_index;
    uint32_t lane;

    if (context == NULL || failure == NULL || result == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    for (cell_index = 0U; cell_index < context->active_cell_count;
         ++cell_index) {
        lrpa_completion_cell_t *cell = &context->cells[cell_index];
        const unsigned int winners = atomic_load_explicit(
            &cell->winner_count, memory_order_acquire);
        const unsigned int terminals = atomic_load_explicit(
            &cell->terminal_count, memory_order_acquire);
        const unsigned int outcome = atomic_load_explicit(
            &cell->outcome, memory_order_acquire);
        const unsigned int payload = atomic_load_explicit(
            &cell->payload_visible, memory_order_acquire);
        const unsigned int live_nodes = atomic_load_explicit(
            &cell->live_nodes, memory_order_acquire);
        const unsigned int stale = atomic_load_explicit(
            &cell->stale_completion, memory_order_acquire);

        if (winners != 1U || terminals != 1U) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_EXACTLY_ONE_WINNER, 1U,
                                  winners, cell_index);
        }
        if (outcome <= LRPA_OUTCOME_NONE || outcome >= LRPA_OUTCOME_COUNT ||
            (context->manifest.allowed_outcomes & (1U << outcome)) == 0U) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_ALLOWED_OUTCOME,
                                  context->manifest.allowed_outcomes, outcome,
                                  cell_index);
        }
        if (payload != (outcome == LRPA_OUTCOME_SEND ? 1U : 0U)) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_PAYLOAD_OWNERSHIP,
                                  outcome == LRPA_OUTCOME_SEND ? 1U : 0U,
                                  payload, cell_index);
        }
        if (live_nodes != 0U) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_LIVE_NODE_DRAIN, 0U,
                                  live_nodes, cell_index);
        }
        if (stale != 0U) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_STALE_GENERATION, 0U, stale,
                                  cell_index);
        }
        round_winners += winners;
        round_discards += atomic_load_explicit(&cell->discard_count,
                                               memory_order_acquire);
        if (outcome == LRPA_OUTCOME_CANCEL) {
            result->cancel_total += 1U;
        } else if (outcome == LRPA_OUTCOME_TIMEOUT) {
            result->timeout_total += 1U;
        }
    }
    for (lane = 0U; lane < context->manifest.lane_count; ++lane) {
        if (atomic_load_explicit(&context->lanes[lane].phase,
                                 memory_order_acquire) != LRPA_LANE_RACING) {
            return oracle_failure(context, failure,
                                  LRPA_ORACLE_GLOBAL_ACCOUNTING,
                                  LRPA_LANE_RACING,
                                  atomic_load_explicit(
                                      &context->lanes[lane].phase,
                                      memory_order_relaxed),
                                  0U);
        }
        lane_winners += atomic_load_explicit(
            &context->lanes[lane].winner_count, memory_order_acquire);
        lane_terminals += atomic_load_explicit(
            &context->lanes[lane].terminal_count, memory_order_acquire);
    }
    if (lane_winners != (uint64_t)(round + 1U) *
                            context->active_cell_count ||
        lane_terminals != lane_winners ||
        round_winners != context->active_cell_count) {
        return oracle_failure(context, failure,
                              LRPA_ORACLE_GLOBAL_ACCOUNTING,
                              (uint64_t)(round + 1U) *
                                  context->active_cell_count,
                              lane_winners, 0U);
    }

    result->armed_total += context->active_cell_count;
    result->winner_total += round_winners;
    result->discard_total += round_discards;
    return LRPA_STATUS_OK;
}
