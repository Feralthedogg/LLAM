// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lcwe_model_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *const MODE_NAMES[] = {
    "scalar",
    "cohort",
    "wave_pointers",
    "wave_capsule",
    "wave_aosoa",
};

static const char *const WORKLOAD_NAMES[] = {
    "exec_io_pipeline",
    "exec_rpc_state",
    "exec_event_fanout",
};

typedef struct canonical_state {
    uint64_t state0;
    uint64_t state1;
    uint64_t state2;
    uint64_t state3;
    uint64_t output;
    uint64_t frame_generation;
    uint64_t event0;
    uint64_t event1;
    uint64_t command_output;
    uint64_t ticket_generation;
    uint32_t frame_site;
    uint32_t steps;
    uint32_t event_kind;
    uint32_t event_reserved;
    uint32_t next_site;
    uint32_t command_kind;
    uint32_t ticket_site;
    uint32_t lifecycle;
    uint32_t frame_index;
    uint32_t ticket_reserved;
} canonical_state_t;

static bool mode_valid(lcwe_model_mode_t mode) {
    return mode >= LCWE_MODEL_SCALAR && mode <= LCWE_MODEL_WAVE_AOSOA;
}

static bool workload_valid(lcwe_model_workload_t workload) {
    return workload >= LCWE_MODEL_EXEC_IO_PIPELINE &&
           workload <= LCWE_MODEL_EXEC_EVENT_FANOUT;
}

static bool lane_width_valid(unsigned lane_width) {
    return lane_width == 1U || lane_width == 2U || lane_width == 4U ||
           lane_width == 8U || lane_width == 16U || lane_width == 32U;
}

