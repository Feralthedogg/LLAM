/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

#include <stdlib.h>
#include <string.h>

enum {
    LRPA_TRACE_ACTOR_ARMED = 1U,
    LRPA_TRACE_ACTOR_RELEASED = 2U,
    LRPA_TRACE_ACTOR_FINISHED = 3U
};

static uint64_t
hash_byte(uint64_t hash, uint8_t byte)
{
    hash ^= (uint64_t)byte;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t
hash_u64(uint64_t hash, uint64_t value)
{
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        hash = hash_byte(hash, (uint8_t)(value & UINT64_C(0xff)));
        value >>= 8U;
    }
    return hash;
}

bool
lrpa_checked_multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (result_out == NULL) {
        return false;
    }
    *result_out = 0U;
    if (right != 0U && left > SIZE_MAX / right) {
        return false;
    }
    *result_out = left * right;
    return true;
}

uint64_t
lrpa_perturbation_hash(const lrpa_perturbation_t *steps, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    if (count != 0U && steps == NULL) {
        return 0U;
    }
    hash = hash_u64(hash, UINT64_C(0x4c52504150525401));
    hash = hash_u64(hash, (uint64_t)count);
    for (index = 0U; index < count; ++index) {
        hash = hash_u64(hash, (uint64_t)steps[index].kind);
        hash = hash_u64(hash, steps[index].lane_mask);
        hash = hash_u64(hash, (uint64_t)steps[index].sequence);
        hash = hash_u64(hash, (uint64_t)steps[index].value);
    }
    return hash;
}

static bool
coupling_is_supported(lrpa_coupling_t coupling)
{
    return coupling == LRPA_COUPLING_INDEPENDENT ||
           coupling == LRPA_COUPLING_SHARED_OBJECT ||
           coupling == LRPA_COUPLING_RING ||
           coupling == LRPA_COUPLING_COLORED_GRAPH;
}

static bool
allowed_outcomes_are_valid(uint32_t allowed)
{
    return allowed != 0U && (allowed & ~((uint32_t)LRPA_ALLOW_ALL)) == 0U;
}

static lrpa_status_t
validate_perturbations(const lrpa_manifest_t *manifest)
{
    uint64_t valid_lane_mask;
    uint32_t index;

    if (manifest->perturbation_count > LRPA_MAX_PERTURBATIONS) {
        return LRPA_STATUS_INVALID_DIMENSIONS;
    }
    if (manifest->perturbation_count == 0U) {
        return manifest->perturbation_hash == 0U
                   ? LRPA_STATUS_OK
                   : LRPA_STATUS_MALFORMED_PERTURBATION;
    }
    valid_lane_mask = manifest->lane_count == 64U
                          ? UINT64_MAX
                          : (UINT64_C(1) << manifest->lane_count) - 1U;
    for (index = 0U; index < manifest->perturbation_count; ++index) {
        const lrpa_perturbation_t *step = &manifest->perturbations[index];

        if (step->kind < LRPA_STEP_YIELD ||
            step->kind > LRPA_STEP_TIMER_OFFSET ||
            step->lane_mask == 0U ||
            (step->lane_mask & ~valid_lane_mask) != 0U ||
            step->sequence != index ||
            (step->kind == LRPA_STEP_SPIN &&
             (step->value < 0 || step->value > 1000000))) {
            return LRPA_STATUS_MALFORMED_PERTURBATION;
        }
        if (step->kind == LRPA_STEP_BARRIER &&
            step->lane_mask != valid_lane_mask) {
            return LRPA_STATUS_MALFORMED_PERTURBATION;
        }
    }
    if (lrpa_perturbation_hash(manifest->perturbations,
                               manifest->perturbation_count) !=
        manifest->perturbation_hash) {
        return LRPA_STATUS_MALFORMED_PERTURBATION;
    }
    return LRPA_STATUS_OK;
}

