/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

enum {
    LRPA_TRACE_SELECT_ATTEMPT = 10U,
    LRPA_TRACE_SELECT_STALE = 11U
};

static uint32_t
select_target_cell(const lrpa_context_t *context, const lrpa_actor_t *actor)
{
    const uint32_t lane_count = context->manifest.lane_count;

    if (lane_count == 0U) {
        return 0U;
    }
    switch (context->manifest.coupling) {
    case LRPA_COUPLING_INDEPENDENT:
        return actor->lane_id;
    case LRPA_COUPLING_SHARED_OBJECT:
        return 0U;
    case LRPA_COUPLING_RING:
        return (actor->lane_id + actor->worker_id) % lane_count;
    case LRPA_COUPLING_COLORED_GRAPH:
        return (actor->lane_id +
                ((actor->worker_id + 1U) * (actor->worker_id + 2U)) / 2U) %
               lane_count;
    case LRPA_COUPLING_SHARED_SHARD:
    case LRPA_COUPLING_BIPARTITE:
    case LRPA_COUPLING_MIXED_BACKEND:
    case LRPA_COUPLING_COUNT:
        return actor->lane_id;
    }
    return actor->lane_id;
}

static lrpa_outcome_t
worker_outcome(uint32_t worker_id)
{
    switch (worker_id % 4U) {
    case 0U: return LRPA_OUTCOME_SEND;
    case 1U: return LRPA_OUTCOME_CLOSE;
    case 2U: return LRPA_OUTCOME_CANCEL;
    case 3U: return LRPA_OUTCOME_TIMEOUT;
    }
    return LRPA_OUTCOME_SEND;
}

void
lrpa_select_prepare_round(lrpa_context_t *context, uint32_t round)
{
    uint32_t cell_index;
    uint32_t actor_index;

    for (cell_index = 0U; cell_index < context->manifest.lane_count;
         ++cell_index) {
        lrpa_completion_cell_t *cell = &context->cells[cell_index];

        atomic_store_explicit(&cell->outcome, LRPA_OUTCOME_NONE,
                              memory_order_relaxed);
        atomic_store_explicit(&cell->winner_count, 0U, memory_order_relaxed);
        atomic_store_explicit(&cell->terminal_count, 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&cell->live_nodes, 0U, memory_order_relaxed);
        atomic_store_explicit(&cell->discard_count, 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&cell->stale_completion, 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&cell->payload_visible, 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&cell->generation, (uint64_t)round + 1U,
                              memory_order_release);
    }
    for (actor_index = 0U; actor_index < context->actor_count;
         ++actor_index) {
        const uint32_t target =
            select_target_cell(context, &context->actors[actor_index]);

        atomic_fetch_add_explicit(&context->cells[target].live_nodes, 1U,
                                  memory_order_relaxed);
    }
}

bool
lrpa_select_try_complete(lrpa_context_t *context, lrpa_actor_t *actor,
                         uint32_t target_cell, uint64_t generation,
                         lrpa_outcome_t outcome, bool publish_payload)
{
    lrpa_completion_cell_t *cell;
    unsigned int expected = LRPA_OUTCOME_NONE;

    if (context == NULL || actor == NULL ||
        target_cell >= context->active_cell_count ||
        outcome <= LRPA_OUTCOME_NONE || outcome >= LRPA_OUTCOME_COUNT) {
        return false;
    }
    cell = &context->cells[target_cell];
    if (atomic_load_explicit(&cell->generation, memory_order_acquire) !=
        generation) {
#if defined(LRPA_ENABLE_FAULTS)
        if (context->manifest.fault_id ==
            LRPA_FAULT_STALE_GENERATION_REUSE) {
            atomic_store_explicit(&cell->stale_completion, 1U,
                                  memory_order_release);
        } else
#endif
        {
        lrpa_trace_event(context, actor->lane_id, actor->actor_id,
                         LRPA_TRACE_SELECT_STALE, cell->object_id, generation,
                         LRPA_OUTCOME_NONE, LRPA_OUTCOME_NONE, 0,
                         (uintptr_t)cell);
        return false;
        }
    }
    if (atomic_compare_exchange_strong_explicit(
            &cell->outcome, &expected, (unsigned int)outcome,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&cell->winner_count, 1U,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&cell->terminal_count, 1U,
                                  memory_order_relaxed);
        if (publish_payload) {
            atomic_store_explicit(&cell->payload_visible, 1U,
                                  memory_order_release);
        }
        atomic_fetch_add_explicit(&context->lanes[actor->lane_id].winner_count,
                                  1U, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &context->lanes[actor->lane_id].terminal_count, 1U,
            memory_order_relaxed);
        return true;
    }
    atomic_fetch_add_explicit(&cell->discard_count, 1U,
                              memory_order_relaxed);
    return false;
}