uint64_t lcwe_model_mix64(uint64_t value) {
    value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

static uint64_t splitmix64_next(uint64_t *state) {
    *state += UINT64_C(0x9E3779B97F4A7C15);
    return lcwe_model_mix64(*state);
}

void lcwe_model_derive_event(uint64_t seed,
                             uint32_t frame_index,
                             const lcwe_model_frame_t *frame,
                             lcwe_model_event_t *event) {
    event->word0 =
        lcwe_model_mix64(seed ^ ((uint64_t)frame_index << 32U) ^
                         frame->generation);
    event->word1 =
        lcwe_model_mix64(event->word0 ^ frame->output ^
                         UINT64_C(0xA0761D6478BD642F));
    event->kind = (uint32_t)(event->word1 & UINT64_C(3));
    event->reserved = 0U;
}

static bool allocation_size_valid(size_t count, size_t element_size) {
    return element_size == 0U || count <= SIZE_MAX / element_size;
}

static void initialize_frame(lcwe_model_batch_t *batch, size_t index) {
    lcwe_model_frame_t *frame = &batch->frames[index];
    lcwe_model_ticket_t *ticket = &batch->tickets[index];
    const uint64_t identity =
        batch->seed ^ ((uint64_t)(uint32_t)index << 32U) ^ (uint64_t)index;

    frame->state0 =
        lcwe_model_mix64(identity ^ UINT64_C(0x243F6A8885A308D3));
    frame->state1 =
        lcwe_model_mix64(identity ^ UINT64_C(0x13198A2E03707344));
    frame->state2 =
        lcwe_model_mix64(identity ^ UINT64_C(0xA4093822299F31D0));
    frame->state3 =
        lcwe_model_mix64(identity ^ UINT64_C(0x082EFA98EC4E6C89));
    frame->output =
        lcwe_model_mix64(identity ^ UINT64_C(0x452821E638D01377));
    frame->generation = UINT64_C(1);
    frame->site = (uint32_t)(index % batch->site_count);
    frame->steps = 0U;

    ticket->frame = frame;
    ticket->generation = frame->generation;
    ticket->site = frame->site;
    ticket->lifecycle = LCWE_MODEL_READY;
    ticket->frame_index = (uint32_t)index;
    ticket->reserved = 0U;
    ticket->command.output = 0U;
    ticket->command.next_site = frame->site;
    ticket->command.kind = 0U;
    lcwe_model_derive_event(batch->seed,
                            ticket->frame_index,
                            frame,
                            &ticket->event);
    batch->ready[index] = ticket;
}

static void shuffle_ready(lcwe_model_batch_t *batch) {
    uint64_t state =
        batch->seed ^ UINT64_C(0xD1B54A32D192ED03) ^
        (uint64_t)batch->instance_count;
    size_t i;

    for (i = batch->instance_count; i > 1U; --i) {
        const size_t j = (size_t)(splitmix64_next(&state) % (uint64_t)i);
        lcwe_model_ticket_t *const tmp = batch->ready[i - 1U];

        batch->ready[i - 1U] = batch->ready[j];
        batch->ready[j] = tmp;
    }
}

static void aosoa_destroy(lcwe_model_aosoa_t *aosoa) {
    if (aosoa == NULL) {
        return;
    }
    free(aosoa->lifecycle);
    free(aosoa->command_kind);
    free(aosoa->next_site);
    free(aosoa->event_kind);
    free(aosoa->steps);
    free(aosoa->site);
    free(aosoa->command_output);
    free(aosoa->event1);
    free(aosoa->event0);
    free(aosoa->generation);
    free(aosoa->output);
    free(aosoa->state3);
    free(aosoa->state2);
    free(aosoa->state1);
    free(aosoa->state0);
    free(aosoa);
}

static int aosoa_create(lcwe_model_batch_t *batch) {
    lcwe_model_aosoa_t *aosoa = calloc(1U, sizeof(*aosoa));
    const size_t count = batch->instance_count;
    size_t i;

    if (aosoa == NULL) {
        return ENOMEM;
    }
#define LCWE_ALLOC_AOSOA(field)                                               \
    do {                                                                       \
        aosoa->field = calloc(count, sizeof(*aosoa->field));                   \
        if (aosoa->field == NULL) {                                            \
            aosoa_destroy(aosoa);                                              \
            return ENOMEM;                                                     \
        }                                                                      \
    } while (false)

    LCWE_ALLOC_AOSOA(state0);
    LCWE_ALLOC_AOSOA(state1);
    LCWE_ALLOC_AOSOA(state2);
    LCWE_ALLOC_AOSOA(state3);
    LCWE_ALLOC_AOSOA(output);
    LCWE_ALLOC_AOSOA(generation);
    LCWE_ALLOC_AOSOA(event0);
    LCWE_ALLOC_AOSOA(event1);
    LCWE_ALLOC_AOSOA(command_output);
    LCWE_ALLOC_AOSOA(site);
    LCWE_ALLOC_AOSOA(steps);
    LCWE_ALLOC_AOSOA(event_kind);
    LCWE_ALLOC_AOSOA(next_site);
    LCWE_ALLOC_AOSOA(command_kind);
    LCWE_ALLOC_AOSOA(lifecycle);
#undef LCWE_ALLOC_AOSOA

    for (i = 0U; i < count; ++i) {
        const lcwe_model_frame_t *frame = &batch->frames[i];
        const lcwe_model_ticket_t *ticket = &batch->tickets[i];

        aosoa->state0[i] = frame->state0;
        aosoa->state1[i] = frame->state1;
        aosoa->state2[i] = frame->state2;
        aosoa->state3[i] = frame->state3;
        aosoa->output[i] = frame->output;
        aosoa->generation[i] = frame->generation;
        aosoa->event0[i] = ticket->event.word0;
        aosoa->event1[i] = ticket->event.word1;
        aosoa->command_output[i] = ticket->command.output;
        aosoa->site[i] = frame->site;
        aosoa->steps[i] = frame->steps;
        aosoa->event_kind[i] = ticket->event.kind;
        aosoa->next_site[i] = ticket->command.next_site;
        aosoa->command_kind[i] = ticket->command.kind;
        aosoa->lifecycle[i] = ticket->lifecycle;
    }
    batch->aosoa = aosoa;
    return 0;
}

static bool ops_valid(const lcwe_model_batch_t *batch) {
    if (batch->ops == NULL || batch->ops->resume_one == NULL) {
        return false;
    }
    switch (batch->mode) {
        case LCWE_MODEL_SCALAR:
        case LCWE_MODEL_COHORT:
            return true;
        case LCWE_MODEL_WAVE_POINTERS:
            return batch->ops->resume_pointers != NULL;
        case LCWE_MODEL_WAVE_CAPSULE:
            return batch->ops->resume_capsule != NULL;
        case LCWE_MODEL_WAVE_AOSOA:
            return batch->ops->resume_aosoa != NULL;
        default:
            return false;
    }
}

int lcwe_model_batch_create(lcwe_model_workload_t workload,
                            lcwe_model_mode_t mode,
                            size_t instance_count,
                            unsigned site_count,
                            uint64_t seed,
                            lcwe_model_batch_t **out_batch) {
    lcwe_model_batch_t *batch;
    size_t i;
    int rc;

    if (out_batch == NULL) {
        return EINVAL;
    }
    *out_batch = NULL;
    if (!workload_valid(workload) || !mode_valid(mode) ||
        instance_count == 0U || instance_count > UINT32_MAX ||
        site_count == 0U || site_count > LCWE_MODEL_MAX_SITES ||
        (mode == LCWE_MODEL_WAVE_AOSOA && site_count != 1U)) {
        return EINVAL;
    }
    if (!allocation_size_valid(instance_count, sizeof(*batch->frames)) ||
        !allocation_size_valid(instance_count, sizeof(*batch->tickets)) ||
        !allocation_size_valid(instance_count, sizeof(*batch->ready))) {
        return EOVERFLOW;
    }

    batch = calloc(1U, sizeof(*batch));
    if (batch == NULL) {
        return ENOMEM;
    }
    batch->frames = calloc(instance_count, sizeof(*batch->frames));
    batch->tickets = calloc(instance_count, sizeof(*batch->tickets));
    batch->ready = calloc(instance_count, sizeof(*batch->ready));
    if (mode != LCWE_MODEL_SCALAR) {
        batch->grouped = calloc(instance_count, sizeof(*batch->grouped));
    }
    if (batch->frames == NULL || batch->tickets == NULL ||
        batch->ready == NULL ||
        (mode != LCWE_MODEL_SCALAR && batch->grouped == NULL)) {
        lcwe_model_batch_destroy(batch);
        return ENOMEM;
    }

    batch->workload = workload;
    batch->mode = mode;
    batch->instance_count = instance_count;
    batch->site_count = site_count;
    batch->seed = seed;
    batch->ops = lcwe_model_get_workload_ops(workload);
    if (!ops_valid(batch)) {
        lcwe_model_batch_destroy(batch);
        return EPROTO;
    }

    for (i = 0U; i < instance_count; ++i) {
        initialize_frame(batch, i);
    }
    shuffle_ready(batch);
    if (mode == LCWE_MODEL_WAVE_AOSOA) {
        rc = aosoa_create(batch);
        if (rc != 0) {
            lcwe_model_batch_destroy(batch);
            return rc;
        }
    }
    *out_batch = batch;
    return 0;
}

void lcwe_model_batch_destroy(lcwe_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    aosoa_destroy(batch->aosoa);
    free(batch->grouped);
    free(batch->ready);
    free(batch->tickets);
    free(batch->frames);
    free(batch);
}

static bool metric_has_room(uint64_t current, size_t increment) {
    return (uint64_t)increment <= UINT64_MAX - current;
}

static int validate_regular_tickets(const lcwe_model_batch_t *batch) {
    size_t i;

    for (i = 0U; i < batch->instance_count; ++i) {
        const lcwe_model_ticket_t *ticket = batch->ready[i];

        if (ticket == NULL || ticket->frame == NULL ||
            ticket->frame_index >= batch->instance_count ||
            ticket != &batch->tickets[ticket->frame_index] ||
            ticket->frame != &batch->frames[ticket->frame_index] ||
            ticket->lifecycle != LCWE_MODEL_READY ||
            ticket->generation == 0U ||
            ticket->generation != ticket->frame->generation ||
            ticket->site != ticket->frame->site ||
            ticket->site >= batch->site_count) {
            return EPROTO;
        }
        if (ticket->generation == UINT64_MAX) {
            return EOVERFLOW;
        }
    }
    return 0;
}

static int validate_aosoa(const lcwe_model_batch_t *batch) {
    size_t i;

    if (batch->aosoa == NULL || batch->site_count != 1U) {
        return EPROTO;
    }
    for (i = 0U; i < batch->instance_count; ++i) {
        if (batch->aosoa->lifecycle[i] != LCWE_MODEL_READY ||
            batch->aosoa->generation[i] == 0U ||
            batch->aosoa->site[i] >= batch->site_count) {
            return EPROTO;
        }
        if (batch->aosoa->generation[i] == UINT64_MAX) {
            return EOVERFLOW;
        }
    }
    return 0;
}

static int validate_metric_capacity(const lcwe_model_batch_t *batch,
                                    const lcwe_model_metrics_t *metrics) {
    const size_t count = batch->instance_count;

    if (!metric_has_room(metrics->admissions, count) ||
        !metric_has_room(metrics->tickets, count)) {
        return EOVERFLOW;
    }
    if ((batch->mode == LCWE_MODEL_SCALAR ||
         batch->mode == LCWE_MODEL_COHORT) &&
        !metric_has_room(metrics->scalar_calls, count)) {
        return EOVERFLOW;
    }
    if (batch->mode >= LCWE_MODEL_WAVE_POINTERS &&
        !metric_has_room(metrics->wave_calls, count)) {
        return EOVERFLOW;
    }
    if ((batch->mode == LCWE_MODEL_WAVE_POINTERS &&
         !metric_has_room(metrics->pointer_lanes, count)) ||
        (batch->mode == LCWE_MODEL_WAVE_CAPSULE &&
         !metric_has_room(metrics->capsule_lanes, count)) ||
        (batch->mode == LCWE_MODEL_WAVE_AOSOA &&
         !metric_has_room(metrics->aosoa_lanes, count))) {
        return EOVERFLOW;
    }
    return 0;
}

static int validate_round(const lcwe_model_batch_t *batch,
                          unsigned lane_width,
                          const lcwe_model_metrics_t *metrics) {
    int rc;

    if (batch == NULL || metrics == NULL ||
        !lane_width_valid(lane_width)) {
        return EINVAL;
    }
    if (!mode_valid(batch->mode) || !ops_valid(batch) ||
        batch->round == UINT64_MAX ||
        (batch->mode != LCWE_MODEL_SCALAR && batch->grouped == NULL)) {
        return EPROTO;
    }
    rc = validate_metric_capacity(batch, metrics);
    if (rc != 0) {
        return rc;
    }
    return batch->mode == LCWE_MODEL_WAVE_AOSOA ?
               validate_aosoa(batch) :
               validate_regular_tickets(batch);
}

static uint32_t ticket_site(const lcwe_model_batch_t *batch,
                            const lcwe_model_ticket_t *ticket) {
    if (batch->mode == LCWE_MODEL_WAVE_AOSOA) {
        return batch->aosoa->site[ticket->frame_index];
    }
    return ticket->site;
}

static int group_ready(lcwe_model_batch_t *batch) {
    size_t i;
    unsigned site;

    memset(batch->site_counts, 0, sizeof(batch->site_counts));
    for (i = 0U; i < batch->instance_count; ++i) {
        const uint32_t site_index = ticket_site(batch, batch->ready[i]);

        if (site_index >= batch->site_count) {
            return EPROTO;
        }
        ++batch->site_counts[site_index];
    }
    batch->site_offsets[0] = 0U;
    for (site = 0U; site < batch->site_count; ++site) {
        batch->site_offsets[site + 1U] =
            batch->site_offsets[site] + batch->site_counts[site];
        batch->site_cursor[site] = batch->site_offsets[site];
    }
    for (i = 0U; i < batch->instance_count; ++i) {
        lcwe_model_ticket_t *ticket = batch->ready[i];
        const uint32_t site_index = ticket_site(batch, ticket);

        batch->grouped[batch->site_cursor[site_index]++] = ticket;
    }
    return 0;
}

static int complete_ticket(lcwe_model_batch_t *batch,
                           lcwe_model_ticket_t *ticket) {
    lcwe_model_frame_t *frame = ticket->frame;

    if (ticket->lifecycle != LCWE_MODEL_RUNNING ||
        ticket->command.kind != 1U ||
        ticket->command.output != frame->output ||
        ticket->command.next_site >= batch->site_count) {
        return EPROTO;
    }
    frame->generation += UINT64_C(1);
    ticket->generation = frame->generation;
    frame->site = ticket->command.next_site;
    ticket->site = frame->site;
    lcwe_model_derive_event(batch->seed,
                            ticket->frame_index,
                            frame,
                            &ticket->event);
    ticket->lifecycle = LCWE_MODEL_READY;
    return 0;
}

static int run_scalar(lcwe_model_batch_t *batch) {
    size_t i;

    for (i = 0U; i < batch->instance_count; ++i) {
        lcwe_model_ticket_t *ticket = batch->ready[i];

        ticket->lifecycle = LCWE_MODEL_RUNNING;
        batch->ops->resume_one(ticket->frame,
                               &ticket->event,
                               &ticket->command,
                               batch->site_count);
        if (complete_ticket(batch, ticket) != 0) {
            return EPROTO;
        }
    }
    return 0;
}

static int run_cohort(lcwe_model_batch_t *batch) {
    unsigned site;

    for (site = 0U; site < batch->site_count; ++site) {
        size_t i;

        for (i = batch->site_offsets[site];
             i < batch->site_offsets[site + 1U];
             ++i) {
            lcwe_model_ticket_t *ticket = batch->grouped[i];

            ticket->lifecycle = LCWE_MODEL_RUNNING;
            batch->ops->resume_one(ticket->frame,
                                   &ticket->event,
                                   &ticket->command,
                                   batch->site_count);
            if (complete_ticket(batch, ticket) != 0) {
                return EPROTO;
            }
        }
    }
    return 0;
}

static int run_pointer_waves(lcwe_model_batch_t *batch,
                             unsigned lane_width,
                             uint64_t *wave_calls) {
    unsigned site;

    for (site = 0U; site < batch->site_count; ++site) {
        size_t first;

        for (first = batch->site_offsets[site];
             first < batch->site_offsets[site + 1U];
             first += lane_width) {
            const size_t remaining =
                batch->site_offsets[site + 1U] - first;
            const unsigned lanes =
                remaining < lane_width ? (unsigned)remaining : lane_width;
            unsigned lane;

            for (lane = 0U; lane < lanes; ++lane) {
                batch->grouped[first + lane]->lifecycle =
                    LCWE_MODEL_RUNNING;
            }
            batch->ops->resume_pointers(&batch->grouped[first],
                                        lanes,
                                        batch->site_count);
            ++*wave_calls;
            for (lane = 0U; lane < lanes; ++lane) {
                if (complete_ticket(batch,
                                    batch->grouped[first + lane]) != 0) {
                    return EPROTO;
                }
            }
        }
    }
    return 0;
}

static void pack_capsule(lcwe_model_batch_t *batch,
                         size_t first,
                         unsigned lane_count) {
    unsigned lane;

    for (lane = 0U; lane < lane_count; ++lane) {
        lcwe_model_ticket_t *ticket = batch->grouped[first + lane];
        lcwe_model_frame_t *frame = ticket->frame;

        ticket->lifecycle = LCWE_MODEL_RUNNING;
        batch->capsule.state0[lane] = frame->state0;
        batch->capsule.state1[lane] = frame->state1;
        batch->capsule.state2[lane] = frame->state2;
        batch->capsule.state3[lane] = frame->state3;
        batch->capsule.event0[lane] = ticket->event.word0;
        batch->capsule.event1[lane] = ticket->event.word1;
        batch->capsule.output[lane] = frame->output;
        batch->capsule.site[lane] = frame->site;
        batch->capsule.next_site[lane] = frame->site;
        batch->capsule.steps[lane] = frame->steps;
    }
}

static int scatter_capsule(lcwe_model_batch_t *batch,
                           size_t first,
                           unsigned lane_count) {
    unsigned lane;

    for (lane = 0U; lane < lane_count; ++lane) {
        lcwe_model_ticket_t *ticket = batch->grouped[first + lane];
        lcwe_model_frame_t *frame = ticket->frame;

        frame->state0 = batch->capsule.state0[lane];
        frame->state1 = batch->capsule.state1[lane];
        frame->state2 = batch->capsule.state2[lane];
        frame->state3 = batch->capsule.state3[lane];
        frame->output = batch->capsule.output[lane];
        frame->steps = batch->capsule.steps[lane];
        ticket->command.output = batch->capsule.output[lane];
        ticket->command.next_site = batch->capsule.next_site[lane];
        ticket->command.kind = 1U;
        if (complete_ticket(batch, ticket) != 0) {
            return EPROTO;
        }
    }
    return 0;
}

static int run_capsule_waves(lcwe_model_batch_t *batch,
                             unsigned lane_width,
                             uint64_t *wave_calls) {
    unsigned site;

    for (site = 0U; site < batch->site_count; ++site) {
        size_t first;

        for (first = batch->site_offsets[site];
             first < batch->site_offsets[site + 1U];
             first += lane_width) {
            const size_t remaining =
                batch->site_offsets[site + 1U] - first;
            const unsigned lanes =
                remaining < lane_width ? (unsigned)remaining : lane_width;

            pack_capsule(batch, first, lanes);
            batch->ops->resume_capsule(&batch->capsule,
                                       lanes,
                                       batch->site_count);
            ++*wave_calls;
            if (scatter_capsule(batch, first, lanes) != 0) {
                return EPROTO;
            }
        }
    }
    return 0;
}

static void derive_aosoa_event(lcwe_model_batch_t *batch, size_t index) {
    lcwe_model_aosoa_t *aosoa = batch->aosoa;

    aosoa->event0[index] =
        lcwe_model_mix64(batch->seed ^ ((uint64_t)(uint32_t)index << 32U) ^
                         aosoa->generation[index]);
    aosoa->event1[index] =
        lcwe_model_mix64(aosoa->event0[index] ^ aosoa->output[index] ^
                         UINT64_C(0xA0761D6478BD642F));
    aosoa->event_kind[index] =
        (uint32_t)(aosoa->event1[index] & UINT64_C(3));
}

static int run_aosoa_waves(lcwe_model_batch_t *batch,
                           unsigned lane_width,
                           uint64_t *wave_calls) {
    size_t first;

    for (first = 0U; first < batch->instance_count;
         first += lane_width) {
        const size_t remaining = batch->instance_count - first;
        const unsigned lanes =
            remaining < lane_width ? (unsigned)remaining : lane_width;
        unsigned lane;

        for (lane = 0U; lane < lanes; ++lane) {
            batch->aosoa->lifecycle[first + lane] =
                LCWE_MODEL_RUNNING;
        }
        batch->ops->resume_aosoa(batch->aosoa,
                                 first,
                                 lanes,
                                 batch->site_count);
        ++*wave_calls;
        for (lane = 0U; lane < lanes; ++lane) {
            const size_t index = first + lane;

            batch->aosoa->command_kind[index] = 1U;
            if (batch->aosoa->command_output[index] !=
                    batch->aosoa->output[index] ||
                batch->aosoa->next_site[index] >= batch->site_count) {
                return EPROTO;
            }
            batch->aosoa->generation[index] += UINT64_C(1);
            batch->aosoa->site[index] =
                batch->aosoa->next_site[index];
            derive_aosoa_event(batch, index);
            batch->aosoa->lifecycle[index] = LCWE_MODEL_READY;
        }
    }
    return 0;
}

int lcwe_model_run_round(lcwe_model_batch_t *batch,
                         unsigned lane_width,
                         lcwe_model_metrics_t *metrics) {
    uint64_t wave_calls = 0U;
    int rc = validate_round(batch, lane_width, metrics);

    if (rc != 0) {
        return rc;
    }
    if (batch->mode != LCWE_MODEL_SCALAR) {
        rc = group_ready(batch);
        if (rc != 0) {
            return rc;
        }
    }

    switch (batch->mode) {
        case LCWE_MODEL_SCALAR:
            rc = run_scalar(batch);
            break;
        case LCWE_MODEL_COHORT:
            rc = run_cohort(batch);
            break;
        case LCWE_MODEL_WAVE_POINTERS:
            rc = run_pointer_waves(batch, lane_width, &wave_calls);
            break;
        case LCWE_MODEL_WAVE_CAPSULE:
            rc = run_capsule_waves(batch, lane_width, &wave_calls);
            break;
        case LCWE_MODEL_WAVE_AOSOA:
            rc = run_aosoa_waves(batch, lane_width, &wave_calls);
            break;
        default:
            rc = EPROTO;
            break;
    }
    if (rc != 0) {
        return rc;
    }

    batch->round += UINT64_C(1);
    metrics->admissions += (uint64_t)batch->instance_count;
    metrics->tickets += (uint64_t)batch->instance_count;
    if (batch->mode == LCWE_MODEL_SCALAR ||
        batch->mode == LCWE_MODEL_COHORT) {
        metrics->scalar_calls += (uint64_t)batch->instance_count;
    } else {
        metrics->wave_calls += wave_calls;
    }
    if (batch->mode == LCWE_MODEL_WAVE_POINTERS) {
        metrics->pointer_lanes += (uint64_t)batch->instance_count;
    } else if (batch->mode == LCWE_MODEL_WAVE_CAPSULE) {
        metrics->capsule_lanes += (uint64_t)batch->instance_count;
    } else if (batch->mode == LCWE_MODEL_WAVE_AOSOA) {
        metrics->aosoa_lanes += (uint64_t)batch->instance_count;
    }
    return 0;
}

static void canonical_at(const lcwe_model_batch_t *batch,
                         size_t index,
                         canonical_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->frame_index = (uint32_t)index;
    if (batch->mode == LCWE_MODEL_WAVE_AOSOA) {
        const lcwe_model_aosoa_t *aosoa = batch->aosoa;

        state->state0 = aosoa->state0[index];
        state->state1 = aosoa->state1[index];
        state->state2 = aosoa->state2[index];
        state->state3 = aosoa->state3[index];
        state->output = aosoa->output[index];
        state->frame_generation = aosoa->generation[index];
        state->event0 = aosoa->event0[index];
        state->event1 = aosoa->event1[index];
        state->command_output = aosoa->command_output[index];
        state->ticket_generation = aosoa->generation[index];
        state->frame_site = aosoa->site[index];
        state->steps = aosoa->steps[index];
        state->event_kind = aosoa->event_kind[index];
        state->next_site = aosoa->next_site[index];
        state->command_kind = aosoa->command_kind[index];
        state->ticket_site = aosoa->site[index];
        state->lifecycle = aosoa->lifecycle[index];
        return;
    }

    {
        const lcwe_model_frame_t *frame = &batch->frames[index];
        const lcwe_model_ticket_t *ticket = &batch->tickets[index];

        state->state0 = frame->state0;
        state->state1 = frame->state1;
        state->state2 = frame->state2;
        state->state3 = frame->state3;
        state->output = frame->output;
        state->frame_generation = frame->generation;
        state->event0 = ticket->event.word0;
        state->event1 = ticket->event.word1;
        state->command_output = ticket->command.output;
        state->ticket_generation = ticket->generation;
        state->frame_site = frame->site;
        state->steps = frame->steps;
        state->event_kind = ticket->event.kind;
        state->event_reserved = ticket->event.reserved;
        state->next_site = ticket->command.next_site;
        state->command_kind = ticket->command.kind;
        state->ticket_site = ticket->site;
        state->lifecycle = ticket->lifecycle;
        state->frame_index = ticket->frame_index;
        state->ticket_reserved = ticket->reserved;
    }
}

static bool canonical_equal(const canonical_state_t *lhs,
                            const canonical_state_t *rhs) {
    return lhs->state0 == rhs->state0 && lhs->state1 == rhs->state1 &&
           lhs->state2 == rhs->state2 && lhs->state3 == rhs->state3 &&
           lhs->output == rhs->output &&
           lhs->frame_generation == rhs->frame_generation &&
           lhs->event0 == rhs->event0 && lhs->event1 == rhs->event1 &&
           lhs->command_output == rhs->command_output &&
           lhs->ticket_generation == rhs->ticket_generation &&
           lhs->frame_site == rhs->frame_site &&
           lhs->steps == rhs->steps &&
           lhs->event_kind == rhs->event_kind &&
           lhs->event_reserved == rhs->event_reserved &&
           lhs->next_site == rhs->next_site &&
           lhs->command_kind == rhs->command_kind &&
           lhs->ticket_site == rhs->ticket_site &&
           lhs->lifecycle == rhs->lifecycle &&
           lhs->frame_index == rhs->frame_index &&
           lhs->ticket_reserved == rhs->ticket_reserved;
}

bool lcwe_model_batch_equal(const lcwe_model_batch_t *lhs,
                            const lcwe_model_batch_t *rhs) {
    size_t i;

    if (lhs == NULL || rhs == NULL || lhs->workload != rhs->workload ||
        lhs->instance_count != rhs->instance_count ||
        lhs->site_count != rhs->site_count || lhs->round != rhs->round) {
        return false;
    }
    for (i = 0U; i < lhs->instance_count; ++i) {
        canonical_state_t lhs_state;
        canonical_state_t rhs_state;

        canonical_at(lhs, i, &lhs_state);
        canonical_at(rhs, i, &rhs_state);
        if (!canonical_equal(&lhs_state, &rhs_state)) {
            return false;
        }
    }
    return true;
}

static uint64_t hash_word(uint64_t hash, uint64_t word) {
    return lcwe_model_mix64(hash ^ word ^ UINT64_C(0x9E3779B97F4A7C15));
}

uint64_t lcwe_model_checksum(const lcwe_model_batch_t *batch) {
    uint64_t hash = UINT64_C(0x6C6C616D77617665);
    size_t i;

    if (batch == NULL) {
        return 0U;
    }
    hash = hash_word(hash, (uint64_t)(unsigned)batch->workload);
    hash = hash_word(hash, (uint64_t)batch->instance_count);
    hash = hash_word(hash, batch->round);
    for (i = 0U; i < batch->instance_count; ++i) {
        canonical_state_t state;

        canonical_at(batch, i, &state);
        hash = hash_word(hash, state.state0);
        hash = hash_word(hash, state.state1);
        hash = hash_word(hash, state.state2);
        hash = hash_word(hash, state.state3);
        hash = hash_word(hash, state.output);
        hash = hash_word(hash, state.frame_generation);
        hash = hash_word(hash,
                         ((uint64_t)state.frame_site << 32U) |
                             state.steps);
        hash = hash_word(hash, state.event0);
        hash = hash_word(hash, state.event1);
        hash = hash_word(hash,
                         ((uint64_t)state.event_kind << 32U) |
                             state.event_reserved);
        hash = hash_word(hash, state.command_output);
        hash = hash_word(hash,
                         ((uint64_t)state.next_site << 32U) |
                             state.command_kind);
        hash = hash_word(hash, state.ticket_generation);
        hash = hash_word(hash,
                         ((uint64_t)state.ticket_site << 32U) |
                             state.lifecycle);
    }
    return hash == 0U ? UINT64_C(1) : hash;
}

const char *lcwe_model_mode_name(lcwe_model_mode_t mode) {
    return mode_valid(mode) ? MODE_NAMES[(unsigned)mode] : NULL;
}

const char *lcwe_model_workload_name(lcwe_model_workload_t workload) {
    return workload_valid(workload) ?
               WORKLOAD_NAMES[(unsigned)workload] :
               NULL;
}

int lcwe_model_parse_mode(const char *text, lcwe_model_mode_t *out) {
    unsigned i;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (i = 0U; i < sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]); ++i) {
        if (strcmp(text, MODE_NAMES[i]) == 0) {
            *out = (lcwe_model_mode_t)i;
            return 0;
        }
    }
    return EINVAL;
}

int lcwe_model_parse_workload(const char *text,
                              lcwe_model_workload_t *out) {
    unsigned i;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (i = 0U; i < sizeof(WORKLOAD_NAMES) / sizeof(WORKLOAD_NAMES[0]); ++i) {
        if (strcmp(text, WORKLOAD_NAMES[i]) == 0) {
            *out = (lcwe_model_workload_t)i;
            return 0;
        }
    }
    return EINVAL;
}