lrpa_status_t
lrpa_manifest_validate(const lrpa_manifest_t *manifest)
{
    uint32_t left;
    uint32_t right;
    lrpa_status_t perturbation_status;

    if (manifest == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    if (manifest->version != LRPA_MANIFEST_VERSION) {
        return LRPA_STATUS_INVALID_VERSION;
    }
    if (manifest->lane_count == 0U ||
        manifest->lane_count > LRPA_MAX_LANES ||
        manifest->worker_count == 0U ||
        manifest->worker_count > LRPA_MAX_WORKERS ||
        manifest->rounds == 0U || manifest->queue_capacity == 0U ||
        manifest->queue_capacity > LRPA_MAX_TRACE_CAPACITY ||
        manifest->timeout_ns == 0U) {
        return LRPA_STATUS_INVALID_DIMENSIONS;
    }
    if (manifest->gadget != LRPA_GADGET_SELECT_COMPLETION ||
        !coupling_is_supported(manifest->coupling) ||
        !allowed_outcomes_are_valid(manifest->allowed_outcomes)) {
        return LRPA_STATUS_INVALID_ENUM;
    }
    if (manifest->fault_id < LRPA_FAULT_NONE ||
        manifest->fault_id >= LRPA_FAULT_COUNT) {
        return LRPA_STATUS_INVALID_ENUM;
    }
#if !defined(LRPA_ENABLE_FAULTS)
    if (manifest->fault_id != LRPA_FAULT_NONE) {
        return LRPA_STATUS_FAULT_UNAVAILABLE;
    }
#endif
    for (left = 0U; left < manifest->lane_count; ++left) {
        if (manifest->object_ids[left] == 0U) {
            return LRPA_STATUS_DUPLICATE_OBJECT;
        }
        for (right = left + 1U; right < manifest->lane_count; ++right) {
            if (manifest->object_ids[left] == manifest->object_ids[right]) {
                return LRPA_STATUS_DUPLICATE_OBJECT;
            }
        }
    }
    perturbation_status = validate_perturbations(manifest);
    if (perturbation_status != LRPA_STATUS_OK) {
        return perturbation_status;
    }
    return LRPA_STATUS_OK;
}

static uint64_t
splitmix64_next(uint64_t *state)
{
    uint64_t value;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

lrpa_status_t
lrpa_manifest_generate_perturbations(lrpa_manifest_t *manifest)
{
    uint64_t state;
    uint32_t count;
    uint32_t index;

    if (manifest == NULL || manifest->lane_count == 0U ||
        manifest->lane_count > LRPA_MAX_LANES) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    count = manifest->perturbation_count;
    if (count == 0U) {
        count = manifest->lane_count * 2U;
        if (count > LRPA_MAX_PERTURBATIONS) {
            count = LRPA_MAX_PERTURBATIONS;
        }
    }
    if (count > LRPA_MAX_PERTURBATIONS) {
        return LRPA_STATUS_INVALID_DIMENSIONS;
    }
    state = manifest->seed;
    for (index = 0U; index < count; ++index) {
        const uint64_t value = splitmix64_next(&state);
        lrpa_perturbation_t *step = &manifest->perturbations[index];
        const uint32_t lane = (uint32_t)(value % manifest->lane_count);

        step->kind = (lrpa_step_kind_t)(value %
                                        ((uint64_t)LRPA_STEP_TIMER_OFFSET +
                                         1U));
        step->lane_mask = step->kind == LRPA_STEP_BARRIER
                              ? (manifest->lane_count == 64U
                                     ? UINT64_MAX
                                     : (UINT64_C(1) <<
                                        manifest->lane_count) - 1U)
                              : UINT64_C(1) << lane;
        step->sequence = index;
        step->value = step->kind == LRPA_STEP_SPIN
                          ? (int64_t)((value >> 16U) % 1024U)
                          : (int64_t)((value >> 24U) & UINT64_C(0xffff));
    }
    manifest->perturbation_count = count;
    manifest->perturbation_hash =
        lrpa_perturbation_hash(manifest->perturbations, count);
    return LRPA_STATUS_OK;
}

void
lrpa_lane_init(lrpa_lane_t *lane, uint32_t id, uint64_t seed)
{
    if (lane == NULL) {
        return;
    }
    memset(lane, 0, sizeof(*lane));
    lane->id = id;
    atomic_init(&lane->phase, LRPA_LANE_ALLOCATED);
    atomic_init(&lane->winner_count, 0U);
    atomic_init(&lane->terminal_count, 0U);
    atomic_init(&lane->invariant_failures, 0U);
    lane->local_seed = seed;
}

static bool
transition_is_allowed(lrpa_lane_phase_t expected,
                      lrpa_lane_phase_t desired)
{
    if (expected == LRPA_LANE_VERIFIED && desired == LRPA_LANE_SETUP) {
        return true;
    }
    return expected >= LRPA_LANE_ALLOCATED &&
           expected < LRPA_LANE_DESTROYED &&
           desired == (lrpa_lane_phase_t)(expected + 1);
}

lrpa_status_t
lrpa_lane_transition(lrpa_lane_t *lane, lrpa_lane_phase_t expected,
                     lrpa_lane_phase_t desired)
{
    unsigned int observed;

    if (lane == NULL || !transition_is_allowed(expected, desired)) {
        return LRPA_STATUS_INVALID_TRANSITION;
    }
    observed = (unsigned int)expected;
    if (!atomic_compare_exchange_strong_explicit(
            &lane->phase, &observed, (unsigned int)desired,
            memory_order_acq_rel, memory_order_acquire)) {
        return LRPA_STATUS_INVALID_TRANSITION;
    }
    return LRPA_STATUS_OK;
}

uint64_t
lrpa_failure_signature(const lrpa_manifest_t *manifest,
                       const lrpa_failure_t *failure)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (manifest == NULL || failure == NULL) {
        return 0U;
    }
    hash = hash_u64(hash, UINT64_C(0x4c52504100000001));
    hash = hash_u64(hash, (uint64_t)manifest->gadget);
    hash = hash_u64(hash, (uint64_t)manifest->coupling);
    hash = hash_u64(hash, (uint64_t)failure->oracle);
    hash = hash_u64(hash, failure->expected);
    hash = hash_u64(hash, failure->actual);
    hash = hash_u64(hash, failure->generation_first);
    hash = hash_u64(hash, failure->generation_second);
    hash = hash_u64(hash, (uint64_t)failure->object_id);
    return hash;
}

const char *
lrpa_status_name(lrpa_status_t status)
{
    switch (status) {
    case LRPA_STATUS_OK: return "ok";
    case LRPA_STATUS_INVALID_ARGUMENT: return "invalid_argument";
    case LRPA_STATUS_INVALID_VERSION: return "invalid_version";
    case LRPA_STATUS_INVALID_DIMENSIONS: return "invalid_dimensions";
    case LRPA_STATUS_INVALID_ENUM: return "invalid_enum";
    case LRPA_STATUS_OVERFLOW: return "overflow";
    case LRPA_STATUS_DUPLICATE_OBJECT: return "duplicate_object";
    case LRPA_STATUS_MALFORMED_PERTURBATION: return "malformed_perturbation";
    case LRPA_STATUS_FAULT_UNAVAILABLE: return "fault_unavailable";
    case LRPA_STATUS_INVALID_TRANSITION: return "invalid_transition";
    case LRPA_STATUS_OUT_OF_MEMORY: return "out_of_memory";
    case LRPA_STATUS_PLATFORM_ERROR: return "platform_error";
    case LRPA_STATUS_TIMEOUT: return "timeout";
    case LRPA_STATUS_ORACLE_FAILURE: return "oracle_failure";
    }
    return "unknown";
}

const char *
lrpa_coupling_name(lrpa_coupling_t coupling)
{
    switch (coupling) {
    case LRPA_COUPLING_INDEPENDENT: return "independent";
    case LRPA_COUPLING_SHARED_SHARD: return "shared_shard";
    case LRPA_COUPLING_SHARED_OBJECT: return "shared_object";
    case LRPA_COUPLING_RING: return "ring";
    case LRPA_COUPLING_BIPARTITE: return "bipartite";
    case LRPA_COUPLING_COLORED_GRAPH: return "colored_graph";
    case LRPA_COUPLING_MIXED_BACKEND: return "mixed_backend";
    case LRPA_COUPLING_COUNT: break;
    }
    return "unknown";
}

const char *
lrpa_fault_name(lrpa_fault_t fault)
{
    switch (fault) {
    case LRPA_FAULT_NONE: return "none";
    case LRPA_FAULT_SELECT_SKIP_WINNER_CAS: return "select_skip_winner_cas";
    case LRPA_FAULT_STALE_GENERATION_REUSE: return "stale_generation_reuse";
    case LRPA_FAULT_COUNT: break;
    }
    return "unknown";
}

static void
initialize_cell(lrpa_completion_cell_t *cell, uint32_t object_id)
{
    atomic_init(&cell->outcome, LRPA_OUTCOME_NONE);
    atomic_init(&cell->winner_count, 0U);
    atomic_init(&cell->terminal_count, 0U);
    atomic_init(&cell->live_nodes, 0U);
    atomic_init(&cell->discard_count, 0U);
    atomic_init(&cell->stale_completion, 0U);
    atomic_init(&cell->payload_visible, 0U);
    atomic_init(&cell->generation, 0U);
    cell->object_id = object_id;
}

lrpa_status_t
lrpa_context_init(lrpa_context_t *context, const lrpa_manifest_t *manifest,
                  const lrpa_run_options_t *options)
{
    size_t actor_count;
    uint32_t index;
    lrpa_status_t status;

    if (context == NULL || manifest == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    status = lrpa_manifest_validate(manifest);
    if (status != LRPA_STATUS_OK) {
        return status;
    }
    if (!lrpa_checked_multiply_size(manifest->lane_count,
                                    manifest->worker_count, &actor_count) ||
        actor_count > UINT32_MAX) {
        return LRPA_STATUS_OVERFLOW;
    }

    memset(context, 0, sizeof(*context));
    context->manifest = *manifest;
    context->actor_count = (uint32_t)actor_count;
    context->active_cell_count =
        manifest->coupling == LRPA_COUPLING_SHARED_OBJECT
            ? 1U
            : manifest->lane_count;
    context->options.fail_setup_actor = UINT32_MAX;
    context->options.stall_actor = UINT32_MAX;
    if (options != NULL) {
        context->options = *options;
    }
    context->lanes = calloc(manifest->lane_count, sizeof(*context->lanes));
    context->cells = calloc(manifest->lane_count, sizeof(*context->cells));
    context->actors = calloc(actor_count, sizeof(*context->actors));
    context->threads = calloc(actor_count, sizeof(*context->threads));
    context->trace_entries = calloc(manifest->queue_capacity,
                                    sizeof(*context->trace_entries));
    if (context->lanes == NULL || context->cells == NULL ||
        context->actors == NULL || context->threads == NULL ||
        context->trace_entries == NULL) {
        lrpa_context_destroy(context);
        return LRPA_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0U; index < manifest->lane_count; ++index) {
        lrpa_lane_init(&context->lanes[index], index,
                       manifest->seed ^ (uint64_t)index);
        initialize_cell(&context->cells[index], manifest->object_ids[index]);
    }
    for (index = 0U; index < context->actor_count; ++index) {
        context->actors[index].context = context;
        context->actors[index].actor_id = index;
        context->actors[index].lane_id = index / manifest->worker_count;
        context->actors[index].worker_id = index % manifest->worker_count;
    }
    lrpa_trace_bind(&context->trace, context->trace_entries,
                    manifest->queue_capacity);
    atomic_init(&context->abort_requested, false);
    atomic_init(&context->armed_actors, 0U);
    atomic_init(&context->release_observed_armed, 0U);
    status = lrpa_platform_gate_init(&context->launch_gate);
    if (status != LRPA_STATUS_OK) {
        lrpa_context_destroy(context);
        return status;
    }
    context->launch_gate_initialized = true;
    status = lrpa_platform_barrier_init(&context->start_barrier,
                                        context->actor_count + 1U);
    if (status != LRPA_STATUS_OK) {
        lrpa_context_destroy(context);
        return status;
    }
    context->start_barrier_initialized = true;
    status = lrpa_platform_barrier_init(&context->finish_barrier,
                                        context->actor_count + 1U);
    if (status != LRPA_STATUS_OK) {
        lrpa_context_destroy(context);
        return status;
    }
    context->finish_barrier_initialized = true;
    status = lrpa_platform_barrier_init(&context->perturb_barrier,
                                        context->actor_count);
    if (status != LRPA_STATUS_OK) {
        lrpa_context_destroy(context);
        return status;
    }
    context->perturb_barrier_initialized = true;
    context->initialized = true;
    return LRPA_STATUS_OK;
}

void
lrpa_request_abort(lrpa_context_t *context)
{
    atomic_store_explicit(&context->abort_requested, true,
                          memory_order_release);
    if (context->start_barrier_initialized) {
        lrpa_platform_barrier_break(&context->start_barrier);
    }
    if (context->finish_barrier_initialized) {
        lrpa_platform_barrier_break(&context->finish_barrier);
    }
    if (context->perturb_barrier_initialized) {
        lrpa_platform_barrier_break(&context->perturb_barrier);
    }
}

static int
coordination_actor_main(void *argument)
{
    lrpa_actor_t *actor = argument;
    lrpa_context_t *context = actor->context;
    uint32_t round;

    if (!lrpa_platform_gate_wait(&context->launch_gate)) {
        return 0;
    }
    for (round = 0U; round < context->manifest.rounds; ++round) {
        if (actor->actor_id == context->options.stall_actor && round == 0U) {
            while (!atomic_load_explicit(&context->abort_requested,
                                         memory_order_acquire)) {
                lrpa_platform_yield();
            }
            return 0;
        }
        if (atomic_load_explicit(&context->abort_requested,
                                 memory_order_acquire)) {
            return 0;
        }
        atomic_fetch_add_explicit(&context->armed_actors, 1U,
                                  memory_order_acq_rel);
        lrpa_trace_event(context, actor->lane_id, actor->actor_id,
                         LRPA_TRACE_ACTOR_ARMED,
                         context->manifest.object_ids[actor->lane_id], round,
                         LRPA_LANE_SETUP, LRPA_LANE_ARMED, 0,
                         (uintptr_t)actor);
        if (!lrpa_platform_barrier_wait(&context->start_barrier,
                                        context->manifest.timeout_ns)) {
            lrpa_request_abort(context);
            return 0;
        }
        lrpa_trace_event(context, actor->lane_id, actor->actor_id,
                         LRPA_TRACE_ACTOR_RELEASED,
                         context->manifest.object_ids[actor->lane_id], round,
                         LRPA_LANE_RELEASED, LRPA_LANE_RACING, 0,
                         (uintptr_t)actor);
        if (context->execute_gadget) {
            lrpa_select_actor_step(actor, round);
        }
        if (!lrpa_platform_barrier_wait(&context->finish_barrier,
                                        context->manifest.timeout_ns)) {
            lrpa_request_abort(context);
            return 0;
        }
        lrpa_trace_event(context, actor->lane_id, actor->actor_id,
                         LRPA_TRACE_ACTOR_FINISHED,
                         context->manifest.object_ids[actor->lane_id], round,
                         LRPA_LANE_DRAINING, LRPA_LANE_VERIFIED, 0,
                         (uintptr_t)actor);
    }
    return 0;
}

static bool
wait_for_all_armed(lrpa_context_t *context)
{
    const uint64_t start = lrpa_platform_monotonic_ns();

    while (atomic_load_explicit(&context->armed_actors,
                                memory_order_acquire) !=
           context->actor_count) {
        const uint64_t now = lrpa_platform_monotonic_ns();

        if (atomic_load_explicit(&context->abort_requested,
                                 memory_order_acquire) ||
            (now >= start && now - start >= context->manifest.timeout_ns)) {
            return false;
        }
        lrpa_platform_yield();
    }
    return true;
}

static lrpa_status_t
transition_lanes(lrpa_context_t *context, lrpa_lane_phase_t expected,
                 lrpa_lane_phase_t desired)
{
    uint32_t lane;

    for (lane = 0U; lane < context->manifest.lane_count; ++lane) {
        const lrpa_status_t status = lrpa_lane_transition(
            &context->lanes[lane], expected, desired);

        if (status != LRPA_STATUS_OK) {
            return status;
        }
    }
    return LRPA_STATUS_OK;
}

static bool
join_all_actors(lrpa_context_t *context)
{
    uint32_t actor;
    bool clean = true;

    if (context->actors == NULL || context->threads == NULL) {
        return true;
    }
    for (actor = 0U; actor < context->actor_count; ++actor) {
        if (context->actors[actor].started &&
            !context->actors[actor].joined) {
            if (lrpa_platform_thread_join(context->threads[actor]) !=
                LRPA_STATUS_OK) {
                clean = false;
            }
            context->actors[actor].joined = true;
        }
    }
    return clean;
}

static void
finalize_result_trace(lrpa_context_t *context, lrpa_result_t *result)
{
    const size_t recorded = atomic_load_explicit(&context->trace.next,
                                                 memory_order_relaxed);

    result->trace_entries = recorded < context->trace.capacity
                                ? recorded
                                : context->trace.capacity;
    result->trace_truncated = atomic_load_explicit(&context->trace.truncated,
                                                   memory_order_relaxed);
}

static lrpa_status_t
run_context(lrpa_context_t *context, lrpa_result_t *result,
            bool execute_gadget)
{
    uint64_t start_ns;
    uint32_t actor;
    uint32_t round;
    lrpa_status_t status = LRPA_STATUS_OK;

    if (context == NULL || result == NULL || !context->initialized) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->version = LRPA_RESULT_VERSION;
    result->seed = context->manifest.seed;
    result->lane_count = context->manifest.lane_count;
    result->first_failure_round = UINT32_MAX;
    context->execute_gadget = execute_gadget;
    start_ns = lrpa_platform_monotonic_ns();

    status = transition_lanes(context, LRPA_LANE_ALLOCATED,
                              LRPA_LANE_SETUP);
    if (status != LRPA_STATUS_OK) {
        result->status = status;
        return status;
    }
    atomic_store_explicit(&context->armed_actors, 0U, memory_order_release);
    for (actor = 0U; actor < context->actor_count; ++actor) {
        if (actor == context->options.fail_setup_actor) {
            status = LRPA_STATUS_PLATFORM_ERROR;
            break;
        }
        status = lrpa_platform_thread_create(
            &context->threads[actor], coordination_actor_main,
            &context->actors[actor]);
        if (status != LRPA_STATUS_OK) {
            break;
        }
        context->actors[actor].started = true;
    }
    if (status != LRPA_STATUS_OK) {
        atomic_store_explicit(&context->abort_requested, true,
                              memory_order_release);
        lrpa_platform_gate_open(&context->launch_gate, true);
        result->cleanup_complete = join_all_actors(context);
        result->status = status;
        finalize_result_trace(context, result);
        result->elapsed_ns = lrpa_platform_monotonic_ns() - start_ns;
        return status;
    }
    lrpa_platform_gate_open(&context->launch_gate, false);

    for (round = 0U; round < context->manifest.rounds; ++round) {
        if (round != 0U) {
            status = transition_lanes(context, LRPA_LANE_VERIFIED,
                                      LRPA_LANE_SETUP);
            if (status != LRPA_STATUS_OK) {
                break;
            }
        }
        status = transition_lanes(context, LRPA_LANE_SETUP,
                                  LRPA_LANE_ARMED);
        if (status == LRPA_STATUS_OK && execute_gadget) {
            lrpa_select_prepare_round(context, round);
        }
        if (status != LRPA_STATUS_OK || !wait_for_all_armed(context)) {
            status = status == LRPA_STATUS_OK ? LRPA_STATUS_TIMEOUT : status;
            break;
        }
        atomic_store_explicit(
            &context->release_observed_armed,
            atomic_load_explicit(&context->armed_actors,
                                 memory_order_acquire),
            memory_order_release);
        status = transition_lanes(context, LRPA_LANE_ARMED,
                                  LRPA_LANE_RELEASED);
        if (status == LRPA_STATUS_OK) {
            status = transition_lanes(context, LRPA_LANE_RELEASED,
                                      LRPA_LANE_RACING);
        }
        if (status != LRPA_STATUS_OK ||
            !lrpa_platform_barrier_wait(&context->start_barrier,
                                        context->manifest.timeout_ns)) {
            status = status == LRPA_STATUS_OK ? LRPA_STATUS_TIMEOUT : status;
            break;
        }
        atomic_store_explicit(&context->armed_actors, 0U,
                              memory_order_release);
        if (!lrpa_platform_barrier_wait(&context->finish_barrier,
                                        context->manifest.timeout_ns)) {
            status = LRPA_STATUS_TIMEOUT;
            break;
        }
        if (execute_gadget) {
            lrpa_failure_t failure;

            memset(&failure, 0, sizeof(failure));
            status = lrpa_select_verify_round(context, round, &failure,
                                              result);
            lrpa_select_drain_round(context);
            if (status != LRPA_STATUS_OK) {
                result->failures += 1U;
                result->first_failure_round = round;
                result->first_failure = failure;
                result->signature = lrpa_failure_signature(
                    &context->manifest, &failure);
            }
        }
        status = transition_lanes(context, LRPA_LANE_RACING,
                                  LRPA_LANE_DRAINING);
        if (status == LRPA_STATUS_OK) {
            status = transition_lanes(context, LRPA_LANE_DRAINING,
                                      LRPA_LANE_VERIFIED);
        }
        if (status != LRPA_STATUS_OK) {
            break;
        }
        result->rounds_completed = round + 1U;
        if (!execute_gadget) {
            result->armed_total += context->manifest.lane_count;
        }
        if (result->failures != 0U) {
            status = LRPA_STATUS_ORACLE_FAILURE;
            break;
        }
    }

    if (status != LRPA_STATUS_OK) {
        lrpa_request_abort(context);
    }
    result->cleanup_complete = join_all_actors(context);
    if (!result->cleanup_complete && status == LRPA_STATUS_OK) {
        status = LRPA_STATUS_PLATFORM_ERROR;
    }
    result->status = status;
    result->lane_executions =
        (uint64_t)result->rounds_completed * context->actor_count;
    finalize_result_trace(context, result);
    result->elapsed_ns = lrpa_platform_monotonic_ns() - start_ns;
    return status;
}

lrpa_status_t
lrpa_context_run_coordination_probe(lrpa_context_t *context,
                                    lrpa_result_t *result)
{
    return run_context(context, result, false);
}

lrpa_status_t
lrpa_context_run(lrpa_context_t *context, lrpa_result_t *result)
{
    return run_context(context, result, true);
}

lrpa_status_t
lrpa_context_reset(lrpa_context_t *context)
{
    uint32_t index;
    lrpa_status_t status;

    if (context == NULL || !context->initialized) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < context->actor_count; ++index) {
        if (context->actors[index].started &&
            !context->actors[index].joined) {
            return LRPA_STATUS_INVALID_TRANSITION;
        }
    }
    if (context->finish_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->finish_barrier);
        context->finish_barrier_initialized = false;
    }
    if (context->perturb_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->perturb_barrier);
        context->perturb_barrier_initialized = false;
    }
    if (context->start_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->start_barrier);
        context->start_barrier_initialized = false;
    }
    if (context->launch_gate_initialized) {
        lrpa_platform_gate_destroy(&context->launch_gate);
        context->launch_gate_initialized = false;
    }
    status = lrpa_platform_gate_init(&context->launch_gate);
    if (status != LRPA_STATUS_OK) {
        return status;
    }
    context->launch_gate_initialized = true;
    status = lrpa_platform_barrier_init(&context->start_barrier,
                                        context->actor_count + 1U);
    if (status != LRPA_STATUS_OK) {
        return status;
    }
    context->start_barrier_initialized = true;
    status = lrpa_platform_barrier_init(&context->finish_barrier,
                                        context->actor_count + 1U);
    if (status != LRPA_STATUS_OK) {
        return status;
    }
    context->finish_barrier_initialized = true;
    status = lrpa_platform_barrier_init(&context->perturb_barrier,
                                        context->actor_count);
    if (status != LRPA_STATUS_OK) {
        return status;
    }
    context->perturb_barrier_initialized = true;

    for (index = 0U; index < context->manifest.lane_count; ++index) {
        lrpa_lane_init(&context->lanes[index], index,
                       context->manifest.seed ^ (uint64_t)index);
        initialize_cell(&context->cells[index],
                        context->manifest.object_ids[index]);
    }
    for (index = 0U; index < context->actor_count; ++index) {
        context->actors[index].started = false;
        context->actors[index].joined = false;
    }
    atomic_store_explicit(&context->trace.next, 0U, memory_order_relaxed);
    atomic_store_explicit(&context->trace.truncated, false,
                          memory_order_relaxed);
    atomic_store_explicit(&context->abort_requested, false,
                          memory_order_release);
    atomic_store_explicit(&context->armed_actors, 0U, memory_order_release);
    atomic_store_explicit(&context->release_observed_armed, 0U,
                          memory_order_release);
    return LRPA_STATUS_OK;
}

void
lrpa_context_destroy(lrpa_context_t *context)
{
    uint32_t index;

    if (context == NULL) {
        return;
    }
    if (context->launch_gate_initialized) {
        lrpa_platform_gate_open(&context->launch_gate, true);
    }
    if (context->start_barrier_initialized ||
        context->finish_barrier_initialized ||
        context->perturb_barrier_initialized) {
        lrpa_request_abort(context);
    }
    (void)join_all_actors(context);
    if (context->finish_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->finish_barrier);
    }
    if (context->perturb_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->perturb_barrier);
    }
    if (context->start_barrier_initialized) {
        lrpa_platform_barrier_destroy(&context->start_barrier);
    }
    if (context->launch_gate_initialized) {
        lrpa_platform_gate_destroy(&context->launch_gate);
    }
    if (context->lanes != NULL) {
        for (index = 0U; index < context->manifest.lane_count; ++index) {
            atomic_store_explicit(&context->lanes[index].phase,
                                  LRPA_LANE_DESTROYED,
                                  memory_order_release);
        }
    }
    free(context->trace_entries);
    free(context->threads);
    free(context->actors);
    free(context->cells);
    free(context->lanes);
    memset(context, 0, sizeof(*context));
}
