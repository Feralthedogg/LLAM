// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

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

static int remote_team_create(srem_model_batch_t *batch);
static void remote_team_destroy(srem_model_batch_t *batch);

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

static bool mode_uses_tiles(srem_model_mode_t mode) {
    return mode == SREM_MODEL_TILE_SCALAR ||
           mode == SREM_MODEL_TILE_VECTOR ||
           mode == SREM_MODEL_ADAPTIVE ||
           mode == SREM_MODEL_REMOTE_ADAPTIVE;
}

static void *aligned_zero_allocate(size_t count, size_t item_size) {
    const size_t alignment = 64U;
    size_t bytes;
    size_t rounded;
    void *memory;

    if (size_mul_overflow(count, item_size, &bytes) ||
        bytes > SIZE_MAX - (alignment - 1U)) {
        return NULL;
    }
    rounded = (bytes + alignment - 1U) & ~(alignment - 1U);
#if defined(_MSC_VER)
    memory = _aligned_malloc(rounded, alignment);
#else
    memory = aligned_alloc(alignment, rounded);
#endif
    if (memory != NULL) {
        memset(memory, 0, rounded);
    }
    return memory;
}

static void aligned_deallocate(void *memory) {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    free(memory);
#endif
}

static uint32_t mask_for_lanes(unsigned lanes) {
    if (lanes >= 32U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << lanes) - UINT32_C(1);
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

static size_t tile_slot(const srem_model_batch_t *batch, size_t index) {
    const size_t tile = index / batch->config.tile_width;
    const size_t lane = index % batch->config.tile_width;

    return tile * batch->config.tile_width + lane;
}

static uint64_t *tile_field_at(srem_model_batch_t *batch,
                               size_t index,
                               unsigned field) {
    const size_t tile = index / batch->config.tile_width;
    const size_t lane = index % batch->config.tile_width;
    const size_t offset =
        (tile * 6U + field) * batch->config.tile_width + lane;

    return &batch->tile_fields[offset];
}

static uint64_t *tile_effect_argument_at(srem_model_batch_t *batch,
                                         size_t index,
                                         unsigned argument) {
    const size_t tile = index / batch->config.tile_width;
    const size_t lane = index % batch->config.tile_width;
    const size_t offset =
        (tile * 3U + argument) * batch->config.tile_width + lane;

    return &batch->tile_effect_arguments[offset];
}

uint32_t *srem_model_tile_generation_at(srem_model_batch_t *batch,
                                        size_t index) {
    if (batch == NULL || index >= batch->config.instance_count) {
        return NULL;
    }
    return &batch->tile_generations[tile_slot(batch, index)];
}

static void store_tile_frame(srem_model_batch_t *batch,
                             size_t index,
                             const srem_model_frame_core_t *frame) {
    const size_t slot = tile_slot(batch, index);
    unsigned field;

    for (field = 0U; field < 6U; ++field) {
        *tile_field_at(batch, index, field) = frame->field[field];
    }
    batch->tile_generations[slot] = frame->generation;
    batch->tile_sites[slot] = frame->site;
    batch->tile_steps[slot] = frame->steps;
    batch->tile_terminal[slot] = frame->terminal;
    batch->tile_flags[slot] = frame->flags;
}

static void load_tile_frame(const srem_model_batch_t *batch,
                            size_t index,
                            srem_model_frame_core_t *frame) {
    srem_model_batch_t *mutable_batch = (srem_model_batch_t *)batch;
    const size_t slot = tile_slot(batch, index);
    unsigned field;

    for (field = 0U; field < 6U; ++field) {
        frame->field[field] =
            *tile_field_at(mutable_batch, index, field);
    }
    frame->generation = batch->tile_generations[slot];
    frame->site = batch->tile_sites[slot];
    frame->steps = batch->tile_steps[slot];
    frame->terminal = batch->tile_terminal[slot];
    frame->flags = batch->tile_flags[slot];
}

static void store_tile_event(srem_model_batch_t *batch,
                             size_t index,
                             const srem_model_event_t *event) {
    const size_t slot = tile_slot(batch, index);

    batch->tile_event_word0[slot] = event->word0;
    batch->tile_event_word1[slot] = event->word1;
    batch->tile_event_result[slot] = event->result;
    batch->tile_event_site[slot] = event->site;
    batch->tile_event_kind[slot] = event->kind;
    batch->tile_event_divergent[slot] = event->divergent;
}

static void load_tile_event(const srem_model_batch_t *batch,
                            size_t index,
                            srem_model_event_t *event) {
    const size_t slot = tile_slot(batch, index);

    event->word0 = batch->tile_event_word0[slot];
    event->word1 = batch->tile_event_word1[slot];
    event->result = batch->tile_event_result[slot];
    event->site = batch->tile_event_site[slot];
    event->kind = batch->tile_event_kind[slot];
    event->divergent = batch->tile_event_divergent[slot];
}

static void store_tile_effect(srem_model_batch_t *batch,
                              size_t index,
                              const srem_model_effect_t *effect) {
    const size_t slot = tile_slot(batch, index);
    unsigned argument;

    for (argument = 0U; argument < 3U; ++argument) {
        uint64_t value = effect->argument0;

        if (argument == 1U) {
            value = effect->argument1;
        } else if (argument == 2U) {
            value = effect->argument2;
        }
        *tile_effect_argument_at(batch, index, argument) = value;
    }
    batch->tile_effect_operation[slot] = effect->operation;
    batch->tile_effect_next_site[slot] = effect->next_site;
    batch->tile_effect_flags[slot] = effect->flags;
}

static void load_tile_effect(const srem_model_batch_t *batch,
                             size_t index,
                             srem_model_effect_t *effect) {
    srem_model_batch_t *mutable_batch = (srem_model_batch_t *)batch;
    const size_t slot = tile_slot(batch, index);

    effect->argument0 =
        *tile_effect_argument_at(mutable_batch, index, 0U);
    effect->argument1 =
        *tile_effect_argument_at(mutable_batch, index, 1U);
    effect->argument2 =
        *tile_effect_argument_at(mutable_batch, index, 2U);
    effect->operation = batch->tile_effect_operation[slot];
    effect->next_site = batch->tile_effect_next_site[slot];
    effect->flags = batch->tile_effect_flags[slot];
}

static void project_frame(const srem_model_batch_t *batch,
                          size_t index,
                          srem_model_frame_core_t *frame) {
    if (mode_uses_tiles(batch->config.mode)) {
        load_tile_frame(batch, index, frame);
    } else {
        *frame = *srem_model_frame_at(batch, index);
    }
}

static void project_effect(const srem_model_batch_t *batch,
                           size_t index,
                           srem_model_effect_t *effect) {
    if (mode_uses_tiles(batch->config.mode)) {
        load_tile_effect(batch, index, effect);
    } else {
        *effect = batch->effects[index];
    }
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

static void initial_frame(const srem_model_batch_t *batch,
                          size_t index,
                          srem_model_frame_core_t *frame) {
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
    if (mode_is_remote(batch->config.mode) &&
        (batch->remote_queue.slots == NULL ||
         atomic_load_explicit(
             &batch->remote_queue.enqueue_position,
             memory_order_acquire) !=
             batch->remote_queue.dequeue_position)) {
        return EBUSY;
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
    memset(batch->tiles,
           0,
           batch->tile_count * sizeof(*batch->tiles));
    memset(batch->tile_fields,
           0,
           batch->tile_slot_count * 6U *
               sizeof(*batch->tile_fields));
    memset(batch->tile_generations,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_generations));
    memset(batch->tile_sites,
           0,
           batch->tile_slot_count * sizeof(*batch->tile_sites));
    memset(batch->tile_steps,
           0,
           batch->tile_slot_count * sizeof(*batch->tile_steps));
    memset(batch->tile_terminal,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_terminal));
    memset(batch->tile_flags,
           0,
           batch->tile_slot_count * sizeof(*batch->tile_flags));
    memset(batch->tile_event_word0,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_word0));
    memset(batch->tile_event_word1,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_word1));
    memset(batch->tile_event_result,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_result));
    memset(batch->tile_event_site,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_site));
    memset(batch->tile_event_kind,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_kind));
    memset(batch->tile_event_divergent,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_event_divergent));
    memset(batch->tile_effect_arguments,
           0,
           batch->tile_slot_count * 3U *
               sizeof(*batch->tile_effect_arguments));
    memset(batch->tile_effect_operation,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_effect_operation));
    memset(batch->tile_effect_next_site,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_effect_next_site));
    memset(batch->tile_effect_flags,
           0,
           batch->tile_slot_count *
               sizeof(*batch->tile_effect_flags));
    batch->round = 0U;
    batch->local_queue.head = 0U;
    batch->local_queue.tail = 0U;
    batch->remote_active_count = 0U;
    batch->fairness_tick = 0U;
    batch->fairness_sample_count = 0U;
    if (batch->fairness_due_ticks != NULL) {
        for (index = 0U;
             index < batch->config.instance_count;
             ++index) {
            batch->fairness_due_ticks[index] = UINT64_MAX;
        }
    }
    if (batch->fairness_histogram != NULL) {
        memset(batch->fairness_histogram,
               0,
               batch->fairness_histogram_size *
                   sizeof(*batch->fairness_histogram));
    }
    for (index = 0U; index < batch->config.instance_count; ++index) {
        srem_model_frame_core_t initial;
        srem_model_frame_core_t *frame;

        initial_frame(batch, index, &initial);
        frame = srem_model_frame_at(batch, index);
        *frame = initial;
        store_tile_frame(batch, index, &initial);
        atomic_store_explicit(
            &batch->wakers[index].state_generation,
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_ARMED),
            memory_order_relaxed);
        batch->wakers[index].instance_index = (uint32_t)index;
        batch->wakers[index].resume_site = frame->site;
        batch->wakers[index].reserved = 0U;
    }
    for (index = 0U; index < batch->tile_count; ++index) {
        const size_t begin = index * batch->config.tile_width;
        const size_t remaining =
            batch->config.instance_count - begin;
        const unsigned lanes =
            (unsigned)(remaining < batch->config.tile_width ?
                           remaining :
                           batch->config.tile_width);

        batch->tiles[index].valid_mask = mask_for_lanes(lanes);
        if (batch->remote_pending_masks != NULL) {
            unsigned site;

            atomic_store_explicit(
                &batch->remote_pending_masks[index],
                0U,
                memory_order_relaxed);
            atomic_store_explicit(
                &batch->remote_published[index],
                0U,
                memory_order_relaxed);
            for (site = 0U;
                 site < SREM_MODEL_MAX_SITES;
                 ++site) {
                atomic_store_explicit(
                    &batch->remote_ready_masks[
                        index * SREM_MODEL_MAX_SITES + site],
                    0U,
                    memory_order_relaxed);
            }
        }
    }
    return 0;
}

