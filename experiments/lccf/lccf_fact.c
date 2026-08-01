/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define LCCF_FACT_STATE_BITS 4U
#define LCCF_FACT_STATE_MASK UINT64_C(0x0f)

static bool state_is_visible(lccf_fact_state_t state) {
    return state == LCCF_FACT_STATE_READY ||
           state == LCCF_FACT_STATE_RUNNING_DIRECT ||
           state == LCCF_FACT_STATE_QUEUED ||
           state == LCCF_FACT_STATE_RUNNING_QUEUED;
}

static bool state_is_running(lccf_fact_state_t state) {
    return state == LCCF_FACT_STATE_RUNNING_DIRECT ||
           state == LCCF_FACT_STATE_RUNNING_QUEUED;
}

static bool source_is_valid(lccf_fact_source_t source) {
    return source >= LCCF_FACT_SOURCE_LINUX_CQE &&
           source < LCCF_FACT_SOURCE_COUNT;
}

static bool event_is_valid(lccf_fact_event_kind_t event_kind) {
    return event_kind >= LCCF_FACT_EVENT_IO &&
           event_kind < LCCF_FACT_EVENT_COUNT;
}

static bool layout_is_valid(lccf_fact_layout_t layout) {
    return layout >= LCCF_FACT_LAYOUT_SPLIT64_64 &&
           layout < LCCF_FACT_LAYOUT_COUNT;
}

static void counter_increment(uint64_t *counter) {
    if (counter != NULL) {
        *counter += 1U;
    }
}

static int64_t bits_to_i64(uint64_t value) {
    int64_t result;

    memcpy(&result, &value, sizeof(result));
    return result;
}

static uint64_t i64_to_bits(int64_t value) {
    uint64_t result;

    memcpy(&result, &value, sizeof(result));
    return result;
}

static uint64_t canonical_fact_id(const lccf_fact_core_t *fact) {
    uint64_t hash = UINT64_C(1469598103934665603);
    static const uint64_t prime = UINT64_C(1099511628211);
    const uint64_t words[] = {
        fact->generation,
        fact->stable_flags,
        i64_to_bits(fact->result),
        fact->payload_word,
        ((uint64_t)(uint32_t)fact->error_code << 32U) | fact->event_kind,
        ((uint64_t)fact->captured_home_shard << 32U) | fact->source_node,
        fact->site_index,
    };
    size_t index;

    for (index = 0U; index < sizeof(words) / sizeof(words[0]); ++index) {
        uint64_t word = words[index];
        unsigned byte;

        for (byte = 0U; byte < 8U; ++byte) {
            hash ^= word & UINT64_C(0xff);
            hash *= prime;
            word >>= 8U;
        }
    }
    return hash;
}

static int reference_add(lccf_fact_cell_t *cell,
                         lccf_fact_ref_kind_t kind,
                         uint32_t amount) {
    uint32_t current;

    if (cell == NULL || kind < LCCF_FACT_REF_CALLBACK ||
        kind >= LCCF_FACT_REF_COUNT || amount == 0U) {
        return EINVAL;
    }
    current = atomic_load_explicit(&cell->references[kind],
                                   memory_order_relaxed);
    for (;;) {
        if (current > UINT32_MAX - amount) {
            return EOVERFLOW;
        }
        if (atomic_compare_exchange_weak_explicit(
                &cell->references[kind], &current, current + amount,
                memory_order_acq_rel, memory_order_relaxed)) {
            return 0;
        }
    }
}

static int reference_subtract(lccf_fact_cell_t *cell,
                              lccf_fact_ref_kind_t kind,
                              uint32_t amount) {
    uint32_t current;

    if (cell == NULL || kind < LCCF_FACT_REF_CALLBACK ||
        kind >= LCCF_FACT_REF_COUNT || amount == 0U) {
        return EINVAL;
    }
    current = atomic_load_explicit(&cell->references[kind],
                                   memory_order_relaxed);
    for (;;) {
        if (current < amount) {
            return EPROTO;
        }
        if (atomic_compare_exchange_weak_explicit(
                &cell->references[kind], &current, current - amount,
                memory_order_acq_rel, memory_order_relaxed)) {
            return 0;
        }
    }
}

static int retire_backend_reference(lccf_fact_cell_t *cell) {
    return reference_subtract(cell, LCCF_FACT_REF_BACKEND, 1U);
}

