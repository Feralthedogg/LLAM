// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *const MODE_NAMES[] = {
    "waker_frame",
    "tile_scalar",
    "tile_vector",
    "adaptive_srem",
    "remote_waker_frame",
    "remote_adaptive_srem",
};

static const char *const WORKLOAD_NAMES[] = {
    "srem_http_pipeline",
    "srem_rpc_pipeline",
    "srem_divergent_cancel",
    "srem_mixed_fairness",
};

static bool size_mul_overflow(size_t lhs,
                              size_t rhs,
                              size_t *out_product) {
    if (lhs != 0U && rhs > SIZE_MAX / lhs) {
        return true;
    }
    *out_product = lhs * rhs;
    return false;
}

static bool mode_is_remote(srem_model_mode_t mode) {
    return mode == SREM_MODEL_REMOTE_WAKER_FRAME ||
           mode == SREM_MODEL_REMOTE_ADAPTIVE;
}

static bool valid_frame_bytes(size_t frame_bytes) {
    return frame_bytes == 64U || frame_bytes == 128U ||
           frame_bytes == 256U;
}

static bool valid_tile_width(unsigned tile_width) {
    return tile_width == 8U || tile_width == 16U ||
           tile_width == 32U;
}

static bool valid_divergence(unsigned divergence_eighths) {
    return divergence_eighths == 0U ||
           divergence_eighths == 1U ||
           divergence_eighths == 4U;
}

static int validate_config(const srem_model_config_t *config) {
    size_t ignored;

    if (config == NULL ||
        config->workload < SREM_MODEL_HTTP_PIPELINE ||
        config->workload > SREM_MODEL_MIXED_FAIRNESS ||
        config->mode < SREM_MODEL_WAKER_FRAME ||
        config->mode > SREM_MODEL_REMOTE_ADAPTIVE ||
        config->instance_count == 0U ||
        config->instance_count > UINT32_MAX ||
        !valid_frame_bytes(config->frame_bytes) ||
        !valid_tile_width(config->tile_width) ||
        config->active_lanes == 0U ||
        config->active_lanes > config->tile_width ||
        (config->site_count != 1U && config->site_count != 8U) ||
        !valid_divergence(config->divergence_eighths) ||
        config->vector_threshold == 0U ||
        config->vector_threshold > config->tile_width) {
        return EINVAL;
    }
    if (mode_is_remote(config->mode)) {
        if (config->remote_producers !=
            SREM_MODEL_REMOTE_PRODUCER_COUNT) {
            return EINVAL;
        }
    } else if (config->remote_producers != 0U) {
        return EINVAL;
    }
    if (size_mul_overflow(config->instance_count,
                          config->frame_bytes,
                          &ignored) ||
        size_mul_overflow(config->instance_count,
                          sizeof(srem_model_effect_t),
                          &ignored) ||
        size_mul_overflow(config->instance_count,
                          sizeof(srem_model_event_t),
                          &ignored) ||
        size_mul_overflow(config->instance_count,
                          sizeof(srem_model_waker_t),
                          &ignored) ||
        size_mul_overflow(config->instance_count,
                          sizeof(srem_model_ticket_t),
                          &ignored)) {
        return EINVAL;
    }
    return 0;
}

static int queue_capacity_for(size_t count, size_t *out_capacity) {
    size_t capacity = 2U;

    if (out_capacity == NULL || count == SIZE_MAX) {
        return EINVAL;
    }
    while (capacity <= count) {
        if (capacity > SIZE_MAX / 2U) {
            return EINVAL;
        }
        capacity *= 2U;
    }
    *out_capacity = capacity;
    return 0;
}

static bool queue_empty(const srem_model_index_queue_t *queue) {
    return queue->head == queue->tail;
}

static int queue_push(srem_model_index_queue_t *queue, uint32_t index) {
    if (queue->tail - queue->head >= queue->capacity) {
        return ENOSPC;
    }
    queue->slots[queue->tail & queue->mask] = index;
    queue->tail += 1U;
    return 0;
}

static int queue_pop(srem_model_index_queue_t *queue,
                     uint32_t *out_index) {
    if (queue_empty(queue) || out_index == NULL) {
        return ENOENT;
    }
    *out_index = queue->slots[queue->head & queue->mask];
    queue->head += 1U;
    return 0;
}