int srem_model_batch_create(const srem_model_config_t *config,
                            srem_model_batch_t **out_batch) {
    srem_model_batch_t *batch;
    size_t frame_storage_bytes;
    size_t queue_capacity;
    size_t tile_count;
    size_t tile_slot_count;
    size_t remote_mask_count;
    size_t fairness_histogram_size = 0U;
    size_t index;
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
    if (config->instance_count >
        SIZE_MAX - (config->tile_width - 1U)) {
        return EINVAL;
    }
    tile_count =
        (config->instance_count + config->tile_width - 1U) /
        config->tile_width;
    if (size_mul_overflow(tile_count,
                          config->tile_width,
                          &tile_slot_count) ||
        size_mul_overflow(tile_count,
                          SREM_MODEL_MAX_SITES,
                          &remote_mask_count) ||
        tile_slot_count > SIZE_MAX / 6U ||
        tile_slot_count > SIZE_MAX / 3U) {
        return EINVAL;
    }
    if (config->workload == SREM_MODEL_MIXED_FAIRNESS) {
        if (config->instance_count > (SIZE_MAX - 1U) / 2U) {
            return EINVAL;
        }
        fairness_histogram_size =
            config->instance_count * 2U + 1U;
    }

    batch = calloc(1U, sizeof(*batch));
    if (batch == NULL) {
        return ENOMEM;
    }
    batch->config = *config;
    batch->tile_count = tile_count;
    batch->tile_slot_count = tile_slot_count;
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
    batch->tiles = calloc(tile_count, sizeof(*batch->tiles));
    batch->tile_fields =
        aligned_zero_allocate(tile_slot_count * 6U,
                              sizeof(*batch->tile_fields));
    batch->tile_generations =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_generations));
    batch->tile_sites =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_sites));
    batch->tile_steps =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_steps));
    batch->tile_terminal =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_terminal));
    batch->tile_flags =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_flags));
    batch->tile_event_word0 =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_word0));
    batch->tile_event_word1 =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_word1));
    batch->tile_event_result =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_result));
    batch->tile_event_site =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_site));
    batch->tile_event_kind =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_kind));
    batch->tile_event_divergent =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_event_divergent));
    batch->tile_effect_arguments =
        aligned_zero_allocate(tile_slot_count * 3U,
                              sizeof(*batch->tile_effect_arguments));
    batch->tile_effect_operation =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_effect_operation));
    batch->tile_effect_next_site =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_effect_next_site));
    batch->tile_effect_flags =
        aligned_zero_allocate(tile_slot_count,
                              sizeof(*batch->tile_effect_flags));
    if (mode_is_remote(config->mode)) {
        batch->remote_queue.slots =
            calloc(queue_capacity,
                   sizeof(*batch->remote_queue.slots));
        batch->remote_active_indices =
            calloc(config->instance_count,
                   sizeof(*batch->remote_active_indices));
        batch->remote_ready_masks =
            calloc(remote_mask_count,
                   sizeof(*batch->remote_ready_masks));
        batch->remote_pending_masks =
            calloc(tile_count,
                   sizeof(*batch->remote_pending_masks));
        batch->remote_published =
            calloc(tile_count,
                   sizeof(*batch->remote_published));
    }
    if (fairness_histogram_size != 0U) {
        batch->fairness_due_ticks =
            calloc(config->instance_count,
                   sizeof(*batch->fairness_due_ticks));
        batch->fairness_histogram =
            calloc(fairness_histogram_size,
                   sizeof(*batch->fairness_histogram));
        batch->fairness_histogram_size =
            fairness_histogram_size;
    }
    if (batch->frame_storage == NULL || batch->effects == NULL ||
        batch->events == NULL || batch->wakers == NULL ||
        batch->tickets == NULL || batch->local_queue.slots == NULL ||
        batch->tiles == NULL || batch->tile_fields == NULL ||
        batch->tile_generations == NULL ||
        batch->tile_sites == NULL || batch->tile_steps == NULL ||
        batch->tile_terminal == NULL || batch->tile_flags == NULL ||
        batch->tile_event_word0 == NULL ||
        batch->tile_event_word1 == NULL ||
        batch->tile_event_result == NULL ||
        batch->tile_event_site == NULL ||
        batch->tile_event_kind == NULL ||
        batch->tile_event_divergent == NULL ||
        batch->tile_effect_arguments == NULL ||
        batch->tile_effect_operation == NULL ||
        batch->tile_effect_next_site == NULL ||
        batch->tile_effect_flags == NULL || batch->ops == NULL ||
        (mode_is_remote(config->mode) &&
         (batch->remote_queue.slots == NULL ||
          batch->remote_active_indices == NULL ||
          batch->remote_ready_masks == NULL ||
          batch->remote_pending_masks == NULL ||
          batch->remote_published == NULL)) ||
        (fairness_histogram_size != 0U &&
         (batch->fairness_due_ticks == NULL ||
          batch->fairness_histogram == NULL))) {
        srem_model_batch_destroy(batch);
        return ENOMEM;
    }
    batch->local_queue.capacity = queue_capacity;
    batch->local_queue.mask = queue_capacity - 1U;
    for (index = 0U;
         index < config->instance_count;
         ++index) {
        atomic_init(&batch->wakers[index].state_generation, 0U);
    }
    if (mode_is_remote(config->mode)) {
        batch->remote_queue.capacity = queue_capacity;
        batch->remote_queue.mask = queue_capacity - 1U;
        atomic_init(&batch->remote_queue.enqueue_position, 0U);
        for (index = 0U; index < queue_capacity; ++index) {
            atomic_init(
                &batch->remote_queue.slots[index].sequence,
                index);
        }
        for (index = 0U; index < remote_mask_count; ++index) {
            atomic_init(&batch->remote_ready_masks[index], 0U);
        }
        for (index = 0U; index < tile_count; ++index) {
            atomic_init(&batch->remote_pending_masks[index], 0U);
            atomic_init(&batch->remote_published[index], 0U);
        }
    }
    error = srem_model_batch_reset(batch);
    if (error != 0) {
        srem_model_batch_destroy(batch);
        return error;
    }
    if (mode_is_remote(config->mode)) {
        error = remote_team_create(batch);
        if (error != 0) {
            srem_model_batch_destroy(batch);
            return error;
        }
    }
    *out_batch = batch;
    return 0;
}

