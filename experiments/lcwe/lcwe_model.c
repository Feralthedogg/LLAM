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

int lcwe_model_batch_create(lcwe_model_workload_t workload,
                            lcwe_model_mode_t mode,
                            size_t instance_count,
                            unsigned site_count,
                            uint64_t seed,
                            lcwe_model_batch_t **out_batch) {
    lcwe_model_batch_t *batch;
    size_t i;

    if (out_batch == NULL) {
        return EINVAL;
    }
    *out_batch = NULL;
    if (!workload_valid(workload) || !mode_valid(mode) ||
        instance_count == 0U || instance_count > UINT32_MAX ||
        site_count == 0U || site_count > LCWE_MODEL_MAX_SITES) {
        return EINVAL;
    }
    if (mode != LCWE_MODEL_SCALAR) {
        return ENOTSUP;
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
    if (batch->frames == NULL || batch->tickets == NULL ||
        batch->ready == NULL) {
        lcwe_model_batch_destroy(batch);
        return ENOMEM;
    }

    batch->workload = workload;
    batch->mode = mode;
    batch->instance_count = instance_count;
    batch->site_count = site_count;
    batch->seed = seed;
    batch->ops = lcwe_model_get_workload_ops(workload);
    if (batch->ops == NULL || batch->ops->resume_one == NULL) {
        lcwe_model_batch_destroy(batch);
        return EPROTO;
    }

    for (i = 0U; i < instance_count; ++i) {
        initialize_frame(batch, i);
    }
    shuffle_ready(batch);
    *out_batch = batch;
    return 0;
}

void lcwe_model_batch_destroy(lcwe_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    free(batch->ready);
    free(batch->tickets);
    free(batch->frames);
    free(batch);
}

static int validate_round(const lcwe_model_batch_t *batch,
                          unsigned lane_width,
                          const lcwe_model_metrics_t *metrics) {
    size_t i;

    if (batch == NULL || metrics == NULL ||
        !lane_width_valid(lane_width)) {
        return EINVAL;
    }
    if (batch->mode != LCWE_MODEL_SCALAR || batch->ops == NULL ||
        batch->ops->resume_one == NULL || batch->round == UINT64_MAX) {
        return EPROTO;
    }
    if ((uint64_t)batch->instance_count >
            UINT64_MAX - metrics->admissions ||
        (uint64_t)batch->instance_count > UINT64_MAX - metrics->tickets ||
        (uint64_t)batch->instance_count >
            UINT64_MAX - metrics->scalar_calls) {
        return EOVERFLOW;
    }
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

int lcwe_model_run_round(lcwe_model_batch_t *batch,
                         unsigned lane_width,
                         lcwe_model_metrics_t *metrics) {
    size_t i;
    int rc = validate_round(batch, lane_width, metrics);

    if (rc != 0) {
        return rc;
    }

    for (i = 0U; i < batch->instance_count; ++i) {
        lcwe_model_ticket_t *ticket = batch->ready[i];
        lcwe_model_frame_t *frame = ticket->frame;

        ticket->lifecycle = LCWE_MODEL_RUNNING;
        batch->ops->resume_one(frame,
                               &ticket->event,
                               &ticket->command,
                               batch->site_count);
        if (ticket->command.kind != 1U ||
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
    }

    batch->round += UINT64_C(1);
    metrics->admissions += (uint64_t)batch->instance_count;
    metrics->tickets += (uint64_t)batch->instance_count;
    metrics->scalar_calls += (uint64_t)batch->instance_count;
    return 0;
}

static bool frame_equal(const lcwe_model_frame_t *lhs,
                        const lcwe_model_frame_t *rhs) {
    return lhs->state0 == rhs->state0 && lhs->state1 == rhs->state1 &&
           lhs->state2 == rhs->state2 && lhs->state3 == rhs->state3 &&
           lhs->output == rhs->output &&
           lhs->generation == rhs->generation && lhs->site == rhs->site &&
           lhs->steps == rhs->steps;
}

static bool ticket_equal(const lcwe_model_ticket_t *lhs,
                         const lcwe_model_ticket_t *rhs) {
    return lhs->event.word0 == rhs->event.word0 &&
           lhs->event.word1 == rhs->event.word1 &&
           lhs->event.kind == rhs->event.kind &&
           lhs->event.reserved == rhs->event.reserved &&
           lhs->command.output == rhs->command.output &&
           lhs->command.next_site == rhs->command.next_site &&
           lhs->command.kind == rhs->command.kind &&
           lhs->generation == rhs->generation && lhs->site == rhs->site &&
           lhs->lifecycle == rhs->lifecycle &&
           lhs->frame_index == rhs->frame_index &&
           lhs->reserved == rhs->reserved;
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
        if (!frame_equal(&lhs->frames[i], &rhs->frames[i]) ||
            !ticket_equal(&lhs->tickets[i], &rhs->tickets[i])) {
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
        const lcwe_model_frame_t *frame = &batch->frames[i];
        const lcwe_model_ticket_t *ticket = &batch->tickets[i];

        hash = hash_word(hash, frame->state0);
        hash = hash_word(hash, frame->state1);
        hash = hash_word(hash, frame->state2);
        hash = hash_word(hash, frame->state3);
        hash = hash_word(hash, frame->output);
        hash = hash_word(hash, frame->generation);
        hash = hash_word(hash,
                         ((uint64_t)frame->site << 32U) | frame->steps);
        hash = hash_word(hash, ticket->event.word0);
        hash = hash_word(hash, ticket->event.word1);
        hash = hash_word(hash, ticket->command.output);
        hash = hash_word(hash,
                         ((uint64_t)ticket->command.next_site << 32U) |
                             ticket->command.kind);
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