uint64_t srem_model_mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31U;
    return value;
}

uint64_t srem_model_pack_waker(uint32_t generation,
                               srem_model_waker_state_t state) {
    return ((uint64_t)generation << 32U) | (uint64_t)(unsigned)state;
}

uint32_t srem_model_unpack_generation(uint64_t word) {
    return (uint32_t)(word >> 32U);
}

srem_model_waker_state_t srem_model_unpack_waker_state(uint64_t word) {
    return (srem_model_waker_state_t)(unsigned)(word & UINT64_C(0xFF));
}

srem_model_frame_core_t *srem_model_frame_at(
    const srem_model_batch_t *batch,
    size_t index) {
    if (batch == NULL || index >= batch->config.instance_count) {
        return NULL;
    }
    return (srem_model_frame_core_t *)(
        batch->frame_storage + index * batch->config.frame_bytes);
}

void srem_model_derive_event(const srem_model_batch_t *batch,
                             size_t index,
                             unsigned site,
                             srem_model_event_t *event) {
    const unsigned lane = (unsigned)(index % batch->config.tile_width);
    const unsigned rotated =
        (lane + (unsigned)(batch->round % batch->config.tile_width)) %
        batch->config.tile_width;
    const unsigned divergent_lanes =
        batch->config.tile_width *
        batch->config.divergence_eighths / 8U;
    const uint64_t key =
        batch->config.seed ^
        ((uint64_t)(index + 1U) * UINT64_C(0x9E3779B97F4A7C15)) ^
        ((batch->round + 1U) * UINT64_C(0xD6E8FEB86659FD93)) ^
        ((uint64_t)(site + 1U) << 48U);

    memset(event, 0, sizeof(*event));
    event->word0 = srem_model_mix64(key);
    event->word1 =
        srem_model_mix64(key ^ UINT64_C(0xA0761D6478BD642F));
    event->result =
        (int32_t)((event->word1 >> 32U) & (uint64_t)INT32_MAX);
    event->site = (uint16_t)site;
    event->divergent = (uint8_t)(rotated < divergent_lanes);
    switch (batch->config.workload) {
    case SREM_MODEL_HTTP_PIPELINE:
        event->kind = (uint8_t)((batch->round & UINT64_C(1)) != 0U ?
                                    SREM_MODEL_EVENT_WRITE :
                                    SREM_MODEL_EVENT_READ);
        break;
    case SREM_MODEL_RPC_PIPELINE:
        event->kind = SREM_MODEL_EVENT_READ;
        break;
    case SREM_MODEL_DIVERGENT_CANCEL:
        if ((event->word0 & UINT64_C(7)) == 0U) {
            event->kind = SREM_MODEL_EVENT_CANCEL;
        } else if ((event->word0 & UINT64_C(3)) == 0U) {
            event->kind = SREM_MODEL_EVENT_TIMEOUT;
        } else {
            event->kind = SREM_MODEL_EVENT_READ;
        }
        break;
    case SREM_MODEL_MIXED_FAIRNESS:
        event->kind =
            (uint8_t)(((index + batch->round) & 31U) == 0U ?
                          SREM_MODEL_EVENT_TIMEOUT :
                          SREM_MODEL_EVENT_READ);
        break;
    }
}

static void reset_frame(srem_model_batch_t *batch, size_t index) {
    srem_model_frame_core_t *frame = srem_model_frame_at(batch, index);
    const size_t tile = index / batch->config.tile_width;
    uint64_t state =
        srem_model_mix64(batch->config.seed ^
                         ((uint64_t)(index + 1U) *
                          UINT64_C(0xE7037ED1A0B428DB)));
    unsigned field;

    for (field = 0U; field < 6U; ++field) {
        state = srem_model_mix64(
            state + (uint64_t)(field + 1U) *
                        UINT64_C(0x9E3779B97F4A7C15));
        frame->field[field] = state;
    }
    frame->generation =
        (uint32_t)(srem_model_mix64(state) | UINT64_C(1));
    frame->site =
        (uint16_t)(tile % batch->config.site_count);
    frame->steps = 0U;
    frame->terminal = 0U;
    frame->flags = 0U;
}