void srem_model_batch_destroy(srem_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    remote_team_destroy(batch);
    free(batch->fairness_histogram);
    free(batch->fairness_due_ticks);
    free(batch->remote_published);
    free(batch->remote_pending_masks);
    free(batch->remote_ready_masks);
    free(batch->remote_active_indices);
    free(batch->remote_queue.slots);
    aligned_deallocate(batch->tile_effect_flags);
    aligned_deallocate(batch->tile_effect_next_site);
    aligned_deallocate(batch->tile_effect_operation);
    aligned_deallocate(batch->tile_effect_arguments);
    aligned_deallocate(batch->tile_event_divergent);
    aligned_deallocate(batch->tile_event_kind);
    aligned_deallocate(batch->tile_event_site);
    aligned_deallocate(batch->tile_event_result);
    aligned_deallocate(batch->tile_event_word1);
    aligned_deallocate(batch->tile_event_word0);
    aligned_deallocate(batch->tile_flags);
    aligned_deallocate(batch->tile_terminal);
    aligned_deallocate(batch->tile_steps);
    aligned_deallocate(batch->tile_sites);
    aligned_deallocate(batch->tile_generations);
    aligned_deallocate(batch->tile_fields);
    free(batch->tiles);
    free(batch->local_queue.slots);
    free(batch->tickets);
    free(batch->wakers);
    free(batch->events);
    free(batch->effects);
    free(batch->frame_storage);
    free(batch);
}

int srem_model_make_ticket(const srem_model_batch_t *batch,
                           size_t index,
                           srem_model_ticket_t *out_ticket) {
    srem_model_frame_core_t frame;

    if (batch == NULL || out_ticket == NULL ||
        index >= batch->config.instance_count) {
        return EINVAL;
    }
    project_frame(batch, index, &frame);
    memset(out_ticket, 0, sizeof(*out_ticket));
    srem_model_derive_event(batch,
                            index,
                            frame.site,
                            &out_ticket->event);
    out_ticket->instance_index = (uint32_t)index;
    out_ticket->generation = frame.generation;
    return 0;
}

static void fairness_admit(srem_model_batch_t *batch,
                           size_t index,
                           uint8_t event_kind) {
    if (batch->fairness_due_ticks == NULL) {
        return;
    }
    batch->fairness_due_ticks[index] =
        event_kind == SREM_MODEL_EVENT_TIMEOUT ?
            batch->fairness_tick :
            UINT64_MAX;
    batch->fairness_tick += UINT64_C(1);
}

static void fairness_service(srem_model_batch_t *batch,
                             size_t index,
                             srem_model_metrics_t *metrics) {
    uint64_t due;

    if (batch->fairness_due_ticks == NULL) {
        return;
    }
    due = batch->fairness_due_ticks[index];
    batch->fairness_due_ticks[index] = UINT64_MAX;
    if (due != UINT64_MAX) {
        uint64_t gap =
            batch->fairness_tick >= due ?
                batch->fairness_tick - due :
                0U;
        size_t bucket =
            gap < batch->fairness_histogram_size ?
                (size_t)gap :
                batch->fairness_histogram_size - 1U;

        batch->fairness_histogram[bucket] += UINT64_C(1);
        batch->fairness_sample_count += UINT64_C(1);
        metrics->fairness_samples += UINT64_C(1);
    }
    batch->fairness_tick += UINT64_C(1);
}

static void update_fairness_p99(
    const srem_model_batch_t *batch,
    srem_model_metrics_t *metrics) {
    uint64_t rank;
    uint64_t cumulative = 0U;
    size_t gap;

    if (batch->fairness_sample_count == 0U ||
        batch->fairness_histogram == NULL) {
        return;
    }
    rank = batch->fairness_sample_count -
           batch->fairness_sample_count / UINT64_C(100);
    for (gap = 0U;
         gap < batch->fairness_histogram_size;
         ++gap) {
        cumulative += batch->fairness_histogram[gap];
        if (cumulative >= rank) {
            metrics->fairness_p99_gap = gap;
            return;
        }
    }
}