static void make_failure_fact(const lccf_fact_ticket_t *ticket,
                              uint64_t fact_id,
                              lccf_fact_core_t *fact) {
    memset(fact, 0, sizeof(*fact));
    fact->generation = ticket->generation;
    fact->fact_id = fact_id;
    fact->stable_flags = ticket->stable_flags;
    fact->result = -1;
    fact->error_code = EPROTO;
    fact->event_kind = LCCF_FACT_EVENT_FAIL;
    fact->source_kind = ticket->source_kind;
    fact->captured_home_shard = ticket->captured_home_shard;
    fact->source_node = ticket->source_node;
    fact->site_index = ticket->site_index;
}

uint64_t lccf_fact_pack_state(uint64_t generation,
                              lccf_fact_state_t state) {
    if (generation > LCCF_FACT_MAX_GENERATION ||
        state < LCCF_FACT_STATE_ARMED || state >= LCCF_FACT_STATE_COUNT) {
        return 0U;
    }
    return (generation << LCCF_FACT_STATE_BITS) | (uint64_t)state;
}

uint64_t lccf_fact_unpack_generation(uint64_t word) {
    return word >> LCCF_FACT_STATE_BITS;
}

lccf_fact_state_t lccf_fact_unpack_state(uint64_t word) {
    return (lccf_fact_state_t)(word & LCCF_FACT_STATE_MASK);
}

int lccf_fact_cell_init(lccf_fact_cell_t *cell, uint64_t generation,
                        lccf_fact_layout_t layout,
                        uint32_t ticket_count) {
    size_t index;

    if (cell == NULL || generation > LCCF_FACT_MAX_GENERATION ||
        !layout_is_valid(layout) || ticket_count == 0U) {
        return EINVAL;
    }
    memset(cell, 0, sizeof(*cell));
    atomic_init(&cell->state_generation,
                lccf_fact_pack_state(generation, LCCF_FACT_STATE_ARMED));
    for (index = 0U; index < LCCF_FACT_REF_COUNT; ++index) {
        atomic_init(&cell->references[index], 0U);
    }
    atomic_store_explicit(&cell->references[LCCF_FACT_REF_BACKEND],
                          ticket_count, memory_order_relaxed);
    cell->layout = layout;
    cell->ticket_count = ticket_count;
    return 0;
}