int srem_model_batch_reset(srem_model_batch_t *batch) {
    size_t frame_storage_bytes;
    size_t index;

    if (batch == NULL) {
        return EINVAL;
    }
    if (size_mul_overflow(batch->config.instance_count,
                          batch->config.frame_bytes,
                          &frame_storage_bytes)) {
        return EINVAL;
    }
    memset(batch->frame_storage, 0, frame_storage_bytes);
    memset(batch->effects,
           0,
           batch->config.instance_count *
               sizeof(*batch->effects));
    memset(batch->events,
           0,
           batch->config.instance_count *
               sizeof(*batch->events));
    memset(batch->tickets,
           0,
           batch->config.instance_count *
               sizeof(*batch->tickets));
    batch->round = 0U;
    batch->local_queue.head = 0U;
    batch->local_queue.tail = 0U;
    for (index = 0U; index < batch->config.instance_count; ++index) {
        srem_model_frame_core_t *frame;

        reset_frame(batch, index);
        frame = srem_model_frame_at(batch, index);
        batch->wakers[index].state_generation =
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_ARMED);
        batch->wakers[index].instance_index = (uint32_t)index;
        batch->wakers[index].resume_site = frame->site;
        batch->wakers[index].reserved = 0U;
    }
    return 0;
}

int srem_model_batch_create(const srem_model_config_t *config,
                            srem_model_batch_t **out_batch) {
    srem_model_batch_t *batch;
    size_t frame_storage_bytes;
    size_t queue_capacity;
    int error;

    if (out_batch == NULL) {
        return EINVAL;
    }
    *out_batch = NULL;
    error = validate_config(config);
    if (error != 0) {
        return error;
    }
    error = queue_capacity_for(config->instance_count, &queue_capacity);
    if (error != 0 ||
        size_mul_overflow(config->instance_count,
                          config->frame_bytes,
                          &frame_storage_bytes)) {
        return EINVAL;
    }

    batch = calloc(1U, sizeof(*batch));
    if (batch == NULL) {
        return ENOMEM;
    }
    batch->config = *config;
    batch->ops = srem_model_get_workload_ops(config->workload);
    batch->frame_storage = calloc(1U, frame_storage_bytes);
    batch->effects =
        calloc(config->instance_count, sizeof(*batch->effects));
    batch->events =
        calloc(config->instance_count, sizeof(*batch->events));
    batch->wakers =
        calloc(config->instance_count, sizeof(*batch->wakers));
    batch->tickets =
        calloc(config->instance_count, sizeof(*batch->tickets));
    batch->local_queue.slots =
        calloc(queue_capacity, sizeof(*batch->local_queue.slots));
    if (batch->frame_storage == NULL || batch->effects == NULL ||
        batch->events == NULL || batch->wakers == NULL ||
        batch->tickets == NULL || batch->local_queue.slots == NULL ||
        batch->ops == NULL) {
        srem_model_batch_destroy(batch);
        return ENOMEM;
    }
    batch->local_queue.capacity = queue_capacity;
    batch->local_queue.mask = queue_capacity - 1U;
    error = srem_model_batch_reset(batch);
    if (error != 0) {
        srem_model_batch_destroy(batch);
        return error;
    }
    *out_batch = batch;
    return 0;
}

void srem_model_batch_destroy(srem_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    free(batch->local_queue.slots);
    free(batch->tickets);
    free(batch->wakers);
    free(batch->events);
    free(batch->effects);
    free(batch->frame_storage);
    free(batch);
}