static int publish_baseline_ticket(srem_model_batch_t *batch,
                                   size_t index,
                                   srem_model_metrics_t *metrics) {
    srem_model_waker_t *waker = &batch->wakers[index];
    srem_model_ticket_t *ticket = &batch->tickets[index];
    const uint64_t current = atomic_load_explicit(
        &waker->state_generation, memory_order_relaxed);

    if (srem_model_make_ticket(batch, index, ticket) != 0) {
        return EINVAL;
    }
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
    atomic_store_explicit(
        &waker->state_generation,
        srem_model_pack_waker(ticket->generation,
                              SREM_MODEL_WAKER_QUEUED),
        memory_order_relaxed);
    waker->resume_site = ticket->event.site;
    batch->events[index] = ticket->event;
    if (queue_push(&batch->local_queue, (uint32_t)index) != 0) {
        return ENOSPC;
    }
    metrics->claims += 1U;
    metrics->queue_pushes += 1U;
    fairness_admit(batch, index, ticket->event.kind);
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
        const uint64_t queued = atomic_load_explicit(
            &waker->state_generation, memory_order_relaxed);
        const unsigned resume_site = waker->resume_site;

        if (srem_model_unpack_generation(queued) !=
                frame->generation ||
            srem_model_unpack_waker_state(queued) !=
                SREM_MODEL_WAKER_QUEUED ||
            resume_site >= batch->config.site_count) {
            return EPROTO;
        }
        atomic_store_explicit(
            &waker->state_generation,
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_RUNNING),
            memory_order_relaxed);
        batch->ops->resume_sites[resume_site](
            frame,
            &batch->events[index],
            &batch->effects[index],
            &batch->config,
            resume_site);
        waker->resume_site = frame->site;
        atomic_store_explicit(
            &waker->state_generation,
            srem_model_pack_waker(frame->generation,
                                  SREM_MODEL_WAKER_ARMED),
            memory_order_relaxed);
        metrics->queue_pops += 1U;
        metrics->resume_calls += 1U;
        fairness_service(batch, index, metrics);
    }
    return 0;
}

static uint32_t tile_ready_union(const srem_model_tile_t *tile,
                                 unsigned site_count) {
    uint32_t ready = 0U;
    unsigned site;

    for (site = 0U; site < site_count; ++site) {
        ready |= tile->ready_mask[site];
    }
    return ready;
}

int srem_model_tile_admit_ticket(srem_model_batch_t *batch,
                                 const srem_model_ticket_t *ticket,
                                 srem_model_metrics_t *metrics) {
    const size_t index =
        ticket != NULL ? ticket->instance_index : SIZE_MAX;
    size_t tile_index;
    unsigned lane;
    uint32_t bit;
    srem_model_tile_t *tile;
    uint32_t *generation;

    if (batch == NULL || ticket == NULL || metrics == NULL ||
        !mode_uses_tiles(batch->config.mode) ||
        index >= batch->config.instance_count ||
        ticket->event.site >= batch->config.site_count) {
        return EINVAL;
    }
    metrics->completions += 1U;
    generation = srem_model_tile_generation_at(batch, index);
    if (*generation != ticket->generation) {
        metrics->stale_tickets += 1U;
        return 0;
    }
    tile_index = index / batch->config.tile_width;
    lane = (unsigned)(index % batch->config.tile_width);
    bit = UINT32_C(1) << lane;
    tile = &batch->tiles[tile_index];
    if ((tile->valid_mask & bit) == 0U) {
        return EPROTO;
    }
    if ((tile->pending_mask & bit) != 0U) {
        metrics->duplicate_tickets += 1U;
        return 0;
    }

    store_tile_event(batch, index, &ticket->event);
    tile->pending_mask |= bit;
    tile->ready_mask[ticket->event.site] |= bit;
    if (tile->queued == 0U && tile->running == 0U) {
        const int error =
            queue_push(&batch->local_queue, (uint32_t)tile_index);
        if (error != 0) {
            tile->ready_mask[ticket->event.site] &= ~bit;
            tile->pending_mask &= ~bit;
            return error;
        }
        tile->queued = 1U;
        metrics->queue_pushes += 1U;
    }
    metrics->claims += 1U;
    fairness_admit(batch, index, ticket->event.kind);
    return 0;
}

static int admit_tile_round(srem_model_batch_t *batch,
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
            srem_model_ticket_t *ticket =
                &batch->tickets[tile_begin + lane];
            int error =
                srem_model_make_ticket(batch,
                                       tile_begin + lane,
                                       ticket);

            if (error == 0) {
                error =
                    srem_model_tile_admit_ticket(batch,
                                                 ticket,
                                                 metrics);
            }
            if (error != 0) {
                return error;
            }
        }
    }
    return 0;
}

static int run_tile_scalar_mask(srem_model_batch_t *batch,
                                size_t tile_index,
                                unsigned resume_site,
                                uint32_t mask,
                                srem_model_metrics_t *metrics) {
    const size_t begin = tile_index * batch->config.tile_width;
    unsigned lane;

    for (lane = 0U; lane < batch->config.tile_width; ++lane) {
        const uint32_t bit = UINT32_C(1) << lane;
        const size_t index = begin + lane;
        srem_model_frame_core_t frame;
        srem_model_event_t event;
        srem_model_effect_t effect;

        if ((mask & bit) == 0U) {
            continue;
        }
        if (index >= batch->config.instance_count ||
            (batch->tiles[tile_index].pending_mask & bit) == 0U) {
            return EPROTO;
        }
        load_tile_frame(batch, index, &frame);
        load_tile_event(batch, index, &event);
        if (frame.site != resume_site ||
            event.site != resume_site) {
            return EPROTO;
        }
        batch->ops->resume_sites[resume_site](
            &frame,
            &event,
            &effect,
            &batch->config,
            resume_site);
        store_tile_frame(batch, index, &frame);
        store_tile_effect(batch, index, &effect);
        batch->tiles[tile_index].pending_mask &= ~bit;
        metrics->scalar_lanes += 1U;
        fairness_service(batch, index, metrics);
    }
    return 0;
}

static unsigned popcount_u32(uint32_t value) {
    unsigned count = 0U;

    while (value != 0U) {
        value &= value - UINT32_C(1);
        count += 1U;
    }
    return count;
}