static bool
apply_perturbations(lrpa_actor_t *actor, lrpa_outcome_t *outcome)
{
    lrpa_context_t *context = actor->context;
    uint32_t index;

    for (index = 0U; index < context->manifest.perturbation_count; ++index) {
        const lrpa_perturbation_t *step =
            &context->manifest.perturbations[index];

        if ((step->lane_mask & (UINT64_C(1) << actor->lane_id)) == 0U) {
            continue;
        }
        switch (step->kind) {
        case LRPA_STEP_YIELD:
            lrpa_platform_yield();
            break;
        case LRPA_STEP_SPIN:
            lrpa_platform_spin((uint32_t)step->value);
            break;
        case LRPA_STEP_BARRIER:
            if (!lrpa_platform_barrier_wait(
                    &context->perturb_barrier,
                    context->manifest.timeout_ns)) {
                lrpa_request_abort(context);
                return false;
            }
            break;
        case LRPA_STEP_TRIGGER:
            *outcome = LRPA_OUTCOME_SEND;
            break;
        case LRPA_STEP_CANCEL:
            *outcome = LRPA_OUTCOME_CANCEL;
            break;
        case LRPA_STEP_CLOSE:
            *outcome = LRPA_OUTCOME_CLOSE;
            break;
        case LRPA_STEP_TIMER_OFFSET:
            lrpa_platform_spin((uint32_t)((uint64_t)step->value & 127U));
            *outcome = LRPA_OUTCOME_TIMEOUT;
            break;
        case LRPA_STEP_HOST_WAKE:
        case LRPA_STEP_REQUEST_STOP:
        case LRPA_STEP_AFFINITY_ROTATE:
        case LRPA_STEP_KIND_COUNT:
            lrpa_request_abort(context);
            return false;
        }
    }
    return true;
}

void
lrpa_select_actor_step(lrpa_actor_t *actor, uint32_t round)
{
    lrpa_context_t *context = actor->context;
    const uint32_t target = select_target_cell(context, actor);
    lrpa_completion_cell_t *cell = &context->cells[target];
    lrpa_outcome_t outcome = worker_outcome(actor->worker_id);
    uint64_t generation = (uint64_t)round + 1U;
    bool publish_payload;
    bool won;

    if (((context->manifest.seed ^ (uint64_t)round ^ actor->actor_id) & 1U) !=
        0U) {
        lrpa_platform_yield();
    } else {
        lrpa_platform_spin((actor->actor_id + round) & 31U);
    }
    if (!apply_perturbations(actor, &outcome)) {
        atomic_fetch_sub_explicit(&cell->live_nodes, 1U,
                                  memory_order_acq_rel);
        return;
    }
#if defined(LRPA_ENABLE_FAULTS)
    if (context->manifest.fault_id == LRPA_FAULT_SELECT_SKIP_WINNER_CAS &&
        actor->actor_id == 0U) {
        atomic_fetch_add_explicit(&cell->winner_count, 1U,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&cell->terminal_count, 1U,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&context->lanes[actor->lane_id].winner_count,
                                  1U, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &context->lanes[actor->lane_id].terminal_count, 1U,
            memory_order_relaxed);
    }
    if (context->manifest.fault_id == LRPA_FAULT_STALE_GENERATION_REUSE &&
        actor->actor_id == 0U && round != 0U) {
        generation = (uint64_t)round;
    }
#endif
    publish_payload = outcome == LRPA_OUTCOME_SEND;
    won = lrpa_select_try_complete(context, actor, target,
                                   generation, outcome, publish_payload);
    atomic_fetch_sub_explicit(&cell->live_nodes, 1U, memory_order_acq_rel);
    lrpa_trace_event(context, actor->lane_id, actor->actor_id,
                     LRPA_TRACE_SELECT_ATTEMPT, cell->object_id,
                     (uint64_t)round + 1U, LRPA_OUTCOME_NONE,
                     (uint32_t)outcome, won ? 1 : 0, (uintptr_t)cell);
}

void
lrpa_select_drain_round(lrpa_context_t *context)
{
    uint32_t cell;

    for (cell = 0U; cell < context->active_cell_count; ++cell) {
        atomic_store_explicit(&context->cells[cell].live_nodes, 0U,
                              memory_order_release);
    }
}