static int publish_baseline_ticket(srem_model_batch_t *batch,
                                   size_t index,
                                   srem_model_metrics_t *metrics) {
    srem_model_frame_core_t *frame = srem_model_frame_at(batch, index);
    srem_model_waker_t *waker = &batch->wakers[index];
    srem_model_ticket_t *ticket = &batch->tickets[index];
    const uint64_t current = waker->state_generation;

    srem_model_derive_event(batch, index, frame->site, &ticket->event);
    ticket->instance_index = (uint32_t)index;
    ticket->generation = frame->generation;
    metrics->completions += 1U;
    if (srem_model_unpack_generation(current) != ticket->generation) {
        metrics->stale_tickets += 1U;
        return 0;
    }
    if (srem_model_unpack_waker_state(current) !=
        SREM_MODEL_WAKER_ARMED) {
        metrics->duplicate_tickets += 1U;
        return 0;
    }
    waker->state_generation =
        srem_model_pack_waker(ticket->generation,
                              SREM_MODEL_WAKER_QUEUED);
    waker->resume_site = ticket->event.site;
    batch->events[index] = ticket->event;
    if (queue_push(&batch->local_queue, (uint32_t)index) != 0) {
        return ENOSPC;
    }
    metrics->claims += 1U;
    metrics->queue_pushes += 1U;
    return 0;
}

static int admit_baseline_round(srem_model_batch_t *batch,
                                srem_model_metrics_t *metrics) {
    const size_t width = batch->config.tile_width;
    size_t tile_begin;

    for (tile_begin = 0U;
         tile_begin < batch->config.instance_count;
         tile_begin += width) {
        const size_t remaining =
            batch->config.instance_count - tile_begin;
        const size_t lanes =
            remaining < width ? remaining : width;
        const size_t active =
            lanes < batch->config.active_lanes ?
                lanes :
                batch->config.active_lanes;
        const size_t tile = tile_begin / width;
        const size_t start =
            (size_t)((batch->round * UINT64_C(5) +
                      (uint64_t)tile * UINT64_C(3)) %
                     lanes);
        size_t offset;

        for (offset = 0U; offset < active; ++offset) {
            const size_t lane = (start + offset) % lanes;
            const int error =
                publish_baseline_ticket(batch,
                                        tile_begin + lane,
                                        metrics);
            if (error != 0) {
                return error;
            }
        }
    }
    return 0;
}

static int drain_baseline_round(srem_model_batch_t *batch,
                                srem_model_metrics_t *metrics) {
    uint32_t index;

    while (queue_pop(&batch->local_queue, &index) == 0) {
        srem_model_frame_core_t *frame =
            srem_model_frame_at(batch, index);
        srem_model_waker_t *waker = &batch->wakers[index];
        const uint64_t queued = waker->state_generation;
        const unsigned resume_site = waker->resume_site;

        if (srem_model_unpack_generation(queued) !=
                frame->generation ||
            srem_model_unpack_waker_state(queued) !=
                SREM_MODEL_WAKER_QUEUED ||
            resume_site >= batch->config.site_count) {
            return EPROTO;
        }
        waker->state_generation =
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_RUNNING);
        batch->ops->resume_sites[resume_site](
            frame,
            &batch->events[index],
            &batch->effects[index],
            &batch->config,
            resume_site);
        waker->resume_site = frame->site;
        waker->state_generation =
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_ARMED);
        metrics->queue_pops += 1U;
        metrics->resume_calls += 1U;
    }
    return 0;
}

int srem_model_run_round(srem_model_batch_t *batch,
                         srem_model_metrics_t *metrics) {
    int error;

    if (batch == NULL || metrics == NULL) {
        return EINVAL;
    }
    if (batch->config.mode != SREM_MODEL_WAKER_FRAME) {
        return ENOTSUP;
    }
    error = admit_baseline_round(batch, metrics);
    if (error == 0) {
        error = drain_baseline_round(batch, metrics);
    }
    if (error == 0) {
        batch->round += 1U;
    }
    return error;
}

static bool frame_equal(const srem_model_frame_core_t *lhs,
                        const srem_model_frame_core_t *rhs) {
    unsigned field;

    for (field = 0U; field < 6U; ++field) {
        if (lhs->field[field] != rhs->field[field]) {
            return false;
        }
    }
    return lhs->generation == rhs->generation &&
           lhs->site == rhs->site && lhs->steps == rhs->steps &&
           lhs->terminal == rhs->terminal &&
           lhs->flags == rhs->flags;
}

static bool effect_equal(const srem_model_effect_t *lhs,
                         const srem_model_effect_t *rhs) {
    return lhs->argument0 == rhs->argument0 &&
           lhs->argument1 == rhs->argument1 &&
           lhs->argument2 == rhs->argument2 &&
           lhs->operation == rhs->operation &&
           lhs->next_site == rhs->next_site &&
           lhs->flags == rhs->flags;
}