static void make_tile_view(srem_model_batch_t *batch,
                           size_t tile_index,
                           unsigned resume_site,
                           uint32_t mask,
                           srem_model_tile_view_t *view) {
    const size_t width = batch->config.tile_width;
    const size_t base = tile_index * width;
    unsigned field;
    unsigned argument;

    memset(view, 0, sizeof(*view));
    view->width = batch->config.tile_width;
    view->resume_site = resume_site;
    view->active_mask = mask;
    for (field = 0U; field < 6U; ++field) {
        view->field[field] =
            &batch->tile_fields[
                (tile_index * 6U + field) * width];
    }
    view->generation = &batch->tile_generations[base];
    view->site = &batch->tile_sites[base];
    view->steps = &batch->tile_steps[base];
    view->terminal = &batch->tile_terminal[base];
    view->flags = &batch->tile_flags[base];
    view->event_word0 = &batch->tile_event_word0[base];
    view->event_word1 = &batch->tile_event_word1[base];
    view->event_result = &batch->tile_event_result[base];
    view->event_site = &batch->tile_event_site[base];
    view->event_kind = &batch->tile_event_kind[base];
    view->event_divergent =
        &batch->tile_event_divergent[base];
    for (argument = 0U; argument < 3U; ++argument) {
        view->effect_argument[argument] =
            &batch->tile_effect_arguments[
                (tile_index * 3U + argument) * width];
    }
    view->effect_operation =
        &batch->tile_effect_operation[base];
    view->effect_next_site =
        &batch->tile_effect_next_site[base];
    view->effect_flags = &batch->tile_effect_flags[base];
}

static int run_tile_vector_mask(srem_model_batch_t *batch,
                                size_t tile_index,
                                unsigned resume_site,
                                uint32_t mask,
                                srem_model_metrics_t *metrics) {
    srem_model_tile_view_t view;
    const unsigned active_lanes = popcount_u32(mask);
    const size_t begin = tile_index * batch->config.tile_width;
    unsigned lane;
    int error;

    make_tile_view(batch,
                   tile_index,
                   resume_site,
                   mask,
                   &view);
    error = srem_model_run_vector_superblock(
        batch->config.workload,
        &batch->config,
        &view);
    if (error != 0) {
        return error;
    }
    batch->tiles[tile_index].pending_mask &= ~mask;
    for (lane = 0U; lane < batch->config.tile_width; ++lane) {
        if ((mask & (UINT32_C(1) << lane)) != 0U) {
            fairness_service(batch, begin + lane, metrics);
        }
    }
    metrics->vector_lanes += active_lanes;
    metrics->vector_blocks += 1U;
    return 0;
}

static unsigned select_tile_site(
    const srem_model_batch_t *batch,
    size_t tile_index,
    const srem_model_tile_t *tile) {
    unsigned fallback = SREM_MODEL_MAX_SITES;
    unsigned selected = SREM_MODEL_MAX_SITES;
    uint64_t earliest_due = UINT64_MAX;
    unsigned site;

    for (site = 0U; site < batch->config.site_count; ++site) {
        uint32_t mask =
            tile->ready_mask[site] & tile->valid_mask;

        if (mask == 0U) {
            continue;
        }
        if (fallback == SREM_MODEL_MAX_SITES) {
            fallback = site;
        }
        if (batch->fairness_due_ticks != NULL) {
            unsigned lane;

            for (lane = 0U;
                 lane < batch->config.tile_width;
                 ++lane) {
                const uint32_t bit = UINT32_C(1) << lane;

                if ((mask & bit) != 0U) {
                    const size_t index =
                        tile_index * batch->config.tile_width + lane;
                    const uint64_t due =
                        batch->fairness_due_ticks[index];

                    if (due < earliest_due) {
                        earliest_due = due;
                        selected = site;
                    }
                }
            }
        }
    }
    return selected != SREM_MODEL_MAX_SITES ?
               selected :
               fallback;
}

int srem_model_tile_drain(srem_model_batch_t *batch,
                          srem_model_metrics_t *metrics) {
    uint32_t tile_index;

    if (batch == NULL || metrics == NULL ||
        !mode_uses_tiles(batch->config.mode)) {
        return EINVAL;
    }
    while (queue_pop(&batch->local_queue, &tile_index) == 0) {
        srem_model_tile_t *tile;
        unsigned site;
        uint32_t mask = 0U;
        int error;

        if (tile_index >= batch->tile_count) {
            return EPROTO;
        }
        tile = &batch->tiles[tile_index];
        if (tile->queued == 0U || tile->running != 0U) {
            return EPROTO;
        }
        tile->queued = 0U;
        tile->running = 1U;
        site = select_tile_site(batch, tile_index, tile);
        if (site < batch->config.site_count) {
            mask = tile->ready_mask[site] & tile->valid_mask;
            tile->ready_mask[site] = 0U;
        }
        if (mask == 0U) {
            tile->running = 0U;
            return EPROTO;
        }
        metrics->queue_pops += 1U;
        metrics->tile_dispatches += 1U;
        if (batch->config.mode == SREM_MODEL_TILE_VECTOR ||
            ((batch->config.mode == SREM_MODEL_ADAPTIVE ||
              batch->config.mode ==
                  SREM_MODEL_REMOTE_ADAPTIVE) &&
             popcount_u32(mask) >=
                 batch->config.vector_threshold)) {
            error = run_tile_vector_mask(batch,
                                         tile_index,
                                         site,
                                         mask,
                                         metrics);
        } else {
            error = run_tile_scalar_mask(batch,
                                         tile_index,
                                         site,
                                         mask,
                                         metrics);
        }
        tile->running = 0U;
        if (error != 0) {
            return error;
        }
        if (tile_ready_union(tile, batch->config.site_count) != 0U) {
            error = queue_push(&batch->local_queue, tile_index);
            if (error != 0) {
                return error;
            }
            tile->queued = 1U;
            metrics->queue_pushes += 1U;
        }
    }
    return 0;
}

static int remote_queue_push(srem_model_batch_t *batch,
                             uint32_t item) {
    srem_model_remote_queue_t *queue = &batch->remote_queue;
    srem_model_remote_slot_t *slot;
    size_t position;

    if (queue->slots == NULL || queue->capacity == 0U) {
        return EINVAL;
    }
    position = atomic_fetch_add_explicit(
        &queue->enqueue_position, 1U, memory_order_relaxed);
    slot = &queue->slots[position & queue->mask];
    while (atomic_load_explicit(
               &slot->sequence, memory_order_acquire) != position) {
        if (atomic_load_explicit(
                &batch->remote_team.stop,
                memory_order_acquire)) {
            return ECANCELED;
        }
        atomic_signal_fence(memory_order_seq_cst);
    }
    slot->item = item;
    atomic_store_explicit(
        &slot->sequence, position + 1U, memory_order_release);
    return 0;
}

static int remote_queue_pop(srem_model_remote_queue_t *queue,
                            uint32_t *out_item) {
    srem_model_remote_slot_t *slot;
    size_t position;

    if (queue == NULL || out_item == NULL ||
        queue->slots == NULL || queue->capacity == 0U) {
        return EINVAL;
    }
    position = queue->dequeue_position;
    slot = &queue->slots[position & queue->mask];
    if (atomic_load_explicit(
            &slot->sequence, memory_order_acquire) !=
        position + 1U) {
        return EAGAIN;
    }
    *out_item = slot->item;
    atomic_store_explicit(
        &slot->sequence,
        position + queue->capacity,
        memory_order_release);
    queue->dequeue_position = position + 1U;
    return 0;
}