int lccf_fact_cell_arm(lccf_fact_cell_t *cell, uint64_t generation,
                       uint32_t ticket_count) {
    uint64_t current;
    uint64_t exclusive;
    size_t index;

    if (cell == NULL || generation > LCCF_FACT_MAX_GENERATION ||
        ticket_count == 0U) {
        return EINVAL;
    }
    if (!lccf_fact_can_reuse(cell)) {
        return EBUSY;
    }
    current = atomic_load_explicit(&cell->state_generation,
                                   memory_order_acquire);
    if (lccf_fact_unpack_generation(current) != generation &&
        lccf_fact_unpack_state(current) != LCCF_FACT_STATE_TERMINAL) {
        return ESTALE;
    }
    exclusive = lccf_fact_pack_state(
        lccf_fact_unpack_generation(current), LCCF_FACT_STATE_BUILDING);
    if (!atomic_compare_exchange_strong_explicit(
            &cell->state_generation, &current, exclusive,
            memory_order_acq_rel, memory_order_acquire)) {
        return EBUSY;
    }
    memset(&cell->fact, 0, sizeof(cell->fact));
    memset(&cell->raw_ticket, 0, sizeof(cell->raw_ticket));
    for (index = 0U; index < LCCF_FACT_REF_COUNT; ++index) {
        atomic_store_explicit(&cell->references[index], 0U,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&cell->references[LCCF_FACT_REF_BACKEND],
                          ticket_count, memory_order_relaxed);
    cell->ticket_count = ticket_count;
    cell->shared = false;
    cell->published = false;
    atomic_store_explicit(
        &cell->state_generation,
        lccf_fact_pack_state(generation, LCCF_FACT_STATE_ARMED),
        memory_order_release);
    return 0;
}

int lccf_fact_ticket_from_logical(
    lccf_fact_source_t source, lccf_fact_event_kind_t event_kind,
    uint64_t generation, int64_t result, int32_t error_code,
    uint64_t payload_word, uint32_t site_index, uint32_t site_count,
    uint32_t captured_home_shard, uint32_t source_node,
    lccf_fact_ticket_t *out_ticket) {
    if (!source_is_valid(source) || !event_is_valid(event_kind) ||
        generation > LCCF_FACT_MAX_GENERATION || error_code < 0 ||
        site_count == 0U || out_ticket == NULL) {
        return EINVAL;
    }
    memset(out_ticket, 0, sizeof(*out_ticket));
    out_ticket->generation = generation;
    out_ticket->payload_word = payload_word;
    out_ticket->event_kind = (uint32_t)event_kind;
    out_ticket->source_kind = (uint32_t)source;
    out_ticket->captured_home_shard = captured_home_shard;
    out_ticket->source_node = source_node;
    out_ticket->site_index = site_index;
    out_ticket->site_count = site_count;

    switch (source) {
    case LCCF_FACT_SOURCE_LINUX_CQE:
        if (error_code != 0) {
            out_ticket->raw_result = -(int64_t)error_code;
            out_ticket->raw_aux = i64_to_bits(result);
        } else {
            out_ticket->raw_result = result;
        }
        break;
    case LCCF_FACT_SOURCE_KQUEUE:
        out_ticket->raw_result = result;
        out_ticket->raw_error = error_code;
        if (error_code != 0) {
            out_ticket->raw_flags |= LCCF_FACT_RAW_KQUEUE_ERROR;
        }
        break;
    case LCCF_FACT_SOURCE_IOCP:
        out_ticket->raw_result = result;
        out_ticket->raw_error = error_code;
        if (error_code == 0) {
            out_ticket->raw_flags |= LCCF_FACT_RAW_IOCP_SUCCESS;
        }
        break;
    case LCCF_FACT_SOURCE_TIMER:
    case LCCF_FACT_SOURCE_CANCEL:
    case LCCF_FACT_SOURCE_EXTERNAL:
    case LCCF_FACT_SOURCE_STOP:
        out_ticket->raw_result = result;
        out_ticket->raw_error = error_code;
        break;
    default:
        return EINVAL;
    }
    return 0;
}

int lccf_fact_normalize(const lccf_fact_ticket_t *ticket,
                        uint64_t fact_id,
                        lccf_fact_core_t *out_fact) {
    int64_t result;
    int32_t error_code;
    bool malformed;

    if (ticket == NULL || out_fact == NULL ||
        ticket->generation > LCCF_FACT_MAX_GENERATION ||
        !source_is_valid((lccf_fact_source_t)ticket->source_kind) ||
        !event_is_valid((lccf_fact_event_kind_t)ticket->event_kind)) {
        return EINVAL;
    }
    malformed = (ticket->raw_flags & LCCF_FACT_TICKET_MALFORMED) != 0U ||
                (ticket->raw_flags & LCCF_FACT_TICKET_INVALID_SITE) != 0U ||
                (ticket->raw_flags &
                 LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN) != 0U ||
                ticket->site_count == 0U ||
                ticket->site_index >= ticket->site_count;
    result = ticket->raw_result;
    error_code = ticket->raw_error;

    switch ((lccf_fact_source_t)ticket->source_kind) {
    case LCCF_FACT_SOURCE_LINUX_CQE:
        if (ticket->raw_result < 0) {
            int64_t decoded_error;

            if (ticket->raw_result == INT64_MIN) {
                malformed = true;
            } else {
                decoded_error = -ticket->raw_result;
                if (decoded_error > INT32_MAX) {
                    malformed = true;
                } else {
                    error_code = (int32_t)decoded_error;
                    result = bits_to_i64(ticket->raw_aux);
                }
            }
        } else if (ticket->raw_error != 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_KQUEUE:
        if ((ticket->raw_flags & LCCF_FACT_RAW_KQUEUE_ERROR) != 0U) {
            if (ticket->raw_error <= 0) {
                malformed = true;
            }
        } else if (ticket->raw_error != 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_IOCP:
        if ((ticket->raw_flags & LCCF_FACT_RAW_IOCP_SUCCESS) != 0U) {
            if (ticket->raw_error != 0) {
                malformed = true;
            }
        } else if (ticket->raw_error <= 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_TIMER:
    case LCCF_FACT_SOURCE_CANCEL:
    case LCCF_FACT_SOURCE_EXTERNAL:
    case LCCF_FACT_SOURCE_STOP:
        if (ticket->raw_error < 0) {
            malformed = true;
        }
        break;
    default:
        return EINVAL;
    }

    if (malformed) {
        make_failure_fact(ticket, fact_id, out_fact);
        return 0;
    }
    memset(out_fact, 0, sizeof(*out_fact));
    out_fact->generation = ticket->generation;
    out_fact->fact_id = fact_id;
    out_fact->stable_flags = ticket->stable_flags;
    out_fact->result = result;
    out_fact->payload_word = ticket->payload_word;
    out_fact->error_code = error_code;
    out_fact->event_kind = ticket->event_kind;
    out_fact->source_kind = ticket->source_kind;
    out_fact->captured_home_shard = ticket->captured_home_shard;
    out_fact->source_node = ticket->source_node;
    out_fact->site_index = ticket->site_index;
    return 0;
}

int lccf_fact_try_publish(lccf_fact_cell_t *cell,
                          const lccf_fact_ticket_t *ticket,
                          bool shared,
                          lccf_fact_counters_t *counters,
                          bool *out_won) {
    uint64_t observed;
    uint64_t building;
    uint64_t ready;
    uint64_t generation;
    int rc;

    if (cell == NULL || ticket == NULL || counters == NULL ||
        out_won == NULL || ticket->generation > LCCF_FACT_MAX_GENERATION) {
        return EINVAL;
    }
    *out_won = false;
    counter_increment(&counters->claim_attempts);
    observed = atomic_load_explicit(&cell->state_generation,
                                    memory_order_acquire);
    generation = lccf_fact_unpack_generation(observed);
    if (ticket->generation != generation) {
        counter_increment(&counters->generation_mismatches);
        counter_increment(&counters->stale_losers);
        return ESTALE;
    }
    building = lccf_fact_pack_state(generation, LCCF_FACT_STATE_BUILDING);
    if (lccf_fact_unpack_state(observed) != LCCF_FACT_STATE_ARMED ||
        !atomic_compare_exchange_strong_explicit(
            &cell->state_generation, &observed, building,
            memory_order_acq_rel, memory_order_acquire)) {
        rc = retire_backend_reference(cell);
        if (rc != 0) {
            return rc;
        }
        counter_increment(&counters->stale_losers);
        return 0;
    }

    if ((ticket->raw_flags & LCCF_FACT_TICKET_FAIL_MODULE_PIN) != 0U) {
        counter_increment(&counters->fact_build_failures);
        rc = retire_backend_reference(cell);
        atomic_store_explicit(
            &cell->state_generation,
            lccf_fact_pack_state(generation, LCCF_FACT_STATE_TERMINAL),
            memory_order_release);
        return rc == 0 ? ENODEV : rc;
    }
    rc = reference_add(cell, LCCF_FACT_REF_MODULE, 1U);
    if (rc != 0) {
        atomic_store_explicit(
            &cell->state_generation,
            lccf_fact_pack_state(generation, LCCF_FACT_STATE_TERMINAL),
            memory_order_release);
        return rc;
    }
    counter_increment(&counters->module_pins);

    if (ticket->payload_word != 0U &&
        (ticket->raw_flags & LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN) == 0U) {
        rc = reference_add(cell, LCCF_FACT_REF_PAYLOAD, 1U);
        if (rc != 0) {
            (void)reference_subtract(cell, LCCF_FACT_REF_MODULE, 1U);
            atomic_store_explicit(
                &cell->state_generation,
                lccf_fact_pack_state(generation, LCCF_FACT_STATE_TERMINAL),
                memory_order_release);
            return rc;
        }
        counter_increment(&counters->payload_pins);
    }

    cell->raw_ticket = *ticket;
    cell->shared = shared;
    cell->published = false;
    if (shared) {
        counter_increment(&counters->normalization_calls);
        counter_increment(&counters->site_lookups);
        rc = lccf_fact_normalize(ticket, 0U, &cell->fact);
        if (rc != 0) {
            counter_increment(&counters->fact_build_failures);
            (void)reference_subtract(cell, LCCF_FACT_REF_PAYLOAD, 1U);
            (void)reference_subtract(cell, LCCF_FACT_REF_MODULE, 1U);
            (void)retire_backend_reference(cell);
            atomic_store_explicit(
                &cell->state_generation,
                lccf_fact_pack_state(generation, LCCF_FACT_STATE_TERMINAL),
                memory_order_release);
            return rc;
        }
        cell->fact.fact_id = canonical_fact_id(&cell->fact);
        if (cell->fact.event_kind == LCCF_FACT_EVENT_FAIL) {
            counter_increment(&counters->fact_build_failures);
            if ((ticket->raw_flags &
                 LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN) != 0U) {
                cell->fact.payload_word = 0U;
            }
        }
    }
    counter_increment(&counters->fact_builds);
    rc = retire_backend_reference(cell);
    if (rc != 0) {
        (void)reference_subtract(cell, LCCF_FACT_REF_PAYLOAD, 1U);
        (void)reference_subtract(cell, LCCF_FACT_REF_MODULE, 1U);
        atomic_store_explicit(
            &cell->state_generation,
            lccf_fact_pack_state(generation, LCCF_FACT_STATE_TERMINAL),
            memory_order_release);
        return rc;
    }
    cell->published = true;
    ready = lccf_fact_pack_state(generation, LCCF_FACT_STATE_READY);
    atomic_store_explicit(&cell->state_generation, ready,
                          memory_order_release);
    *out_won = true;
    return 0;
}

static int snapshot_fact(const lccf_fact_cell_t *cell,
                         uint64_t generation,
                         bool require_shared,
                         lccf_fact_core_t *out_fact) {
    lccf_fact_cell_t *mutable_cell = (lccf_fact_cell_t *)(uintptr_t)cell;
    uint64_t before;
    uint64_t after;
    int rc;

    if (cell == NULL || out_fact == NULL) {
        return EINVAL;
    }
    before = atomic_load_explicit(&cell->state_generation,
                                  memory_order_acquire);
    if (lccf_fact_unpack_generation(before) != generation) {
        return ESTALE;
    }
    if (!state_is_visible(lccf_fact_unpack_state(before))) {
        return EAGAIN;
    }
    if (require_shared && !cell->shared) {
        return ENODATA;
    }
    rc = reference_add(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U);
    if (rc != 0) {
        return rc;
    }
    after = atomic_load_explicit(&cell->state_generation,
                                 memory_order_acquire);
    if (lccf_fact_unpack_generation(after) != generation ||
        !state_is_visible(lccf_fact_unpack_state(after)) ||
        !cell->published) {
        (void)reference_subtract(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U);
        return ESTALE;
    }
    *out_fact = cell->fact;
    rc = reference_subtract(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U);
    return rc;
}

int lccf_fact_acquire(const lccf_fact_cell_t *cell,
                      uint64_t generation,
                      lccf_fact_core_t *out_fact) {
    return snapshot_fact(cell, generation, true, out_fact);
}

int lccf_fact_materialize(const lccf_fact_cell_t *cell,
                          uint64_t generation,
                          lccf_fact_counters_t *counters,
                          lccf_fact_core_t *out_fact) {
    lccf_fact_cell_t *mutable_cell = (lccf_fact_cell_t *)(uintptr_t)cell;
    uint64_t before;
    uint64_t after;
    int rc;

    if (cell == NULL || counters == NULL || out_fact == NULL) {
        return EINVAL;
    }
    before = atomic_load_explicit(&cell->state_generation,
                                  memory_order_acquire);
    if (lccf_fact_unpack_generation(before) != generation) {
        return ESTALE;
    }
    if (!state_is_visible(lccf_fact_unpack_state(before))) {
        return EAGAIN;
    }
    if (cell->shared) {
        return snapshot_fact(cell, generation, true, out_fact);
    }
    rc = reference_add(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U);
    if (rc != 0) {
        return rc;
    }
    after = atomic_load_explicit(&cell->state_generation,
                                 memory_order_acquire);
    if (lccf_fact_unpack_generation(after) != generation ||
        !state_is_visible(lccf_fact_unpack_state(after)) ||
        !cell->published) {
        (void)reference_subtract(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U);
        return ESTALE;
    }
    counter_increment(&counters->normalization_calls);
    counter_increment(&counters->site_lookups);
    rc = lccf_fact_normalize(&cell->raw_ticket, 0U, out_fact);
    if (rc == 0) {
        out_fact->fact_id = canonical_fact_id(out_fact);
    }
    if (reference_subtract(mutable_cell, LCCF_FACT_REF_EXTERNAL, 1U) != 0 &&
        rc == 0) {
        rc = EPROTO;
    }
    return rc;
}

static uint64_t guard_escape_reasons(const lccf_fact_guard_t *guard) {
    uint64_t reasons = 0U;

    if ((guard->flags & LCCF_FACT_GUARD_DIRECT_ENABLED) == 0U) {
        reasons |= LCCF_FACT_ESCAPE_MODULE_POLICY;
    }
    if ((guard->flags & LCCF_FACT_GUARD_MODULE_ENABLED) == 0U) {
        reasons |= LCCF_FACT_ESCAPE_MODULE_POLICY;
    }
    if ((guard->flags & LCCF_FACT_GUARD_BACKEND_CAPABLE) == 0U) {
        reasons |= LCCF_FACT_ESCAPE_BACKEND_CAPABILITY;
    }
    if ((guard->flags & LCCF_FACT_GUARD_STOP) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_STOP;
    }
    if ((guard->flags & LCCF_FACT_GUARD_FAIRNESS_DUE) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_FAIRNESS;
    }
    if ((guard->flags & LCCF_FACT_GUARD_TRACE) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_TRACE;
    }
    if ((guard->flags & (LCCF_FACT_GUARD_SHARD_PAUSED |
                         LCCF_FACT_GUARD_SHARD_OFFLINE)) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_SHARD_STATE;
    }
    if ((guard->flags & LCCF_FACT_GUARD_CALLBACK_ACTIVE) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_CALLBACK_ACTIVE;
    }
    if ((guard->flags & LCCF_FACT_GUARD_QUEUE_PRESSURE) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_QUEUE_PRESSURE;
    }
    if ((guard->flags & LCCF_FACT_GUARD_MIGRATING) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_MIGRATION;
    }
    if (guard->budget_remaining == 0U) {
        reasons |= LCCF_FACT_ESCAPE_BUDGET;
    }
    if (guard->consuming_shard != guard->current_home_shard) {
        reasons |= LCCF_FACT_ESCAPE_WRONG_SHARD;
    }
    return reasons;
}

static uint64_t queued_defer_reasons(const lccf_fact_guard_t *guard) {
    uint64_t reasons = 0U;

    if ((guard->flags & LCCF_FACT_GUARD_MODULE_ENABLED) == 0U) {
        reasons |= LCCF_FACT_ESCAPE_MODULE_POLICY;
    }
    if ((guard->flags & LCCF_FACT_GUARD_BACKEND_CAPABLE) == 0U) {
        reasons |= LCCF_FACT_ESCAPE_BACKEND_CAPABILITY;
    }
    if ((guard->flags & LCCF_FACT_GUARD_STOP) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_STOP;
    }
    if ((guard->flags & (LCCF_FACT_GUARD_SHARD_PAUSED |
                         LCCF_FACT_GUARD_SHARD_OFFLINE)) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_SHARD_STATE;
    }
    if ((guard->flags & LCCF_FACT_GUARD_CALLBACK_ACTIVE) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_CALLBACK_ACTIVE;
    }
    if ((guard->flags & LCCF_FACT_GUARD_MIGRATING) != 0U) {
        reasons |= LCCF_FACT_ESCAPE_MIGRATION;
    }
    return reasons;
}

int lccf_fact_consume(lccf_fact_cell_t *cell, uint64_t generation,
                      lccf_fact_consumer_t consumer,
                      const lccf_fact_guard_t *guard,
                      lccf_fact_counters_t *counters,
                      lccf_fact_decision_t *out_decision,
                      lccf_fact_core_t *out_fact) {
    uint64_t observed;
    uint64_t desired;
    uint64_t reasons;
    lccf_fact_state_t state;
    int rc;

    if (cell == NULL || guard == NULL || counters == NULL ||
        out_decision == NULL || out_fact == NULL ||
        (consumer != LCCF_FACT_CONSUMER_DIRECT &&
         consumer != LCCF_FACT_CONSUMER_QUEUE)) {
        return EINVAL;
    }
    observed = atomic_load_explicit(&cell->state_generation,
                                    memory_order_acquire);
    if (lccf_fact_unpack_generation(observed) != generation) {
        return ESTALE;
    }
    state = lccf_fact_unpack_state(observed);
    if ((consumer == LCCF_FACT_CONSUMER_DIRECT &&
         state != LCCF_FACT_STATE_READY) ||
        (consumer == LCCF_FACT_CONSUMER_QUEUE &&
         state != LCCF_FACT_STATE_QUEUED)) {
        return EPROTO;
    }
    rc = lccf_fact_materialize(cell, generation, counters, out_fact);
    if (rc != 0) {
        return rc;
    }
    counter_increment(&counters->guard_rechecks);
    memset(out_decision, 0, sizeof(*out_decision));
    out_decision->destination_shard = guard->current_home_shard;
    reasons = guard_escape_reasons(guard);
    out_decision->escape_reasons = reasons;

    if (consumer == LCCF_FACT_CONSUMER_DIRECT) {
        if (reasons == 0U) {
            rc = reference_add(cell, LCCF_FACT_REF_CALLBACK, 1U);
            if (rc != 0) {
                return rc;
            }
            desired = lccf_fact_pack_state(
                generation, LCCF_FACT_STATE_RUNNING_DIRECT);
            if (!atomic_compare_exchange_strong_explicit(
                    &cell->state_generation, &observed, desired,
                    memory_order_acq_rel, memory_order_acquire)) {
                (void)reference_subtract(cell, LCCF_FACT_REF_CALLBACK, 1U);
                return EBUSY;
            }
            out_decision->route = LCCF_FACT_ROUTE_DIRECT;
            return 0;
        }
        rc = reference_add(cell, LCCF_FACT_REF_QUEUE, 1U);
        if (rc != 0) {
            return rc;
        }
        desired = lccf_fact_pack_state(generation, LCCF_FACT_STATE_QUEUED);
        if (!atomic_compare_exchange_strong_explicit(
                &cell->state_generation, &observed, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            (void)reference_subtract(cell, LCCF_FACT_REF_QUEUE, 1U);
            return EBUSY;
        }
        out_decision->route = LCCF_FACT_ROUTE_QUEUE;
        return 0;
    }

    reasons = queued_defer_reasons(guard);
    if ((reasons & ~LCCF_FACT_ESCAPE_MIGRATION) != 0U) {
        out_decision->route = LCCF_FACT_ROUTE_DEFER;
        out_decision->escape_reasons = reasons;
        return 0;
    }
    if (guard->consuming_shard != guard->current_home_shard) {
        out_decision->route = LCCF_FACT_ROUTE_FORWARD;
        out_decision->escape_reasons |= LCCF_FACT_ESCAPE_WRONG_SHARD;
        counter_increment(&counters->queue_forwards);
        return 0;
    }
    if (reasons != 0U) {
        out_decision->route = LCCF_FACT_ROUTE_DEFER;
        out_decision->escape_reasons = reasons;
        return 0;
    }
    rc = reference_add(cell, LCCF_FACT_REF_CALLBACK, 1U);
    if (rc != 0) {
        return rc;
    }
    desired = lccf_fact_pack_state(
        generation, LCCF_FACT_STATE_RUNNING_QUEUED);
    if (!atomic_compare_exchange_strong_explicit(
            &cell->state_generation, &observed, desired,
            memory_order_acq_rel, memory_order_acquire)) {
        (void)reference_subtract(cell, LCCF_FACT_REF_CALLBACK, 1U);
        return EBUSY;
    }
    rc = reference_subtract(cell, LCCF_FACT_REF_QUEUE, 1U);
    if (rc != 0) {
        return rc;
    }
    out_decision->route = LCCF_FACT_ROUTE_QUEUE;
    out_decision->escape_reasons = 0U;
    return 0;
}

int lccf_fact_yield_to_queue(lccf_fact_cell_t *cell,
                             uint64_t generation) {
    uint64_t observed;
    uint64_t desired;
    int rc;

    if (cell == NULL) {
        return EINVAL;
    }
    observed = atomic_load_explicit(&cell->state_generation,
                                    memory_order_acquire);
    if (lccf_fact_unpack_generation(observed) != generation) {
        return ESTALE;
    }
    if (!state_is_running(lccf_fact_unpack_state(observed))) {
        return EPROTO;
    }
    rc = reference_add(cell, LCCF_FACT_REF_QUEUE, 1U);
    if (rc != 0) {
        return rc;
    }
    desired = lccf_fact_pack_state(generation, LCCF_FACT_STATE_QUEUED);
    if (!atomic_compare_exchange_strong_explicit(
            &cell->state_generation, &observed, desired,
            memory_order_acq_rel, memory_order_acquire)) {
        (void)reference_subtract(cell, LCCF_FACT_REF_QUEUE, 1U);
        return EBUSY;
    }
    rc = reference_subtract(cell, LCCF_FACT_REF_CALLBACK, 1U);
    return rc;
}

int lccf_fact_finish(lccf_fact_cell_t *cell, uint64_t generation,
                     bool terminal, uint64_t next_generation,
                     lccf_fact_counters_t *counters) {
    uint64_t observed;
    uint64_t desired;
    lccf_fact_state_t state;
    uint32_t payload_refs;
    int rc;

    if (cell == NULL || counters == NULL ||
        next_generation > LCCF_FACT_MAX_GENERATION) {
        return EINVAL;
    }
    if (!terminal && next_generation <= generation) {
        return EINVAL;
    }
    observed = atomic_load_explicit(&cell->state_generation,
                                    memory_order_acquire);
    if (lccf_fact_unpack_generation(observed) != generation) {
        return ESTALE;
    }
    state = lccf_fact_unpack_state(observed);
    if (!state_is_running(state)) {
        return EPROTO;
    }
    payload_refs = atomic_load_explicit(
        &cell->references[LCCF_FACT_REF_PAYLOAD], memory_order_acquire);
    if (atomic_load_explicit(&cell->references[LCCF_FACT_REF_CALLBACK],
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&cell->references[LCCF_FACT_REF_QUEUE],
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&cell->references[LCCF_FACT_REF_BACKEND],
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&cell->references[LCCF_FACT_REF_EXTERNAL],
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&cell->references[LCCF_FACT_REF_MODULE],
                             memory_order_acquire) != 1U ||
        payload_refs > 1U) {
        counter_increment(&counters->reuse_delays);
        return EBUSY;
    }
    desired = lccf_fact_pack_state(
        terminal ? generation : next_generation,
        terminal ? LCCF_FACT_STATE_TERMINAL : LCCF_FACT_STATE_ARMED);
    if (!atomic_compare_exchange_strong_explicit(
            &cell->state_generation, &observed, desired,
            memory_order_acq_rel, memory_order_acquire)) {
        return EBUSY;
    }
    cell->published = false;
    cell->shared = false;
    if (payload_refs == 1U) {
        rc = reference_subtract(cell, LCCF_FACT_REF_PAYLOAD, 1U);
        if (rc != 0) {
            return rc;
        }
    }
    rc = reference_subtract(cell, LCCF_FACT_REF_MODULE, 1U);
    if (rc != 0) {
        return rc;
    }
    return reference_subtract(cell, LCCF_FACT_REF_CALLBACK, 1U);
}

int lccf_fact_retain(lccf_fact_cell_t *cell,
                     lccf_fact_ref_kind_t kind) {
    return reference_add(cell, kind, 1U);
}

int lccf_fact_release(lccf_fact_cell_t *cell,
                      lccf_fact_ref_kind_t kind) {
    return reference_subtract(cell, kind, 1U);
}

bool lccf_fact_can_reuse(const lccf_fact_cell_t *cell) {
    uint64_t state_word;
    lccf_fact_state_t state;
    size_t index;

    if (cell == NULL) {
        return false;
    }
    state_word = atomic_load_explicit(&cell->state_generation,
                                      memory_order_acquire);
    state = lccf_fact_unpack_state(state_word);
    if ((state != LCCF_FACT_STATE_ARMED &&
         state != LCCF_FACT_STATE_TERMINAL) ||
        cell->published) {
        return false;
    }
    for (index = 0U; index < LCCF_FACT_REF_COUNT; ++index) {
        if (atomic_load_explicit(&cell->references[index],
                                 memory_order_acquire) != 0U) {
            return false;
        }
    }
    return true;
}

int lccf_fact_module_unregister(const lccf_fact_cell_t *cell) {
    if (cell == NULL) {
        return EINVAL;
    }
    return atomic_load_explicit(
               &cell->references[LCCF_FACT_REF_MODULE],
               memory_order_acquire) == 0U
               ? 0
               : EBUSY;
}