bool srem_model_batch_equal(const srem_model_batch_t *lhs,
                            const srem_model_batch_t *rhs) {
    size_t index;

    if (lhs == NULL || rhs == NULL ||
        lhs->config.instance_count != rhs->config.instance_count ||
        lhs->round != rhs->round) {
        return false;
    }
    for (index = 0U; index < lhs->config.instance_count; ++index) {
        if (!frame_equal(srem_model_frame_at(lhs, index),
                         srem_model_frame_at(rhs, index)) ||
            !effect_equal(&lhs->effects[index],
                          &rhs->effects[index])) {
            return false;
        }
    }
    return true;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    unsigned byte;

    for (byte = 0U; byte < 8U; ++byte) {
        hash ^= (value >> (byte * 8U)) & UINT64_C(0xFF);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t srem_model_checksum(const srem_model_batch_t *batch) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    if (batch == NULL) {
        return 0U;
    }
    hash = hash_u64(hash, batch->round);
    hash = hash_u64(hash, batch->config.instance_count);
    for (index = 0U; index < batch->config.instance_count; ++index) {
        const srem_model_frame_core_t *frame =
            srem_model_frame_at(batch, index);
        const srem_model_effect_t *effect = &batch->effects[index];
        unsigned field;

        for (field = 0U; field < 6U; ++field) {
            hash = hash_u64(hash, frame->field[field]);
        }
        hash = hash_u64(hash, frame->generation);
        hash = hash_u64(hash, frame->site);
        hash = hash_u64(hash, frame->steps);
        hash = hash_u64(hash, frame->terminal);
        hash = hash_u64(hash, frame->flags);
        hash = hash_u64(hash, effect->argument0);
        hash = hash_u64(hash, effect->argument1);
        hash = hash_u64(hash, effect->argument2);
        hash = hash_u64(hash, effect->operation);
        hash = hash_u64(hash, effect->next_site);
        hash = hash_u64(hash, effect->flags);
    }
    return hash;
}

const char *srem_model_mode_name(srem_model_mode_t mode) {
    if (mode < SREM_MODEL_WAKER_FRAME ||
        mode > SREM_MODEL_REMOTE_ADAPTIVE) {
        return NULL;
    }
    return MODE_NAMES[(unsigned)mode];
}

const char *srem_model_workload_name(srem_model_workload_t workload) {
    if (workload < SREM_MODEL_HTTP_PIPELINE ||
        workload > SREM_MODEL_MIXED_FAIRNESS) {
        return NULL;
    }
    return WORKLOAD_NAMES[(unsigned)workload];
}

int srem_model_parse_mode(const char *text, srem_model_mode_t *out) {
    unsigned mode;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (mode = SREM_MODEL_WAKER_FRAME;
         mode <= SREM_MODEL_REMOTE_ADAPTIVE;
         ++mode) {
        if (strcmp(text, MODE_NAMES[mode]) == 0) {
            *out = (srem_model_mode_t)mode;
            return 0;
        }
    }
    return EINVAL;
}

int srem_model_parse_workload(const char *text,
                              srem_model_workload_t *out) {
    unsigned workload;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (workload = SREM_MODEL_HTTP_PIPELINE;
         workload <= SREM_MODEL_MIXED_FAIRNESS;
         ++workload) {
        if (strcmp(text, WORKLOAD_NAMES[workload]) == 0) {
            *out = (srem_model_workload_t)workload;
            return 0;
        }
    }
    return EINVAL;
}

int srem_model_candidate_baseline(
    srem_model_mode_t candidate,
    srem_model_mode_t *out_baseline) {
    if (out_baseline == NULL) {
        return EINVAL;
    }
    switch (candidate) {
    case SREM_MODEL_TILE_SCALAR:
    case SREM_MODEL_TILE_VECTOR:
    case SREM_MODEL_ADAPTIVE:
        *out_baseline = SREM_MODEL_WAKER_FRAME;
        return 0;
    case SREM_MODEL_REMOTE_ADAPTIVE:
        *out_baseline = SREM_MODEL_REMOTE_WAKER_FRAME;
        return 0;
    default:
        return EINVAL;
    }
}