static int build_remote_completion_stream(
    srem_model_batch_t *batch) {
    const size_t width = batch->config.tile_width;
    size_t tile_begin;
    size_t count = 0U;

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
            const size_t index = tile_begin + lane;
            uint8_t kind = SREM_MODEL_EVENT_READ;

            if (count >= batch->config.instance_count) {
                return EOVERFLOW;
            }
            batch->remote_active_indices[count] =
                (uint32_t)index;
            count += 1U;
            if (batch->config.workload ==
                    SREM_MODEL_MIXED_FAIRNESS &&
                ((index + batch->round) & 31U) == 0U) {
                kind = SREM_MODEL_EVENT_TIMEOUT;
            }
            fairness_admit(batch, index, kind);
        }
    }
    batch->remote_active_count = count;
    return count == 0U ? EPROTO : 0;
}

static int remote_publish_baseline(
    srem_model_remote_worker_t *worker,
    size_t index) {
    srem_model_batch_t *batch = worker->batch;
    srem_model_waker_t *waker = &batch->wakers[index];
    srem_model_ticket_t *ticket = &batch->tickets[index];
    uint64_t current;
    uint64_t expected;

    if (srem_model_make_ticket(batch, index, ticket) != 0) {
        return EPROTO;
    }
    worker->metrics.completions += UINT64_C(1);
    current = atomic_load_explicit(
        &waker->state_generation, memory_order_acquire);
    if (srem_model_unpack_generation(current) !=
        ticket->generation) {
        worker->metrics.stale_tickets += UINT64_C(1);
        return 0;
    }
    if (srem_model_unpack_waker_state(current) !=
        SREM_MODEL_WAKER_ARMED) {
        worker->metrics.duplicate_tickets += UINT64_C(1);
        return 0;
    }

    batch->events[index] = ticket->event;
    waker->resume_site = ticket->event.site;
    expected = current;
    if (!atomic_compare_exchange_strong_explicit(
            &waker->state_generation,
            &expected,
            srem_model_pack_waker(
                ticket->generation,
                SREM_MODEL_WAKER_QUEUED),
            memory_order_acq_rel,
            memory_order_acquire)) {
        worker->metrics.duplicate_tickets += UINT64_C(1);
        return 0;
    }
    if (remote_queue_push(batch, (uint32_t)index) != 0) {
        return EPROTO;
    }
    worker->metrics.claims += UINT64_C(1);
    worker->metrics.queue_pushes += UINT64_C(1);
    worker->metrics.remote_pushes += UINT64_C(1);
    return 0;
}

static int remote_publish_tile(
    srem_model_remote_worker_t *worker,
    size_t index) {
    srem_model_batch_t *batch = worker->batch;
    srem_model_ticket_t *ticket = &batch->tickets[index];
    const size_t tile_index =
        index / batch->config.tile_width;
    const unsigned lane =
        (unsigned)(index % batch->config.tile_width);
    const uint32_t bit = UINT32_C(1) << lane;
    _Atomic uint32_t *ready;
    uint32_t old_pending;
    uint32_t expected;

    if (srem_model_make_ticket(batch, index, ticket) != 0) {
        return EPROTO;
    }
    worker->metrics.completions += UINT64_C(1);
    if (batch->tile_generations[tile_slot(batch, index)] !=
        ticket->generation) {
        worker->metrics.stale_tickets += UINT64_C(1);
        return 0;
    }
    if ((batch->tiles[tile_index].valid_mask & bit) == 0U ||
        ticket->event.site >= batch->config.site_count) {
        return EPROTO;
    }

    store_tile_event(batch, index, &ticket->event);
    old_pending = atomic_fetch_or_explicit(
        &batch->remote_pending_masks[tile_index],
        bit,
        memory_order_acq_rel);
    if ((old_pending & bit) != 0U) {
        worker->metrics.duplicate_tickets += UINT64_C(1);
        return 0;
    }
    ready = &batch->remote_ready_masks[
        tile_index * SREM_MODEL_MAX_SITES +
        ticket->event.site];
    (void)atomic_fetch_or_explicit(
        ready, bit, memory_order_release);
    expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &batch->remote_published[tile_index],
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        const int error =
            remote_queue_push(batch, (uint32_t)tile_index);

        if (error != 0) {
            atomic_store_explicit(
                &batch->remote_published[tile_index],
                0U,
                memory_order_release);
            return error;
        }
        worker->metrics.queue_pushes += UINT64_C(1);
    }
    worker->metrics.claims += UINT64_C(1);
    worker->metrics.remote_pushes += UINT64_C(1);
    return 0;
}

static int remote_worker_main(void *opaque) {
    srem_model_remote_worker_t *worker = opaque;
    srem_model_batch_t *batch = worker->batch;
    srem_model_remote_team_t *team = &batch->remote_team;
    uint64_t observed =
        srem_platform_event_epoch(team->start_event);

    if (observed == UINT64_MAX) {
        return EPROTO;
    }
    worker->affinity_result =
        srem_platform_pin_current_thread(worker->index + 1U);
    atomic_fetch_add_explicit(
        &team->ready_workers, 1U, memory_order_acq_rel);
    if (srem_platform_event_signal(team->ready_event) != 0) {
        return EPROTO;
    }
    for (;;) {
        uint64_t current;
        size_t position;
        int error;

        if (atomic_load_explicit(
                &team->stop, memory_order_acquire)) {
            return 0;
        }
        error = srem_platform_event_wait(
            team->start_event, observed);
        if (error != 0) {
            return error;
        }
        current =
            srem_platform_event_epoch(team->start_event);
        if (current == UINT64_MAX || current <= observed) {
            return EPROTO;
        }
        observed = current;
        if (atomic_load_explicit(
                &team->stop, memory_order_acquire)) {
            return 0;
        }

        worker->error = 0;
        for (position = worker->index;
             position < batch->remote_active_count;
             position += team->worker_count) {
            const size_t index =
                batch->remote_active_indices[position];

            if (batch->config.mode ==
                SREM_MODEL_REMOTE_WAKER_FRAME) {
                error = remote_publish_baseline(worker, index);
            } else if (batch->config.mode ==
                       SREM_MODEL_REMOTE_ADAPTIVE) {
                error = remote_publish_tile(worker, index);
            } else {
                error = ENOTSUP;
            }
            if (error != 0) {
                worker->error = error;
                break;
            }
        }
        if (atomic_fetch_add_explicit(
                &team->completed_workers,
                1U,
                memory_order_acq_rel) + 1U ==
            team->worker_count) {
            error =
                srem_platform_event_signal(team->done_event);
            if (error != 0) {
                return error;
            }
        }
    }
}

static int wait_for_remote_workers(
    srem_platform_event_t *event,
    const _Atomic unsigned *counter,
    unsigned target) {
    while (atomic_load_explicit(
               counter, memory_order_acquire) < target) {
        const uint64_t observed =
            srem_platform_event_epoch(event);
        int error;

        if (observed == UINT64_MAX) {
            return EPROTO;
        }
        if (atomic_load_explicit(
                counter, memory_order_acquire) >= target) {
            break;
        }
        error =
            srem_platform_event_wait(event, observed);
        if (error != 0) {
            return error;
        }
    }
    return 0;
}

static int remote_team_create(srem_model_batch_t *batch) {
    srem_model_remote_team_t *team = &batch->remote_team;
    unsigned index;
    int error;

    atomic_init(&team->ready_workers, 0U);
    atomic_init(&team->completed_workers, 0U);
    atomic_init(&team->stop, false);
    error = srem_platform_event_create(&team->start_event);
    if (error == 0) {
        error =
            srem_platform_event_create(&team->ready_event);
    }
    if (error == 0) {
        error =
            srem_platform_event_create(&team->done_event);
    }
    if (error != 0) {
        return error;
    }
    for (index = 0U;
         index < batch->config.remote_producers;
         ++index) {
        srem_model_remote_worker_t *worker =
            &team->workers[index];

        worker->batch = batch;
        worker->index = index;
        error = srem_platform_thread_start(
            &worker->thread, remote_worker_main, worker);
        if (error != 0) {
            return error;
        }
        team->worker_count += 1U;
    }
    return wait_for_remote_workers(
        team->ready_event,
        &team->ready_workers,
        team->worker_count);
}

static void remote_team_destroy(srem_model_batch_t *batch) {
    srem_model_remote_team_t *team;
    unsigned index;

    if (batch == NULL) {
        return;
    }
    team = &batch->remote_team;
    if (team->worker_count != 0U) {
        atomic_store_explicit(
            &team->stop, true, memory_order_release);
        if (team->start_event != NULL) {
            (void)srem_platform_event_signal(
                team->start_event);
        }
        for (index = 0U;
             index < team->worker_count;
             ++index) {
            if (team->workers[index].thread != NULL) {
                (void)srem_platform_thread_join(
                    team->workers[index].thread, NULL);
                team->workers[index].thread = NULL;
            }
        }
        team->worker_count = 0U;
    }
    srem_platform_event_destroy(team->done_event);
    srem_platform_event_destroy(team->ready_event);
    srem_platform_event_destroy(team->start_event);
    team->done_event = NULL;
    team->ready_event = NULL;
    team->start_event = NULL;
}

static void accumulate_metrics(
    srem_model_metrics_t *target,
    const srem_model_metrics_t *source) {
    target->completions += source->completions;
    target->claims += source->claims;
    target->stale_tickets += source->stale_tickets;
    target->duplicate_tickets += source->duplicate_tickets;
    target->queue_pushes += source->queue_pushes;
    target->queue_pops += source->queue_pops;
    target->resume_calls += source->resume_calls;
    target->tile_dispatches += source->tile_dispatches;
    target->scalar_lanes += source->scalar_lanes;
    target->vector_lanes += source->vector_lanes;
    target->vector_blocks += source->vector_blocks;
    target->forced_escapes += source->forced_escapes;
    target->remote_pushes += source->remote_pushes;
    target->fairness_samples += source->fairness_samples;
    if (source->fairness_p99_gap >
        target->fairness_p99_gap) {
        target->fairness_p99_gap =
            source->fairness_p99_gap;
    }
    target->hot_allocations += source->hot_allocations;
}

static int discard_remote_items(srem_model_batch_t *batch) {
    while (batch->remote_queue.dequeue_position !=
           atomic_load_explicit(
               &batch->remote_queue.enqueue_position,
               memory_order_acquire)) {
        uint32_t ignored;
        const int error =
            remote_queue_pop(&batch->remote_queue, &ignored);

        if (error != 0) {
            return error;
        }
    }
    return 0;
}

static int dispatch_remote_producers(
    srem_model_batch_t *batch,
    srem_model_metrics_t *metrics,
    uint64_t *out_queue_items) {
    srem_model_remote_team_t *team = &batch->remote_team;
    srem_model_metrics_t produced = {0};
    const size_t enqueue_position = atomic_load_explicit(
        &batch->remote_queue.enqueue_position,
        memory_order_acquire);
    unsigned index;
    int first_error = 0;
    int error;

    if (out_queue_items == NULL ||
        team->worker_count !=
            SREM_MODEL_REMOTE_PRODUCER_COUNT ||
        team->worker_count !=
            batch->config.remote_producers ||
        enqueue_position !=
            batch->remote_queue.dequeue_position) {
        return EPROTO;
    }
    error = build_remote_completion_stream(batch);
    if (error != 0) {
        return error;
    }
    if (batch->remote_active_count >
            batch->remote_queue.capacity ||
        enqueue_position >
            SIZE_MAX - batch->remote_active_count) {
        return EOVERFLOW;
    }
    atomic_store_explicit(
        &team->completed_workers, 0U, memory_order_release);
    for (index = 0U; index < team->worker_count; ++index) {
        memset(&team->workers[index].metrics,
               0,
               sizeof(team->workers[index].metrics));
        team->workers[index].error = 0;
    }
    {
        const uint64_t done_epoch =
            srem_platform_event_epoch(team->done_event);

        if (done_epoch == UINT64_MAX) {
            return EPROTO;
        }
        error =
            srem_platform_event_signal(team->start_event);
        if (error == 0) {
            error = srem_platform_event_wait(
                team->done_event, done_epoch);
        }
        if (error != 0) {
            return error;
        }
    }
    for (index = 0U; index < team->worker_count; ++index) {
        accumulate_metrics(
            &produced, &team->workers[index].metrics);
        if (first_error == 0 &&
            team->workers[index].error != 0) {
            first_error = team->workers[index].error;
        }
    }
    if (first_error != 0 ||
        produced.completions != batch->remote_active_count ||
        produced.claims != batch->remote_active_count ||
        produced.remote_pushes != batch->remote_active_count) {
        const int discard_error =
            discard_remote_items(batch);

        return discard_error != 0 ?
                   discard_error :
                   (first_error != 0 ?
                        first_error :
                        EPROTO);
    }
    *out_queue_items = produced.queue_pushes;
    accumulate_metrics(metrics, &produced);
    return 0;
}

static int resume_remote_baseline_index(
    srem_model_batch_t *batch,
    uint32_t index,
    srem_model_metrics_t *metrics) {
    srem_model_frame_core_t *frame;
    srem_model_waker_t *waker;
    uint64_t queued;
    unsigned resume_site;

    if (index >= batch->config.instance_count) {
        return EPROTO;
    }
    frame = srem_model_frame_at(batch, index);
    waker = &batch->wakers[index];
    queued = atomic_load_explicit(
        &waker->state_generation, memory_order_acquire);
    resume_site = waker->resume_site;
    if (srem_model_unpack_generation(queued) !=
            frame->generation ||
        srem_model_unpack_waker_state(queued) !=
            SREM_MODEL_WAKER_QUEUED ||
        resume_site >= batch->config.site_count) {
        return EPROTO;
    }
    atomic_store_explicit(
        &waker->state_generation,
        srem_model_pack_waker(
            frame->generation, SREM_MODEL_WAKER_RUNNING),
        memory_order_relaxed);
    batch->ops->resume_sites[resume_site](
        frame,
        &batch->events[index],
        &batch->effects[index],
        &batch->config,
        resume_site);
    waker->resume_site = frame->site;
    atomic_store_explicit(
        &waker->state_generation,
        srem_model_pack_waker(
            frame->generation, SREM_MODEL_WAKER_ARMED),
        memory_order_release);
    metrics->queue_pops += UINT64_C(1);
    metrics->resume_calls += UINT64_C(1);
    fairness_service(batch, index, metrics);
    return 0;
}

static int run_remote_baseline(
    srem_model_batch_t *batch,
    srem_model_metrics_t *metrics) {
    uint64_t queue_items = 0U;
    uint64_t position;
    int error =
        dispatch_remote_producers(
            batch, metrics, &queue_items);

    if (error != 0) {
        return error;
    }
    if (queue_items != batch->remote_active_count) {
        (void)discard_remote_items(batch);
        return EPROTO;
    }
    for (position = 0U; position < queue_items; ++position) {
        uint32_t index;

        error =
            remote_queue_pop(&batch->remote_queue, &index);
        if (error == 0) {
            error = resume_remote_baseline_index(
                batch, index, metrics);
        }
        if (error != 0) {
            (void)discard_remote_items(batch);
            return error;
        }
    }
    return 0;
}

static int import_remote_tile(srem_model_batch_t *batch,
                              uint32_t tile_index) {
    srem_model_tile_t *tile;
    uint32_t union_mask = 0U;
    uint32_t pending;
    unsigned site;

    if (tile_index >= batch->tile_count) {
        return EPROTO;
    }
    tile = &batch->tiles[tile_index];
    if (tile->queued != 0U || tile->running != 0U ||
        tile->pending_mask != 0U ||
        atomic_exchange_explicit(
            &batch->remote_published[tile_index],
            0U,
            memory_order_acq_rel) != 1U) {
        return EPROTO;
    }
    for (site = 0U;
         site < batch->config.site_count;
         ++site) {
        const size_t offset =
            (size_t)tile_index * SREM_MODEL_MAX_SITES + site;
        const uint32_t mask = atomic_exchange_explicit(
            &batch->remote_ready_masks[offset],
            0U,
            memory_order_acq_rel);

        if ((mask & ~tile->valid_mask) != 0U ||
            (union_mask & mask) != 0U) {
            return EPROTO;
        }
        tile->ready_mask[site] = mask;
        union_mask |= mask;
    }
    pending = atomic_exchange_explicit(
        &batch->remote_pending_masks[tile_index],
        0U,
        memory_order_acq_rel);
    if (union_mask == 0U || pending != union_mask) {
        return EPROTO;
    }
    tile->pending_mask = union_mask;
    if (queue_push(
            &batch->local_queue, tile_index) != 0) {
        return ENOSPC;
    }
    tile->queued = 1U;

    atomic_thread_fence(memory_order_acquire);
    for (site = 0U;
         site < batch->config.site_count;
         ++site) {
        const size_t offset =
            (size_t)tile_index * SREM_MODEL_MAX_SITES + site;

        if (atomic_load_explicit(
                &batch->remote_ready_masks[offset],
                memory_order_acquire) != 0U) {
            return EBUSY;
        }
    }
    return 0;
}

static int run_remote_tiles(
    srem_model_batch_t *batch,
    srem_model_metrics_t *metrics) {
    uint64_t queue_items = 0U;
    uint64_t position;
    int error =
        dispatch_remote_producers(
            batch, metrics, &queue_items);

    if (error != 0) {
        return error;
    }
    if (queue_items == 0U ||
        queue_items > batch->tile_count) {
        (void)discard_remote_items(batch);
        return EPROTO;
    }
    for (position = 0U; position < queue_items; ++position) {
        uint32_t tile_index;

        error = remote_queue_pop(
            &batch->remote_queue, &tile_index);
        if (error == 0) {
            error = import_remote_tile(batch, tile_index);
        }
        if (error != 0) {
            (void)discard_remote_items(batch);
            return error;
        }
    }
    return srem_model_tile_drain(batch, metrics);
}

int srem_model_run_round(srem_model_batch_t *batch,
                         srem_model_metrics_t *metrics) {
    int error;

    if (batch == NULL || metrics == NULL) {
        return EINVAL;
    }
    if (batch->config.mode == SREM_MODEL_TILE_SCALAR ||
        batch->config.mode == SREM_MODEL_TILE_VECTOR ||
        batch->config.mode == SREM_MODEL_ADAPTIVE) {
        error = admit_tile_round(batch, metrics);
        if (error == 0) {
            error = srem_model_tile_drain(batch, metrics);
        }
        if (error == 0) {
            batch->round += 1U;
            update_fairness_p99(batch, metrics);
        }
        return error;
    }
    if (batch->config.mode == SREM_MODEL_REMOTE_ADAPTIVE) {
        error = run_remote_tiles(batch, metrics);
        if (error == 0) {
            batch->round += 1U;
            update_fairness_p99(batch, metrics);
        }
        return error;
    }
    if (batch->config.mode == SREM_MODEL_REMOTE_WAKER_FRAME) {
        error = run_remote_baseline(batch, metrics);
        if (error == 0) {
            batch->round += 1U;
            update_fairness_p99(batch, metrics);
        }
        return error;
    }
    if (batch->config.mode != SREM_MODEL_WAKER_FRAME) {
        return EINVAL;
    }
    error = admit_baseline_round(batch, metrics);
    if (error == 0) {
        error = drain_baseline_round(batch, metrics);
    }
    if (error == 0) {
        batch->round += 1U;
        update_fairness_p99(batch, metrics);
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
        srem_model_frame_core_t lhs_frame;
        srem_model_frame_core_t rhs_frame;
        srem_model_effect_t lhs_effect;
        srem_model_effect_t rhs_effect;

        project_frame(lhs, index, &lhs_frame);
        project_frame(rhs, index, &rhs_frame);
        project_effect(lhs, index, &lhs_effect);
        project_effect(rhs, index, &rhs_effect);
        if (!frame_equal(&lhs_frame, &rhs_frame) ||
            !effect_equal(&lhs_effect, &rhs_effect)) {
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
        srem_model_frame_core_t frame;
        srem_model_effect_t effect;
        unsigned field;

        project_frame(batch, index, &frame);
        project_effect(batch, index, &effect);
        for (field = 0U; field < 6U; ++field) {
            hash = hash_u64(hash, frame.field[field]);
        }
        hash = hash_u64(hash, frame.generation);
        hash = hash_u64(hash, frame.site);
        hash = hash_u64(hash, frame.steps);
        hash = hash_u64(hash, frame.terminal);
        hash = hash_u64(hash, frame.flags);
        hash = hash_u64(hash, effect.argument0);
        hash = hash_u64(hash, effect.argument1);
        hash = hash_u64(hash, effect.argument2);
        hash = hash_u64(hash, effect.operation);
        hash = hash_u64(hash, effect.next_site);
        hash = hash_u64(hash, effect.flags);
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
