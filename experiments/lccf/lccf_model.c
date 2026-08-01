// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lccf_model_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define LCCF_MODEL_STATE_BITS 3U
#define LCCF_MODEL_STATE_MASK UINT64_C(7)
#define LCCF_MODEL_MAX_GENERATION                                          \
    (UINT64_MAX >> LCCF_MODEL_STATE_BITS)

static const char *const MODE_NAMES[] = {
    "waker_queue",
    "causal_cell_queue",
    "fused_causal_cell",
    "budgeted_fused_chain",
    "remote_waker_queue",
    "remote_causal_cell",
    "recompute_queue",
    "shared_fact_queue",
    "recompute_fused",
    "shared_fact_fused",
    "mixed_recompute",
    "mixed_shared_fact",
};

static const char *const WORKLOAD_NAMES[] = {
    "completion_io_pipeline",
    "completion_rpc_state",
    "completion_timer_cancel",
    "completion_mixed_fairness",
};

static int remote_team_create(lccf_model_batch_t *batch);
static void remote_team_destroy(lccf_model_batch_t *batch);
static int invoke_resolved_fact_site(
    const lccf_fact_site_descriptor_t *descriptor,
    const lccf_fact_core_t *fact,
    void *context);

static bool mode_valid(lccf_model_mode_t mode) {
    return mode >= LCCF_MODEL_WAKER_QUEUE &&
           mode < LCCF_MODEL_MODE_COUNT;
}

static bool workload_valid(lccf_model_workload_t workload) {
    return workload >= LCCF_MODEL_COMPLETION_IO_PIPELINE &&
           workload <= LCCF_MODEL_COMPLETION_MIXED_FAIRNESS;
}

static bool mode_uses_wakers(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_WAKER_QUEUE ||
           mode == LCCF_MODEL_REMOTE_WAKER_QUEUE;
}

static bool mode_uses_cells(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_CAUSAL_CELL_QUEUE ||
           mode == LCCF_MODEL_FUSED_CAUSAL_CELL ||
           mode == LCCF_MODEL_BUDGETED_FUSED_CHAIN ||
           mode == LCCF_MODEL_REMOTE_CAUSAL_CELL ||
           mode == LCCF_MODEL_RECOMPUTE_QUEUE ||
           mode == LCCF_MODEL_SHARED_FACT_QUEUE ||
           mode == LCCF_MODEL_RECOMPUTE_FUSED ||
           mode == LCCF_MODEL_SHARED_FACT_FUSED ||
           mode == LCCF_MODEL_MIXED_RECOMPUTE ||
           mode == LCCF_MODEL_MIXED_SHARED_FACT;
}

static bool mode_uses_facts(lccf_model_mode_t mode) {
    return mode >= LCCF_MODEL_RECOMPUTE_QUEUE &&
           mode <= LCCF_MODEL_MIXED_SHARED_FACT;
}

static bool mode_shares_facts(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_SHARED_FACT_QUEUE ||
           mode == LCCF_MODEL_SHARED_FACT_FUSED ||
           mode == LCCF_MODEL_MIXED_SHARED_FACT;
}

static bool mode_is_fact_queue(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_RECOMPUTE_QUEUE ||
           mode == LCCF_MODEL_SHARED_FACT_QUEUE;
}

static bool mode_is_fact_fused(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_RECOMPUTE_FUSED ||
           mode == LCCF_MODEL_SHARED_FACT_FUSED;
}

static bool mode_is_fact_mixed(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_MIXED_RECOMPUTE ||
           mode == LCCF_MODEL_MIXED_SHARED_FACT;
}

static bool mode_is_remote(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_REMOTE_WAKER_QUEUE ||
           mode == LCCF_MODEL_REMOTE_CAUSAL_CELL;
}

static bool frame_bytes_valid(size_t frame_bytes) {
    return frame_bytes == 64U || frame_bytes == 128U ||
           frame_bytes == 256U;
}

static bool cell_bytes_valid(size_t cell_bytes) {
    return cell_bytes == 64U || cell_bytes == 96U ||
           cell_bytes == 128U;
}

static bool site_count_valid(unsigned site_count) {
    return site_count == 1U || site_count == LCCF_MODEL_MAX_SITES;
}

static bool allocation_size_valid(size_t count, size_t element_size) {
    return element_size == 0U || count <= SIZE_MAX / element_size;
}

uint64_t lccf_model_mix64(uint64_t value) {
    value = (value ^ (value >> 30U)) *
            UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) *
            UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

uint64_t lccf_model_pack_state(uint64_t generation,
                               lccf_model_state_t state) {
    return (generation << LCCF_MODEL_STATE_BITS) | (uint64_t)state;
}

uint64_t lccf_model_unpack_generation(uint64_t word) {
    return word >> LCCF_MODEL_STATE_BITS;
}

lccf_model_state_t lccf_model_unpack_state(uint64_t word) {
    return (lccf_model_state_t)(word & LCCF_MODEL_STATE_MASK);
}

static size_t next_power_of_two(size_t value) {
    size_t power = 1U;

    while (power < value) {
        if (power > SIZE_MAX / 2U) {
            return 0U;
        }
        power *= 2U;
    }
    return power;
}

static lccf_model_frame_core_t *
frame_at(const lccf_model_batch_t *batch, size_t index) {
    return (lccf_model_frame_core_t *)(void *)(
        batch->frame_storage + index * batch->config.frame_bytes);
}

lccf_model_cell_hot_t *lccf_model_cell_at(
    const lccf_model_batch_t *batch,
    size_t index) {
    if (batch == NULL || batch->cell_storage == NULL ||
        index >= batch->config.instance_count) {
        return NULL;
    }
    return (lccf_model_cell_hot_t *)(void *)(
        batch->cell_storage + index * batch->config.cell_bytes);
}

static lccf_fact_cell_t *fact_cell_at(
    const lccf_model_batch_t *batch,
    size_t index) {
    if (batch == NULL || batch->fact_cells == NULL ||
        index >= batch->config.instance_count) {
        return NULL;
    }
    return &batch->fact_cells[index];
}

static lccf_fact_core_t *fact_storage_at(
    const lccf_model_batch_t *batch,
    size_t index) {
    if (batch == NULL || index >= batch->config.instance_count ||
        !mode_uses_facts(batch->config.mode)) {
        return NULL;
    }
    if (batch->config.cell_bytes == 128U) {
        return (lccf_fact_core_t *)(void *)(
            batch->cell_storage + index * batch->config.cell_bytes + 64U);
    }
    if (batch->fact_sidecar_storage == NULL) {
        return NULL;
    }
    return &batch->fact_sidecar_storage[index];
}

static lccf_fact_layout_t fact_layout_for_cell_bytes(size_t cell_bytes) {
    switch (cell_bytes) {
    case 64U:
        return LCCF_FACT_LAYOUT_SPLIT64_64;
    case 96U:
        return LCCF_FACT_LAYOUT_SPLIT96_64;
    case 128U:
        return LCCF_FACT_LAYOUT_UNIFIED128;
    default:
        return LCCF_FACT_LAYOUT_COUNT;
    }
}

lccf_model_ticket_t *lccf_model_ticket_at(
    const lccf_model_batch_t *batch,
    size_t instance_index,
    unsigned ticket_index) {
    if (batch == NULL || batch->tickets == NULL ||
        instance_index >= batch->config.instance_count ||
        ticket_index >= LCCF_MODEL_TICKETS_PER_INSTANCE) {
        return NULL;
    }
    return &batch->tickets[
        instance_index * LCCF_MODEL_TICKETS_PER_INSTANCE +
        ticket_index];
}

static void initialize_frame_bytes(lccf_model_batch_t *batch,
                                   size_t index) {
    unsigned char *bytes =
        batch->frame_storage + index * batch->config.frame_bytes;
    uint64_t stream =
        batch->config.seed ^ ((uint64_t)(uint32_t)index << 32U) ^
        (uint64_t)index ^ UINT64_C(0xD1B54A32D192ED03);
    size_t offset;

    for (offset = 0U; offset < batch->config.frame_bytes; ++offset) {
        if ((offset & 7U) == 0U) {
            stream = lccf_model_mix64(
                stream + UINT64_C(0x9E3779B97F4A7C15));
        }
        bytes[offset] =
            (unsigned char)(stream >> ((offset & 7U) * 8U));
    }
}

static unsigned active_ticket_count(const lccf_model_batch_t *batch) {
    return batch->config.workload ==
                   LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
               LCCF_MODEL_TICKETS_PER_INSTANCE :
               1U;
}

static uint32_t ticket_event_kind(unsigned ticket_index) {
    static const uint32_t kinds[LCCF_MODEL_TICKETS_PER_INSTANCE] = {
        LCCF_MODEL_EVENT_IO,
        LCCF_MODEL_EVENT_TIMEOUT,
        LCCF_MODEL_EVENT_CANCEL,
    };

    return kinds[ticket_index];
}

static int prepare_causal_generation(lccf_model_batch_t *batch,
                                     lccf_model_instance_t *instance,
                                     bool require_retired) {
    lccf_model_cell_hot_t *cell = instance->cell;
    lccf_model_event_t base_event;
    const unsigned count = active_ticket_count(batch);
    unsigned ticket_index;

    if (cell == NULL ||
        (require_retired &&
         atomic_load_explicit(&cell->backend_refs,
                              memory_order_acquire) != 0U)) {
        return EBUSY;
    }
    lccf_model_derive_event(batch, instance, &base_event);
    for (ticket_index = 0U;
         ticket_index < LCCF_MODEL_TICKETS_PER_INSTANCE;
         ++ticket_index) {
        lccf_model_ticket_t *ticket = lccf_model_ticket_at(
            batch, instance->index, ticket_index);

        if (ticket == NULL) {
            return EPROTO;
        }
        memset(ticket, 0, sizeof(*ticket));
        if (ticket_index < count) {
            ticket->target = cell;
            ticket->generation = instance->frame->generation;
            ticket->event = base_event;
            if (count > 1U) {
                ticket->event.kind =
                    ticket_event_kind(ticket_index);
            }
            ticket->instance_index = instance->index;
            ticket->ticket_index = ticket_index;
        }
    }
    cell->next_site = instance->frame->site;
    atomic_store_explicit(
        &cell->backend_refs,
        mode_uses_facts(batch->config.mode) ? 0U : count,
        memory_order_release);
    atomic_store_explicit(
        &cell->state_generation,
        lccf_model_pack_state(instance->frame->generation,
                              LCCF_MODEL_STATE_ARMED),
        memory_order_release);
    return 0;
}

static int initialize_instance(lccf_model_batch_t *batch,
                               size_t index,
                               bool initialize_atomic) {
    lccf_model_instance_t *instance = &batch->instances[index];
    lccf_model_frame_core_t *frame;
    lccf_model_waker_t *waker =
        batch->wakers == NULL ? NULL : &batch->wakers[index];
    lccf_model_cell_hot_t *cell =
        lccf_model_cell_at(batch, index);
    lccf_fact_cell_t *fact_cell = fact_cell_at(batch, index);
    const uint64_t identity =
        batch->config.seed ^ ((uint64_t)(uint32_t)index << 32U) ^
        (uint64_t)index;

    initialize_frame_bytes(batch, index);
    frame = frame_at(batch, index);
    frame->state0 =
        lccf_model_mix64(identity ^ UINT64_C(0x243F6A8885A308D3));
    frame->state1 =
        lccf_model_mix64(identity ^ UINT64_C(0x13198A2E03707344));
    frame->state2 =
        lccf_model_mix64(identity ^ UINT64_C(0xA4093822299F31D0));
    frame->output =
        lccf_model_mix64(identity ^ UINT64_C(0x452821E638D01377));
    frame->generation = UINT64_C(1);
    frame->command_word = 0U;
    frame->site = (uint32_t)(index % batch->config.site_count);
    frame->steps = 0U;
    frame->terminal = 0U;
    frame->reserved = 0U;

    instance->batch = batch;
    instance->frame = frame;
    instance->waker = waker;
    instance->cell = cell;
    instance->fact_cell = fact_cell;
    memset(&instance->event, 0, sizeof(instance->event));
    memset(&instance->command, 0, sizeof(instance->command));
    instance->event_sequence_hash = UINT64_C(0x4556454e54534551);
    instance->command_sequence_hash = UINT64_C(0x434f4d4d414e4453);
    instance->callback_sequence_count = 0U;
    instance->overflow_next = NULL;
    instance->index = (uint32_t)index;
    instance->reserved = 0U;

    if (waker != NULL) {
        if (initialize_atomic) {
            atomic_init(
                &waker->state_generation,
                lccf_model_pack_state(frame->generation,
                                      LCCF_MODEL_STATE_ARMED));
        } else {
            atomic_store_explicit(
                &waker->state_generation,
                lccf_model_pack_state(frame->generation,
                                      LCCF_MODEL_STATE_ARMED),
                memory_order_relaxed);
        }
        waker->instance = instance;
        waker->resume_site = frame->site;
        waker->reserved = 0U;
    }
    if (cell != NULL) {
        if (initialize_atomic) {
            atomic_init(&cell->state_generation, 0U);
            atomic_init(&cell->queue_owned, 0U);
            atomic_init(&cell->backend_refs, 0U);
        } else {
            atomic_store_explicit(
                &cell->state_generation, 0U, memory_order_relaxed);
            atomic_store_explicit(
                &cell->queue_owned, 0U, memory_order_relaxed);
            atomic_store_explicit(
                &cell->backend_refs, 0U, memory_order_relaxed);
        }
        cell->instance = instance;
        memset(&cell->event, 0, sizeof(cell->event));
        cell->command_word = 0U;
        cell->home_shard = 0U;
        cell->next_site = frame->site;
        if (prepare_causal_generation(batch, instance, false) != 0) {
            return EPROTO;
        }
    }
    if (fact_cell != NULL &&
        lccf_fact_cell_init(
            fact_cell, frame->generation,
            fact_layout_for_cell_bytes(batch->config.cell_bytes),
            active_ticket_count(batch), fact_storage_at(batch, index)) != 0) {
        return EPROTO;
    }
    return 0;
}

static int queue_push(lccf_model_local_queue_t *queue, void *item) {
    if (queue == NULL || item == NULL || queue->slots == NULL ||
        queue->tail - queue->head >= queue->capacity) {
        return EAGAIN;
    }
    queue->slots[queue->tail & queue->mask] = item;
    queue->tail += 1U;
    return 0;
}

static void *queue_pop(lccf_model_local_queue_t *queue) {
    void *item;

    if (queue == NULL || queue->slots == NULL ||
        queue->head == queue->tail) {
        return NULL;
    }
    item = queue->slots[queue->head & queue->mask];
    queue->slots[queue->head & queue->mask] = NULL;
    queue->head += 1U;
    return item;
}

static int overflow_push(lccf_model_batch_t *batch,
                         lccf_model_cell_hot_t *cell) {
    lccf_model_instance_t *instance;

    if (batch == NULL || cell == NULL || cell->instance == NULL) {
        return EINVAL;
    }
    instance = cell->instance;
    if (instance->overflow_next != NULL ||
        batch->overflow_tail == instance) {
        return EPROTO;
    }
    if (batch->overflow_tail == NULL) {
        batch->overflow_head = instance;
    } else {
        batch->overflow_tail->overflow_next = instance;
    }
    batch->overflow_tail = instance;
    return 0;
}

static lccf_model_cell_hot_t *dequeue_cell(
    lccf_model_batch_t *batch,
    lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell = queue_pop(&batch->local_queue);

    if (cell != NULL) {
        return cell;
    }
    if (batch->overflow_head != NULL) {
        lccf_model_instance_t *instance = batch->overflow_head;

        batch->overflow_head = instance->overflow_next;
        if (batch->overflow_head == NULL) {
            batch->overflow_tail = NULL;
        }
        instance->overflow_next = NULL;
        if (mode_uses_facts(batch->config.mode)) {
            metrics->fact_overflow_pops += UINT64_C(1);
        }
        return instance->cell;
    }
    return NULL;
}

void lccf_model_derive_event(const lccf_model_batch_t *batch,
                             const lccf_model_instance_t *instance,
                             lccf_model_event_t *event) {
    const lccf_model_frame_core_t *frame = instance->frame;
    const uint64_t identity =
        batch->config.seed ^
        ((uint64_t)instance->index << 32U) ^
        frame->generation;

    event->word0 =
        lccf_model_mix64(identity ^ frame->output ^
                         UINT64_C(0xA0761D6478BD642F));
    event->word1 =
        lccf_model_mix64(event->word0 ^ frame->state1 ^
                         UINT64_C(0xE7037ED1A0B428DB));
    if (batch->config.workload ==
        LCCF_MODEL_COMPLETION_TIMER_CANCEL) {
        static const uint32_t kinds[] = {
            LCCF_MODEL_EVENT_IO,
            LCCF_MODEL_EVENT_TIMEOUT,
            LCCF_MODEL_EVENT_CANCEL,
        };
        event->kind =
            kinds[(unsigned)(event->word1 % UINT64_C(3))];
    } else if (batch->config.workload ==
               LCCF_MODEL_COMPLETION_MIXED_FAIRNESS) {
        event->kind =
            (instance->index & 31U) == 0U ?
                LCCF_MODEL_EVENT_TIMER :
                LCCF_MODEL_EVENT_IO;
    } else {
        event->kind = LCCF_MODEL_EVENT_IO;
    }
    event->error_code =
        event->kind == LCCF_MODEL_EVENT_IO &&
                (event->word0 & UINT64_C(7)) == 0U
            ? EIO
            : 0;
}

static bool config_valid(const lccf_model_config_t *config) {
    if (config == NULL || !workload_valid(config->workload) ||
        !mode_valid(config->mode) ||
        config->instance_count == 0U ||
        config->instance_count > UINT32_MAX ||
        !frame_bytes_valid(config->frame_bytes) ||
        !cell_bytes_valid(config->cell_bytes) ||
        !site_count_valid(config->site_count) ||
        config->direct_budget == 0U ||
        config->direct_budget > LCCF_MODEL_MAX_DIRECT_BUDGET ||
        config->chain_length == 0U ||
        config->chain_length > LCCF_MODEL_MAX_CHAIN_LENGTH ||
        config->callback_failure_step > config->chain_length ||
        config->fact_queue_capacity > config->instance_count ||
        config->remote_producers >
            LCCF_MODEL_REMOTE_PRODUCER_COUNT ||
        (mode_is_remote(config->mode) &&
         config->remote_producers !=
             LCCF_MODEL_REMOTE_PRODUCER_COUNT) ||
        config->instance_count == SIZE_MAX) {
        return false;
    }
    if (!allocation_size_valid(config->instance_count,
                               config->frame_bytes) ||
        !allocation_size_valid(config->instance_count,
                               sizeof(lccf_model_instance_t)) ||
        !allocation_size_valid(config->instance_count,
                               sizeof(lccf_model_waker_t)) ||
        !allocation_size_valid(
            config->instance_count,
            LCCF_MODEL_TICKETS_PER_INSTANCE *
                sizeof(lccf_model_ticket_t)) ||
        !allocation_size_valid(config->instance_count,
                               config->cell_bytes) ||
        (mode_uses_facts(config->mode) &&
         !allocation_size_valid(config->instance_count,
                                sizeof(lccf_fact_cell_t))) ||
        (mode_uses_facts(config->mode) && config->cell_bytes < 128U &&
         !allocation_size_valid(config->instance_count,
                                sizeof(lccf_fact_core_t))) ||
        config->instance_count >
            SIZE_MAX / (size_t)config->chain_length) {
        return false;
    }
    return true;
}

int lccf_model_batch_create(const lccf_model_config_t *config,
                            lccf_model_batch_t **out_batch) {
    lccf_model_batch_t *batch;
    size_t queue_capacity;
    size_t i;

    if (out_batch == NULL) {
        return EINVAL;
    }
    *out_batch = NULL;
    if (!config_valid(config)) {
        return EINVAL;
    }
    if (!mode_uses_wakers(config->mode) &&
        !mode_uses_cells(config->mode)) {
        return ENOTSUP;
    }

    queue_capacity = next_power_of_two(
        mode_uses_facts(config->mode) && config->fact_queue_capacity != 0U
            ? config->fact_queue_capacity
            : config->instance_count + 1U);
    if (queue_capacity == 0U ||
        !allocation_size_valid(queue_capacity, sizeof(void *)) ||
        (mode_is_remote(config->mode) &&
         !allocation_size_valid(
             queue_capacity,
             sizeof(lccf_model_remote_slot_t)))) {
        return EOVERFLOW;
    }
    batch = calloc(1U, sizeof(*batch));
    if (batch == NULL) {
        return ENOMEM;
    }
    batch->config = *config;
    batch->frame_storage =
        calloc(config->instance_count, config->frame_bytes);
    batch->instances =
        calloc(config->instance_count, sizeof(*batch->instances));
    if (mode_uses_wakers(config->mode)) {
        batch->wakers =
            calloc(config->instance_count, sizeof(*batch->wakers));
    }
    if (mode_uses_cells(config->mode)) {
        batch->cell_storage =
            calloc(config->instance_count, config->cell_bytes);
        batch->tickets = calloc(
            config->instance_count *
                LCCF_MODEL_TICKETS_PER_INSTANCE,
            sizeof(*batch->tickets));
    }
    if (mode_uses_facts(config->mode)) {
        batch->fact_cells =
            calloc(config->instance_count, sizeof(*batch->fact_cells));
        if (config->cell_bytes < 128U) {
            batch->fact_sidecar_storage = calloc(
                config->instance_count,
                sizeof(*batch->fact_sidecar_storage));
        }
    }
    batch->local_queue.slots =
        calloc(queue_capacity, sizeof(*batch->local_queue.slots));
    if (mode_is_remote(config->mode)) {
        batch->remote_queue.slots = calloc(
            queue_capacity, sizeof(*batch->remote_queue.slots));
    }
    if (batch->frame_storage == NULL || batch->instances == NULL ||
        batch->local_queue.slots == NULL ||
        (mode_is_remote(config->mode) &&
         batch->remote_queue.slots == NULL) ||
        (mode_uses_wakers(config->mode) &&
         batch->wakers == NULL) ||
        (mode_uses_cells(config->mode) &&
         (batch->cell_storage == NULL || batch->tickets == NULL)) ||
        (mode_uses_facts(config->mode) &&
         (batch->fact_cells == NULL ||
          (config->cell_bytes < 128U &&
           batch->fact_sidecar_storage == NULL)))) {
        lccf_model_batch_destroy(batch);
        return ENOMEM;
    }
    batch->local_queue.capacity = queue_capacity;
    batch->local_queue.mask = queue_capacity - 1U;
    if (mode_is_remote(config->mode)) {
        batch->remote_queue.capacity = queue_capacity;
        batch->remote_queue.mask = queue_capacity - 1U;
        atomic_init(
            &batch->remote_queue.enqueue_position, 0U);
        for (i = 0U; i < queue_capacity; ++i) {
            atomic_init(
                &batch->remote_queue.slots[i].sequence, i);
        }
    }
    batch->ops = lccf_model_get_workload_ops(config->workload);
    if (batch->ops == NULL) {
        lccf_model_batch_destroy(batch);
        return EPROTO;
    }
    if (mode_uses_facts(config->mode)) {
        for (i = 0U; i < config->site_count; ++i) {
            batch->fact_sites[i].descriptor.invoke =
                invoke_resolved_fact_site;
            batch->fact_sites[i].descriptor.logical_index = (uint32_t)i;
            batch->fact_sites[i].descriptor.reserved = 0U;
            batch->fact_sites[i].resume = batch->ops->resume_sites[i];
            batch->fact_site_table[i] =
                &batch->fact_sites[i].descriptor;
            if (batch->fact_sites[i].resume == NULL) {
                lccf_model_batch_destroy(batch);
                return EPROTO;
            }
        }
    }
    for (i = 0U; i < config->instance_count; ++i) {
        if (initialize_instance(batch, i, true) != 0) {
            lccf_model_batch_destroy(batch);
            return EPROTO;
        }
    }
    if (mode_is_remote(config->mode)) {
        const int remote_rc = remote_team_create(batch);

        if (remote_rc != 0) {
            lccf_model_batch_destroy(batch);
            return remote_rc;
        }
    }
    *out_batch = batch;
    return 0;
}

void lccf_model_batch_destroy(lccf_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    remote_team_destroy(batch);
    free(batch->remote_queue.slots);
    free(batch->local_queue.slots);
    free(batch->tickets);
    free(batch->wakers);
    free(batch->fact_cells);
    free(batch->fact_sidecar_storage);
    free(batch->cell_storage);
    free(batch->instances);
    free(batch->frame_storage);
    free(batch);
}

int lccf_model_batch_reset(lccf_model_batch_t *batch) {
    size_t i;

    if (batch == NULL || batch->local_queue.slots == NULL ||
        batch->local_queue.head != batch->local_queue.tail ||
        batch->overflow_head != NULL || batch->overflow_tail != NULL ||
        (mode_is_remote(batch->config.mode) &&
         (batch->remote_queue.slots == NULL ||
          atomic_load_explicit(
              &batch->remote_queue.enqueue_position,
              memory_order_acquire) !=
              batch->remote_queue.dequeue_position))) {
        return EINVAL;
    }
    memset(batch->local_queue.slots,
           0,
           batch->local_queue.capacity *
               sizeof(*batch->local_queue.slots));
    batch->local_queue.head = 0U;
    batch->local_queue.tail = 0U;
    batch->overflow_head = NULL;
    batch->overflow_tail = NULL;
    batch->trace_count = 0U;
    batch->round = 0U;
    batch->callback_depth = 0U;
    batch->maximum_callback_depth = 0U;
    batch->fairness_tick = 0U;
    batch->fairness_due_ns = 0U;
    batch->fairness_services = 0U;
    memset(batch->fairness_histogram,
           0,
           sizeof(batch->fairness_histogram));
    batch->fairness_due = false;
    for (i = 0U; i < batch->config.instance_count; ++i) {
        if (initialize_instance(batch, i, false) != 0) {
            return EPROTO;
        }
    }
    return 0;
}

static bool metric_room(uint64_t current, uint64_t increment) {
    return increment <= UINT64_MAX - current;
}

static int validate_metrics(const lccf_model_batch_t *batch,
                            const lccf_model_metrics_t *metrics) {
    uint64_t callback_count;
    const uint64_t instances =
        (uint64_t)batch->config.instance_count;

    if (instances >
        UINT64_MAX / (uint64_t)batch->config.chain_length) {
        return EOVERFLOW;
    }
    callback_count =
        instances * (uint64_t)batch->config.chain_length;
    if (batch->trace_rows != NULL &&
        (batch->trace_count > batch->trace_capacity ||
         callback_count > batch->trace_capacity - batch->trace_count)) {
        return ENOSPC;
    }
    if (!metric_room(metrics->completions, instances) ||
        !metric_room(metrics->claims, instances) ||
        !metric_room(metrics->queue_pushes, callback_count) ||
        !metric_room(metrics->queue_pops, callback_count) ||
        !metric_room(metrics->resume_calls, callback_count) ||
        !metric_room(metrics->direct_calls, callback_count) ||
        !metric_room(metrics->forced_escapes, callback_count) ||
        !metric_room(metrics->remote_pushes, instances) ||
        !metric_room(metrics->fairness_samples, callback_count) ||
        (mode_uses_facts(batch->config.mode) &&
         (!metric_room(metrics->facts_attempted,
                       instances * LCCF_MODEL_TICKETS_PER_INSTANCE) ||
          !metric_room(metrics->facts_built, instances) ||
          !metric_room(metrics->facts_build_failed, instances) ||
          !metric_room(metrics->fact_normalizations,
                       callback_count + instances) ||
          !metric_room(metrics->fact_site_lookups,
                       callback_count + instances) ||
          !metric_room(metrics->fact_module_pins, instances) ||
          !metric_room(metrics->fact_payload_pins, instances) ||
          !metric_room(metrics->fact_stale_losers,
                       instances * UINT64_C(2)) ||
          !metric_room(metrics->fact_guard_rechecks,
                       callback_count + instances) ||
          !metric_room(metrics->fact_queue_forwards, callback_count) ||
          !metric_room(metrics->fact_generation_mismatches, instances) ||
          !metric_room(metrics->fact_reuse_delays, instances) ||
          !metric_room(metrics->fact_overflow_pushes, callback_count) ||
          !metric_room(metrics->fact_overflow_pops, callback_count))) ||
        !metric_room(batch->fairness_tick, callback_count) ||
        !metric_room(batch->fairness_services, callback_count) ||
        (mode_uses_cells(batch->config.mode) &&
         batch->config.workload ==
             LCCF_MODEL_COMPLETION_TIMER_CANCEL &&
         !metric_room(metrics->stale_tickets,
                      instances * UINT64_C(2)))) {
        return EOVERFLOW;
    }
    return 0;
}

static int validate_instances(const lccf_model_batch_t *batch) {
    size_t i;

    if (batch->round == UINT64_MAX) {
        return EOVERFLOW;
    }
    for (i = 0U; i < batch->config.instance_count; ++i) {
        const lccf_model_instance_t *instance =
            &batch->instances[i];

        if (instance->batch != batch ||
            instance->frame != frame_at(batch, i) ||
            instance->index != i ||
            instance->frame->generation == 0U ||
            instance->frame->generation > LCCF_MODEL_MAX_GENERATION ||
            instance->frame->site >= batch->config.site_count) {
            return EPROTO;
        }
        if (instance->frame->generation ==
                LCCF_MODEL_MAX_GENERATION ||
            instance->frame->steps >
                UINT32_MAX - batch->config.chain_length) {
            return EOVERFLOW;
        }
        if (mode_uses_wakers(batch->config.mode)) {
            const lccf_model_waker_t *waker = &batch->wakers[i];
            const uint64_t word = atomic_load_explicit(
                &waker->state_generation,
                memory_order_acquire);

            if (instance->waker != waker ||
                instance->cell != NULL ||
                waker->instance != instance ||
                waker->resume_site != instance->frame->site ||
                lccf_model_unpack_generation(word) !=
                    instance->frame->generation ||
                lccf_model_unpack_state(word) !=
                    LCCF_MODEL_STATE_ARMED) {
                return EPROTO;
            }
        } else if (mode_uses_cells(batch->config.mode)) {
            const lccf_model_cell_hot_t *cell =
                lccf_model_cell_at(batch, i);
            const unsigned refs = active_ticket_count(batch);
            const unsigned cell_refs =
                mode_uses_facts(batch->config.mode) ? 0U : refs;
            uint64_t word;
            unsigned ticket_index;

            if (cell == NULL) {
                return EPROTO;
            }
            word = atomic_load_explicit(
                &cell->state_generation,
                memory_order_acquire);
            if (instance->waker != NULL ||
                instance->cell != cell ||
                cell->instance != instance ||
                cell->next_site != instance->frame->site ||
                lccf_model_unpack_generation(word) !=
                    instance->frame->generation ||
                lccf_model_unpack_state(word) !=
                    LCCF_MODEL_STATE_ARMED ||
                atomic_load_explicit(&cell->queue_owned,
                                     memory_order_acquire) != 0U ||
                atomic_load_explicit(&cell->backend_refs,
                                     memory_order_acquire) != cell_refs) {
                return EPROTO;
            }
            if (mode_uses_facts(batch->config.mode)) {
                const lccf_fact_cell_t *fact_cell = fact_cell_at(batch, i);
                const uint64_t fact_word =
                    fact_cell == NULL ? 0U : atomic_load_explicit(
                        &fact_cell->state_generation,
                        memory_order_acquire);

                if (instance->fact_cell != fact_cell ||
                    lccf_fact_unpack_generation(fact_word) !=
                        instance->frame->generation ||
                    lccf_fact_unpack_state(fact_word) !=
                        LCCF_FACT_STATE_ARMED ||
                    atomic_load_explicit(
                        &fact_cell->references[LCCF_FACT_REF_BACKEND],
                        memory_order_acquire) != refs) {
                    return EPROTO;
                }
            } else if (instance->fact_cell != NULL) {
                return EPROTO;
            }
            for (ticket_index = 0U;
                 ticket_index < refs;
                 ++ticket_index) {
                const lccf_model_ticket_t *ticket =
                    lccf_model_ticket_at(
                        batch, i, ticket_index);

                if (ticket == NULL || ticket->target != cell ||
                    ticket->generation !=
                        instance->frame->generation ||
                    ticket->instance_index != i ||
                    ticket->ticket_index != ticket_index) {
                    return EPROTO;
                }
            }
        } else {
            return ENOTSUP;
        }
    }
    return 0;
}

static bool command_valid(const lccf_model_batch_t *batch,
                          const lccf_model_instance_t *instance) {
    const lccf_model_command_t *command = &instance->command;

    return command->output == instance->frame->output &&
           command->next_site < batch->config.site_count &&
           command->kind >= LCCF_MODEL_COMMAND_CONTINUE &&
           command->kind <= LCCF_MODEL_COMMAND_FAIL;
}

static unsigned fairness_bucket(uint64_t latency) {
    unsigned bucket = 0U;
    uint64_t upper = UINT64_C(1);

    while (upper < latency && bucket < 63U) {
        upper <<= 1U;
        bucket += 1U;
    }
    return bucket;
}

static void update_fairness_p99(lccf_model_batch_t *batch,
                                lccf_model_metrics_t *metrics) {
    const uint64_t rank =
        batch->fairness_services -
        batch->fairness_services / UINT64_C(100);
    uint64_t cumulative = 0U;
    unsigned bucket;

    for (bucket = 0U; bucket < 64U; ++bucket) {
        cumulative += batch->fairness_histogram[bucket];
        if (cumulative >= rank) {
            metrics->fairness_p99_ns =
                bucket == 63U ?
                    (UINT64_C(1) << 63U) :
                    (UINT64_C(1) << bucket);
            return;
        }
    }
}

static int fairness_note_callback(lccf_model_batch_t *batch) {
    batch->fairness_tick += UINT64_C(1);
    if (batch->config.workload ==
            LCCF_MODEL_COMPLETION_MIXED_FAIRNESS &&
        !batch->fairness_due &&
        (batch->fairness_tick & UINT64_C(31)) == 0U) {
        const uint64_t now =
            lccf_platform_monotonic_ns();

        if (now == 0U) {
            return EIO;
        }
        batch->fairness_due = true;
        batch->fairness_due_ns = now;
    }
    return 0;
}

static int fairness_service(lccf_model_batch_t *batch,
                            lccf_model_metrics_t *metrics) {
    uint64_t now;
    uint64_t latency;
    unsigned bucket;

    if (!batch->fairness_due) {
        return 0;
    }
    now = lccf_platform_monotonic_ns();
    if (now == 0U || now < batch->fairness_due_ns) {
        return EIO;
    }
    latency = now - batch->fairness_due_ns;
    if (latency == 0U) {
        latency = UINT64_C(1);
    }
    bucket = fairness_bucket(latency);
    batch->fairness_histogram[bucket] += UINT64_C(1);
    batch->fairness_services += UINT64_C(1);
    metrics->fairness_samples += UINT64_C(1);
    batch->fairness_due = false;
    update_fairness_p99(batch, metrics);
    return 0;
}

static int execute_resolved_resume_callback(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    const lccf_model_event_t *event,
    lccf_model_resume_fn resume,
    unsigned site,
    bool direct,
    lccf_model_metrics_t *metrics) {
    if (site >= batch->config.site_count ||
        batch->callback_depth == UINT_MAX || resume == NULL ||
        resume != batch->ops->resume_sites[site]) {
        return EPROTO;
    }
    memset(&instance->command, 0, sizeof(instance->command));
    if (batch->config.callback_failure_step != 0U &&
        instance->callback_sequence_count + UINT64_C(1) ==
            batch->config.callback_failure_step) {
        return EIO;
    }
    if (fairness_note_callback(batch) != 0) {
        return EIO;
    }
    batch->callback_depth += 1U;
    if (batch->callback_depth > batch->maximum_callback_depth) {
        batch->maximum_callback_depth = batch->callback_depth;
    }
    resume(instance->frame,
           event,
           &instance->command,
           &batch->config,
           site);
    batch->callback_depth -= 1U;
    instance->event_sequence_hash = lccf_model_mix64(
        instance->event_sequence_hash ^ event->word0 ^
        lccf_model_mix64(event->word1) ^
        ((uint64_t)event->kind << 32U) ^
        (uint64_t)(uint32_t)event->error_code ^ (uint64_t)site);
    instance->command_sequence_hash = lccf_model_mix64(
        instance->command_sequence_hash ^ instance->command.output ^
        ((uint64_t)instance->command.next_site << 32U) ^
        (uint64_t)instance->command.kind);
    instance->callback_sequence_count += UINT64_C(1);
    if (batch->trace_rows != NULL) {
        lccf_model_trace_row_t *row;

        if (batch->trace_count >= batch->trace_capacity) {
            return ENOSPC;
        }
        row = &batch->trace_rows[batch->trace_count++];
        row->generation = instance->frame->generation;
        row->callback_ordinal = instance->callback_sequence_count;
        row->event_word0 = event->word0;
        row->event_word1 = event->word1;
        row->command_output = instance->command.output;
        row->instance_index = instance->index;
        row->site_index = site;
        row->event_kind = event->kind;
        row->error_code = event->error_code;
        row->command_next_site = instance->command.next_site;
        row->command_kind = instance->command.kind;
    }
    metrics->resume_calls += UINT64_C(1);
    if (direct) {
        metrics->direct_calls += UINT64_C(1);
    }
    return command_valid(batch, instance) ? 0 : EPROTO;
}

static int execute_resume_callback(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    const lccf_model_event_t *event,
    unsigned site,
    bool direct,
    lccf_model_metrics_t *metrics) {
    if (site >= batch->config.site_count) {
        return EPROTO;
    }
    return execute_resolved_resume_callback(
        batch, instance, event, batch->ops->resume_sites[site], site,
        direct, metrics);
}

static int claim_and_publish_wakers(lccf_model_batch_t *batch,
                                    lccf_model_metrics_t *metrics) {
    size_t i;

    for (i = 0U; i < batch->config.instance_count; ++i) {
        lccf_model_instance_t *instance = &batch->instances[i];
        lccf_model_waker_t *waker = instance->waker;
        const uint64_t generation = instance->frame->generation;
        uint64_t expected =
            lccf_model_pack_state(generation,
                                  LCCF_MODEL_STATE_ARMED);
        const uint64_t claimed =
            lccf_model_pack_state(generation,
                                  LCCF_MODEL_STATE_CLAIMED);

        lccf_model_derive_event(batch, instance, &instance->event);
        if (!atomic_compare_exchange_strong_explicit(
                &waker->state_generation,
                &expected,
                claimed,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return EPROTO;
        }
        atomic_store_explicit(
            &waker->state_generation,
            lccf_model_pack_state(generation,
                                  LCCF_MODEL_STATE_QUEUED),
            memory_order_release);
        if (queue_push(&batch->local_queue, waker) != 0) {
            return EOVERFLOW;
        }
        metrics->completions += UINT64_C(1);
        metrics->claims += UINT64_C(1);
        metrics->queue_pushes += UINT64_C(1);
    }
    return 0;
}

static int resume_one_waker(lccf_model_batch_t *batch,
                            lccf_model_waker_t *waker,
                            lccf_model_metrics_t *metrics) {
    lccf_model_instance_t *instance;
    lccf_model_frame_core_t *frame;
    uint64_t word;
    uint64_t generation;
    unsigned site;

    if (waker == NULL || waker->instance == NULL ||
        waker->instance->batch != batch ||
        waker->instance->waker != waker) {
        return EPROTO;
    }
    instance = waker->instance;
    frame = instance->frame;
    word = atomic_load_explicit(
        &waker->state_generation, memory_order_acquire);
    generation = lccf_model_unpack_generation(word);
    site = waker->resume_site;
    if (lccf_model_unpack_state(word) !=
            LCCF_MODEL_STATE_QUEUED ||
        generation != frame->generation ||
        site >= batch->config.site_count) {
        return EPROTO;
    }
    atomic_store_explicit(
        &waker->state_generation,
        lccf_model_pack_state(generation,
                              LCCF_MODEL_STATE_RUNNING),
        memory_order_release);
    if (execute_resume_callback(batch,
                                instance,
                                &instance->event,
                                site,
                                false,
                                metrics) != 0) {
        return EPROTO;
    }
    metrics->queue_pops += UINT64_C(1);
    if (fairness_service(batch, metrics) != 0) {
        return EIO;
    }
    frame->command_word =
        instance->command.output ^
        ((uint64_t)instance->command.next_site << 32U) ^
        (uint64_t)instance->command.kind;
    frame->site = instance->command.next_site;
    waker->resume_site = frame->site;

    if (instance->command.kind ==
        LCCF_MODEL_COMMAND_CONTINUE) {
        atomic_store_explicit(
            &waker->state_generation,
            lccf_model_pack_state(generation,
                                  LCCF_MODEL_STATE_QUEUED),
            memory_order_release);
        if (queue_push(&batch->local_queue, waker) != 0) {
            return EOVERFLOW;
        }
        metrics->queue_pushes += UINT64_C(1);
    } else {
        frame->generation += UINT64_C(1);
        atomic_store_explicit(
            &waker->state_generation,
            lccf_model_pack_state(frame->generation,
                                  LCCF_MODEL_STATE_ARMED),
            memory_order_release);
    }
    return 0;
}

static int resume_queued_wakers(lccf_model_batch_t *batch,
                                lccf_model_metrics_t *metrics) {
    lccf_model_waker_t *waker;

    while ((waker = queue_pop(&batch->local_queue)) != NULL) {
        const int rc = resume_one_waker(
            batch, waker, metrics);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static void publish_cell_command(lccf_model_instance_t *instance) {
    lccf_model_frame_core_t *frame = instance->frame;
    lccf_model_cell_hot_t *cell = instance->cell;

    frame->command_word =
        instance->command.output ^
        ((uint64_t)instance->command.next_site << 32U) ^
        (uint64_t)instance->command.kind;
    cell->command_word = frame->command_word;
    frame->site = instance->command.next_site;
    cell->next_site = frame->site;
}

static int rearm_cell_generation(lccf_model_batch_t *batch,
                                 lccf_model_instance_t *instance) {
    if (atomic_load_explicit(&instance->cell->backend_refs,
                             memory_order_acquire) != 0U) {
        return EBUSY;
    }
    instance->frame->generation += UINT64_C(1);
    return prepare_causal_generation(batch, instance, true);
}

static int mark_cell_running(lccf_model_instance_t *instance,
                             lccf_model_state_t expected_state) {
    lccf_model_cell_hot_t *cell = instance->cell;
    const uint64_t word = atomic_load_explicit(
        &cell->state_generation, memory_order_acquire);

    if (lccf_model_unpack_generation(word) !=
            instance->frame->generation ||
        lccf_model_unpack_state(word) != expected_state) {
        return EPROTO;
    }
    atomic_store_explicit(
        &cell->state_generation,
        lccf_model_pack_state(instance->frame->generation,
                              LCCF_MODEL_STATE_RUNNING),
        memory_order_release);
    return 0;
}

static unsigned causal_winner_ticket(
    const lccf_model_batch_t *batch,
    const lccf_model_instance_t *instance) {
    lccf_model_event_t event;

    lccf_model_derive_event(batch, instance, &event);
    switch (event.kind) {
        case LCCF_MODEL_EVENT_TIMEOUT:
            return 1U;
        case LCCF_MODEL_EVENT_CANCEL:
            return 2U;
        case LCCF_MODEL_EVENT_IO:
        default:
            return 0U;
    }
}

static int retire_backend_reference(lccf_model_cell_hot_t *cell) {
    uint32_t current = atomic_load_explicit(
        &cell->backend_refs, memory_order_acquire);

    while (current != 0U) {
        if (atomic_compare_exchange_weak_explicit(
                &cell->backend_refs,
                &current,
                current - 1U,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return 0;
        }
    }
    return EPROTO;
}

static int64_t event_result_word(uint64_t word) {
    return (int64_t)(word & (uint64_t)INT64_MAX);
}

static lccf_fact_event_kind_t fact_event_kind(uint32_t event_kind) {
    switch ((lccf_model_event_kind_t)event_kind) {
    case LCCF_MODEL_EVENT_IO:
        return LCCF_FACT_EVENT_IO;
    case LCCF_MODEL_EVENT_TIMEOUT:
    case LCCF_MODEL_EVENT_TIMER:
        return LCCF_FACT_EVENT_TIMER;
    case LCCF_MODEL_EVENT_CANCEL:
        return LCCF_FACT_EVENT_CANCEL;
    case LCCF_MODEL_EVENT_INTERNAL:
    default:
        return LCCF_FACT_EVENT_EXTERNAL;
    }
}

static lccf_fact_source_t fact_source_kind(
    const lccf_model_instance_t *instance,
    const lccf_model_event_t *event) {
    if (event->kind == LCCF_MODEL_EVENT_TIMEOUT ||
        event->kind == LCCF_MODEL_EVENT_TIMER) {
        return LCCF_FACT_SOURCE_TIMER;
    }
    if (event->kind == LCCF_MODEL_EVENT_CANCEL) {
        return LCCF_FACT_SOURCE_CANCEL;
    }
    if (event->kind == LCCF_MODEL_EVENT_INTERNAL) {
        return LCCF_FACT_SOURCE_EXTERNAL;
    }
    switch (instance->index % 3U) {
    case 0U:
        return LCCF_FACT_SOURCE_LINUX_CQE;
    case 1U:
        return LCCF_FACT_SOURCE_KQUEUE;
    default:
        return LCCF_FACT_SOURCE_IOCP;
    }
}

static int make_fact_ticket(
    const lccf_model_batch_t *batch,
    const lccf_model_instance_t *instance,
    const lccf_model_ticket_t *ticket,
    lccf_fact_ticket_t *out_ticket) {
    int rc;

    rc = lccf_fact_ticket_from_logical(
        fact_source_kind(instance, &ticket->event),
        fact_event_kind(ticket->event.kind), ticket->generation,
        event_result_word(ticket->event.word0), ticket->event.error_code,
        ticket->event.word1, instance->frame->site,
        batch->config.site_count, instance->cell->home_shard,
        ticket->ticket_index, ticket->ticket_index,
        batch->fact_site_table, out_ticket);
    if (rc == 0) {
        out_ticket->stable_flags =
            (uint64_t)ticket->event.kind |
            ((ticket->event.word0 >> 63U) << 32U);
    }
    return rc;
}

static int fact_to_model_event(const lccf_fact_core_t *fact,
                               lccf_model_event_t *event) {
    if (fact == NULL || event == NULL ||
        fact->event_kind == LCCF_FACT_EVENT_FAIL ||
        fact->result < 0) {
        return EPROTO;
    }
    event->word0 = (uint64_t)fact->result |
                   (((fact->stable_flags >> 32U) & UINT64_C(1)) << 63U);
    event->word1 = fact->payload_word;
    event->kind = (uint32_t)fact->stable_flags;
    event->error_code = fact->error_code;
    return event->kind >= LCCF_MODEL_EVENT_IO &&
                   event->kind <= LCCF_MODEL_EVENT_INTERNAL
               ? 0
               : EPROTO;
}

typedef struct lccf_model_fact_invoke_context {
    lccf_model_batch_t *batch;
    lccf_model_instance_t *instance;
    lccf_model_metrics_t *metrics;
    bool direct;
} lccf_model_fact_invoke_context_t;

static int invoke_resolved_fact_site(
    const lccf_fact_site_descriptor_t *descriptor,
    const lccf_fact_core_t *fact,
    void *context) {
    const lccf_model_fact_site_t *site =
        (const lccf_model_fact_site_t *)(const void *)descriptor;
    lccf_model_fact_invoke_context_t *invoke_context = context;
    lccf_model_event_t event;
    int rc;

    if (descriptor == NULL || fact == NULL || invoke_context == NULL ||
        invoke_context->batch == NULL || invoke_context->instance == NULL ||
        invoke_context->metrics == NULL || site->resume == NULL) {
        return EINVAL;
    }
    rc = fact_to_model_event(fact, &event);
    if (rc != 0 ||
        memcmp(&invoke_context->instance->cell->event, &event,
               sizeof(event)) != 0) {
        return rc != 0 ? rc : EPROTO;
    }
    return execute_resolved_resume_callback(
        invoke_context->batch, invoke_context->instance, &event,
        site->resume, invoke_context->instance->cell->next_site,
        invoke_context->direct, invoke_context->metrics);
}

static void accumulate_fact_counters(
    lccf_model_metrics_t *metrics,
    const lccf_fact_counters_t *counters) {
    metrics->facts_attempted += counters->claim_attempts;
    metrics->facts_built += counters->fact_builds;
    metrics->facts_build_failed += counters->fact_build_failures;
    metrics->fact_normalizations += counters->normalization_calls;
    metrics->fact_site_lookups += counters->site_lookups;
    metrics->fact_module_pins += counters->module_pins;
    metrics->fact_payload_pins += counters->payload_pins;
    metrics->fact_stale_losers += counters->stale_losers;
    metrics->fact_guard_rechecks += counters->guard_rechecks;
    metrics->fact_queue_forwards += counters->queue_forwards;
    metrics->fact_generation_mismatches +=
        counters->generation_mismatches;
    metrics->fact_reuse_delays += counters->reuse_delays;
}

static int claim_fact_tickets(lccf_model_batch_t *batch,
                              lccf_model_instance_t *instance,
                              lccf_model_metrics_t *metrics) {
    const unsigned count = active_ticket_count(batch);
    const unsigned first = causal_winner_ticket(batch, instance);
    unsigned offset;
    bool won = false;

    for (offset = 0U; offset < count; ++offset) {
        const unsigned ticket_index =
            count == 1U ? 0U : (first + offset) % count;
        const lccf_model_ticket_t *ticket = lccf_model_ticket_at(
            batch, instance->index, ticket_index);
        lccf_fact_ticket_t fact_ticket;
        lccf_fact_counters_t counters = {0};
        bool ticket_won = false;
        int rc;

        if (ticket == NULL || ticket->target != instance->cell ||
            make_fact_ticket(batch, instance, ticket, &fact_ticket) != 0) {
            return EPROTO;
        }
        rc = lccf_fact_try_publish(
            instance->fact_cell, &fact_ticket,
            mode_shares_facts(batch->config.mode), &counters,
            &ticket_won);
        accumulate_fact_counters(metrics, &counters);
        if (rc != 0) {
            return rc;
        }
        if (ticket_won) {
            if (won) {
                return EPROTO;
            }
            instance->cell->event = ticket->event;
            instance->cell->next_site = instance->frame->site;
            atomic_store_explicit(
                &instance->cell->state_generation,
                lccf_model_pack_state(instance->frame->generation,
                                      LCCF_MODEL_STATE_CLAIMED),
                memory_order_release);
            metrics->completions += UINT64_C(1);
            metrics->claims += UINT64_C(1);
            won = true;
        } else {
            metrics->stale_tickets += UINT64_C(1);
        }
    }
    if (!won ||
        atomic_load_explicit(
            &instance->fact_cell->references[LCCF_FACT_REF_BACKEND],
            memory_order_acquire) != 0U) {
        return EPROTO;
    }
    return 0;
}

int lccf_model_try_claim_ticket(
    const lccf_model_ticket_t *ticket,
    bool *out_won) {
    lccf_model_cell_hot_t *cell;
    uint64_t expected;
    bool won;
    int rc;

    if (out_won == NULL) {
        return EINVAL;
    }
    *out_won = false;
    if (ticket == NULL || ticket->target == NULL ||
        ticket->generation == 0U) {
        return EINVAL;
    }
    cell = ticket->target;
    expected = lccf_model_pack_state(
        ticket->generation, LCCF_MODEL_STATE_ARMED);
    won = atomic_compare_exchange_strong_explicit(
        &cell->state_generation,
        &expected,
        lccf_model_pack_state(ticket->generation,
                              LCCF_MODEL_STATE_CLAIMED),
        memory_order_acq_rel,
        memory_order_acquire);
    if (won) {
        cell->event = ticket->event;
    } else if (lccf_model_unpack_generation(expected) !=
               ticket->generation) {
        return 0;
    }
    rc = retire_backend_reference(cell);
    if (rc != 0) {
        return rc;
    }
    *out_won = won;
    return 0;
}

static int claim_cell_tickets(lccf_model_batch_t *batch,
                              lccf_model_instance_t *instance,
                              lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell = instance->cell;
    const unsigned count = active_ticket_count(batch);
    const unsigned first =
        causal_winner_ticket(batch, instance);
    unsigned offset;
    bool won = false;

    for (offset = 0U; offset < count; ++offset) {
        const unsigned ticket_index =
            count == 1U ? 0U : (first + offset) % count;
        const lccf_model_ticket_t *ticket =
            lccf_model_ticket_at(
                batch, instance->index, ticket_index);
        bool ticket_won;
        int rc;

        if (ticket == NULL || ticket->target != cell) {
            return EPROTO;
        }
        rc = lccf_model_try_claim_ticket(
            ticket, &ticket_won);
        if (rc != 0) {
            return rc;
        }
        if (ticket_won) {
            if (won) {
                return EPROTO;
            }
            cell->next_site = instance->frame->site;
            metrics->completions += UINT64_C(1);
            metrics->claims += UINT64_C(1);
            won = true;
        } else {
            metrics->stale_tickets += UINT64_C(1);
        }
    }
    if (!won ||
        atomic_load_explicit(&cell->backend_refs,
                             memory_order_acquire) != 0U) {
        return EPROTO;
    }
    return 0;
}

static int enqueue_claimed_cell(lccf_model_batch_t *batch,
                                lccf_model_cell_hot_t *cell,
                                lccf_model_metrics_t *metrics,
                                bool forced_escape) {
    const uint64_t word = atomic_load_explicit(
        &cell->state_generation, memory_order_acquire);
    const lccf_model_state_t state =
        lccf_model_unpack_state(word);

    if ((state != LCCF_MODEL_STATE_CLAIMED &&
         state != LCCF_MODEL_STATE_RUNNING) ||
        atomic_exchange_explicit(
            &cell->queue_owned,
            1U,
            memory_order_acq_rel) != 0U) {
        return EPROTO;
    }
    atomic_store_explicit(
        &cell->state_generation,
        lccf_model_pack_state(
            lccf_model_unpack_generation(word),
            LCCF_MODEL_STATE_QUEUED),
        memory_order_release);
    if (queue_push(&batch->local_queue, cell) != 0) {
        const int overflow_rc = overflow_push(batch, cell);

        if (overflow_rc != 0) {
            return overflow_rc;
        }
        if (mode_uses_facts(batch->config.mode)) {
            metrics->fact_overflow_pushes += UINT64_C(1);
        }
    }
    metrics->queue_pushes += UINT64_C(1);
    if (forced_escape) {
        metrics->forced_escapes += UINT64_C(1);
    }
    return 0;
}

static int claim_and_publish_cells(lccf_model_batch_t *batch,
                                   lccf_model_metrics_t *metrics) {
    size_t i;

    for (i = 0U; i < batch->config.instance_count; ++i) {
        lccf_model_instance_t *instance = &batch->instances[i];
        int rc = claim_cell_tickets(batch, instance, metrics);

        if (rc == 0) {
            rc = enqueue_claimed_cell(
                batch, instance->cell, metrics, false);
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int resume_one_cell(lccf_model_batch_t *batch,
                           lccf_model_cell_hot_t *cell,
                           lccf_model_metrics_t *metrics) {
    lccf_model_instance_t *instance;
    uint64_t word;
    unsigned site;

    if (cell == NULL || cell->instance == NULL ||
        cell->instance->batch != batch ||
        cell->instance->cell != cell) {
        return EPROTO;
    }
    instance = cell->instance;
    word = atomic_load_explicit(
        &cell->state_generation, memory_order_acquire);
    site = cell->next_site;
    if (lccf_model_unpack_state(word) !=
            LCCF_MODEL_STATE_QUEUED ||
        lccf_model_unpack_generation(word) !=
            instance->frame->generation ||
        site >= batch->config.site_count ||
        atomic_exchange_explicit(
            &cell->queue_owned,
            0U,
            memory_order_acq_rel) != 1U) {
        return EPROTO;
    }
    if (mark_cell_running(
            instance, LCCF_MODEL_STATE_QUEUED) != 0 ||
        execute_resume_callback(batch,
                                instance,
                                &cell->event,
                                site,
                                false,
                                metrics) != 0) {
        return EPROTO;
    }
    metrics->queue_pops += UINT64_C(1);
    if (fairness_service(batch, metrics) != 0) {
        return EIO;
    }
    publish_cell_command(instance);

    if (instance->command.kind ==
        LCCF_MODEL_COMMAND_CONTINUE) {
        return enqueue_claimed_cell(
            batch, cell, metrics, false);
    }
    return rearm_cell_generation(batch, instance);
}

static int resume_queued_cells(lccf_model_batch_t *batch,
                               lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell;

    while ((cell = dequeue_cell(batch, metrics)) != NULL) {
        const int rc = resume_one_cell(batch, cell, metrics);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int run_direct_cell_segment(
    lccf_model_batch_t *batch,
    lccf_model_cell_hot_t *cell,
    lccf_model_metrics_t *metrics,
    bool budgeted) {
    lccf_model_instance_t *instance;
    unsigned direct_count = 0U;
    lccf_model_state_t expected_state;

    if (cell == NULL || cell->instance == NULL ||
        cell->instance->batch != batch ||
        cell->instance->cell != cell) {
        return EPROTO;
    }
    instance = cell->instance;
    expected_state = lccf_model_unpack_state(
        atomic_load_explicit(&cell->state_generation,
                             memory_order_acquire));
    if (expected_state != LCCF_MODEL_STATE_CLAIMED &&
        expected_state != LCCF_MODEL_STATE_RUNNING) {
        return EPROTO;
    }

    for (;;) {
        int rc;

        rc = mark_cell_running(instance, expected_state);
        if (rc != 0) {
            return rc;
        }
        rc = execute_resume_callback(batch,
                                     instance,
                                     &cell->event,
                                     cell->next_site,
                                     true,
                                     metrics);
        if (rc != 0) {
            return rc;
        }
        direct_count += 1U;
        publish_cell_command(instance);

        if (instance->command.kind !=
            LCCF_MODEL_COMMAND_CONTINUE) {
            rc = fairness_service(batch, metrics);
            if (rc != 0) {
                return rc;
            }
            return rearm_cell_generation(batch, instance);
        }
        if (!budgeted && batch->fairness_due) {
            rc = fairness_service(batch, metrics);
            if (rc != 0) {
                return rc;
            }
        }
        if (budgeted &&
            (direct_count >= batch->config.direct_budget ||
             batch->fairness_due)) {
            rc = enqueue_claimed_cell(
                batch, cell, metrics, true);
            if (rc == 0) {
                rc = fairness_service(batch, metrics);
            }
            return rc;
        }
        expected_state = LCCF_MODEL_STATE_RUNNING;
    }
}

int lccf_model_resume_claimed_cell(
    lccf_model_batch_t *batch,
    size_t instance_index,
    lccf_model_metrics_t *metrics) {
    if (batch == NULL || metrics == NULL ||
        !mode_uses_cells(batch->config.mode) ||
        instance_index >= batch->config.instance_count ||
        batch->local_queue.head != batch->local_queue.tail) {
        return EINVAL;
    }
    return run_direct_cell_segment(
        batch,
        batch->instances[instance_index].cell,
        metrics,
        false);
}

static int resume_budgeted_escape_cells(
    lccf_model_batch_t *batch,
    lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell;

    while ((cell = dequeue_cell(batch, metrics)) != NULL) {
        lccf_model_instance_t *instance = cell->instance;
        uint64_t word;
        int rc;

        if (instance == NULL || instance->batch != batch ||
            instance->cell != cell) {
            return EPROTO;
        }
        word = atomic_load_explicit(
            &cell->state_generation, memory_order_acquire);
        if (lccf_model_unpack_state(word) !=
                LCCF_MODEL_STATE_QUEUED ||
            lccf_model_unpack_generation(word) !=
                instance->frame->generation ||
            cell->next_site >= batch->config.site_count ||
            atomic_exchange_explicit(
                &cell->queue_owned,
                0U,
                memory_order_acq_rel) != 1U) {
            return EPROTO;
        }
        rc = mark_cell_running(
            instance, LCCF_MODEL_STATE_QUEUED);
        if (rc == 0) {
            rc = execute_resume_callback(batch,
                                         instance,
                                         &cell->event,
                                         cell->next_site,
                                         false,
                                         metrics);
        }
        if (rc != 0) {
            return rc;
        }
        metrics->queue_pops += UINT64_C(1);
        rc = fairness_service(batch, metrics);
        if (rc != 0) {
            return rc;
        }
        publish_cell_command(instance);

        if (instance->command.kind ==
            LCCF_MODEL_COMMAND_CONTINUE) {
            rc = run_direct_cell_segment(
                batch, cell, metrics, true);
        } else {
            rc = rearm_cell_generation(batch, instance);
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int run_fused_cells(lccf_model_batch_t *batch,
                           lccf_model_metrics_t *metrics,
                           bool budgeted) {
    size_t i;

    for (i = 0U; i < batch->config.instance_count; ++i) {
        lccf_model_instance_t *instance = &batch->instances[i];
        int rc = claim_cell_tickets(batch, instance, metrics);

        if (rc == 0) {
            rc = run_direct_cell_segment(
                batch, instance->cell, metrics, budgeted);
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (budgeted) {
        return resume_budgeted_escape_cells(batch, metrics);
    }
    return 0;
}

static lccf_fact_guard_t make_consume_guard(
    const lccf_model_instance_t *instance,
    bool direct_enabled) {
    lccf_fact_guard_t guard;

    memset(&guard, 0, sizeof(guard));
    guard.flags = LCCF_FACT_GUARD_MODULE_ENABLED |
                  LCCF_FACT_GUARD_BACKEND_CAPABLE;
    if (direct_enabled) {
        guard.flags |= LCCF_FACT_GUARD_DIRECT_ENABLED;
    }
    guard.budget_remaining = instance->batch->config.direct_budget;
    guard.current_home_shard = instance->cell->home_shard;
    guard.consuming_shard = instance->cell->home_shard;
    return guard;
}

static bool fact_event_matches(
    const lccf_model_instance_t *instance,
    const lccf_model_event_t *event) {
    return memcmp(&instance->cell->event, event,
                  sizeof(*event)) == 0;
}

static int abort_fact_generation(lccf_model_instance_t *instance) {
    const uint64_t generation = instance->frame->generation;
    const int rc = lccf_fact_abort(instance->fact_cell, generation);

    atomic_store_explicit(
        &instance->cell->queue_owned, 0U, memory_order_release);
    atomic_store_explicit(
        &instance->cell->state_generation,
        lccf_model_pack_state(generation, LCCF_MODEL_STATE_TERMINAL),
        memory_order_release);
    return rc;
}

static int validate_materialized_fact(
    const lccf_model_instance_t *instance,
    const lccf_fact_core_t *fact) {
    lccf_model_event_t event;
    const int rc = fact_to_model_event(fact, &event);

    return rc != 0 ? rc :
        (fact_event_matches(instance, &event) ? 0 : EPROTO);
}

static int invoke_materialized_fact(
    lccf_model_instance_t *instance,
    const lccf_fact_core_t *fact,
    bool direct,
    lccf_model_metrics_t *metrics) {
    lccf_model_fact_invoke_context_t context;
    int rc;

    context.batch = instance->batch;
    context.instance = instance;
    context.metrics = metrics;
    context.direct = direct;
    rc = lccf_fact_invoke(fact, &context);
    if (rc != 0) {
        const int abort_rc = abort_fact_generation(instance);

        return abort_rc == 0 ? rc : abort_rc;
    }
    return 0;
}

static int consume_fact_transaction(
    lccf_model_instance_t *instance,
    lccf_fact_consumer_t consumer,
    const lccf_fact_guard_t *guard,
    lccf_model_metrics_t *metrics,
    lccf_fact_decision_t *decision) {
    lccf_fact_counters_t counters = {0};
    lccf_fact_core_t fact;
    bool execute;
    int rc;

    rc = lccf_fact_consume(
        instance->fact_cell, instance->frame->generation,
        instance->cell->next_site, consumer, guard, &counters,
        decision, &fact);
    accumulate_fact_counters(metrics, &counters);
    if (rc != 0) {
        return rc;
    }
    rc = validate_materialized_fact(instance, &fact);
    if (rc != 0) {
        (void)abort_fact_generation(instance);
        return rc;
    }
    execute =
        (consumer == LCCF_FACT_CONSUMER_DIRECT &&
         decision->route == LCCF_FACT_ROUTE_DIRECT) ||
        (consumer == LCCF_FACT_CONSUMER_QUEUE &&
         decision->route == LCCF_FACT_ROUTE_QUEUE);
    if (!execute) {
        return 0;
    }
    rc = mark_cell_running(
        instance,
        consumer == LCCF_FACT_CONSUMER_DIRECT
            ? LCCF_MODEL_STATE_CLAIMED
            : LCCF_MODEL_STATE_QUEUED);
    if (rc != 0) {
        (void)abort_fact_generation(instance);
        return rc;
    }
    return invoke_materialized_fact(
        instance, &fact, consumer == LCCF_FACT_CONSUMER_DIRECT,
        metrics);
}

static int materialize_and_invoke_fact(
    lccf_model_instance_t *instance,
    lccf_model_metrics_t *metrics) {
    lccf_fact_counters_t counters = {0};
    lccf_fact_core_t fact;
    int rc;

    counters.guard_rechecks = 1U;
    rc = lccf_fact_materialize(
        instance->fact_cell, instance->frame->generation,
        instance->cell->next_site, &counters, &fact);
    accumulate_fact_counters(metrics, &counters);
    if (rc != 0) {
        return rc;
    }
    rc = validate_materialized_fact(instance, &fact);
    if (rc != 0) {
        (void)abort_fact_generation(instance);
        return rc;
    }
    return invoke_materialized_fact(instance, &fact, true, metrics);
}

static int finish_fact_generation(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    lccf_model_metrics_t *metrics) {
    lccf_fact_counters_t counters = {0};
    const uint64_t generation = instance->frame->generation;
    const uint64_t next_generation = generation + UINT64_C(1);
    int rc;

    rc = lccf_fact_finish(instance->fact_cell, generation, &counters);
    accumulate_fact_counters(metrics, &counters);
    if (rc != 0) {
        return rc;
    }
    instance->frame->generation = next_generation;
    rc = prepare_causal_generation(batch, instance, true);
    if (rc == 0) {
        rc = lccf_fact_cell_arm(instance->fact_cell, next_generation,
                                active_ticket_count(batch));
    }
    return rc;
}

static int publish_ready_fact_to_queue(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    lccf_model_metrics_t *metrics,
    bool forced_escape) {
    const lccf_fact_guard_t guard = make_consume_guard(instance, false);
    lccf_fact_decision_t decision;
    int rc;

    rc = consume_fact_transaction(
        instance, LCCF_FACT_CONSUMER_DIRECT, &guard, metrics, &decision);
    if (rc != 0 || decision.route != LCCF_FACT_ROUTE_QUEUE) {
        return rc != 0 ? rc : EPROTO;
    }
    rc = enqueue_claimed_cell(batch, instance->cell, metrics,
                              forced_escape);
    if (rc != 0) {
        (void)abort_fact_generation(instance);
    }
    return rc;
}

static int yield_fact_to_queue(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    lccf_model_metrics_t *metrics) {
    int rc = lccf_fact_yield_to_queue(
        instance->fact_cell, instance->frame->generation);

    if (rc == 0) {
        rc = enqueue_claimed_cell(batch, instance->cell, metrics, false);
    }
    if (rc != 0) {
        (void)abort_fact_generation(instance);
    }
    return rc;
}

static int resume_one_fact_cell(
    lccf_model_batch_t *batch,
    lccf_model_cell_hot_t *cell,
    lccf_model_metrics_t *metrics) {
    lccf_model_instance_t *instance;
    lccf_fact_guard_t guard;
    lccf_fact_decision_t decision;
    uint64_t word;
    int rc;

    if (cell == NULL || cell->instance == NULL ||
        cell->instance->batch != batch ||
        cell->instance->cell != cell ||
        cell->instance->fact_cell == NULL) {
        return EPROTO;
    }
    instance = cell->instance;
    word = atomic_load_explicit(&cell->state_generation,
                                memory_order_acquire);
    if (lccf_model_unpack_state(word) != LCCF_MODEL_STATE_QUEUED ||
        lccf_model_unpack_generation(word) !=
            instance->frame->generation ||
        atomic_exchange_explicit(&cell->queue_owned, 0U,
                                 memory_order_acq_rel) != 1U) {
        return EPROTO;
    }
    guard = make_consume_guard(instance, true);
    rc = consume_fact_transaction(
        instance, LCCF_FACT_CONSUMER_QUEUE, &guard, metrics, &decision);
    if (rc != 0 || decision.route != LCCF_FACT_ROUTE_QUEUE) {
        return rc != 0 ? rc : EPROTO;
    }
    metrics->queue_pops += UINT64_C(1);
    rc = fairness_service(batch, metrics);
    if (rc != 0) {
        (void)abort_fact_generation(instance);
        return rc;
    }
    publish_cell_command(instance);
    if (instance->command.kind == LCCF_MODEL_COMMAND_CONTINUE) {
        return yield_fact_to_queue(batch, instance, metrics);
    }
    return finish_fact_generation(batch, instance, metrics);
}

static int resume_queued_fact_cells(
    lccf_model_batch_t *batch,
    lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell;

    while ((cell = dequeue_cell(batch, metrics)) != NULL) {
        const int rc = resume_one_fact_cell(batch, cell, metrics);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int run_direct_fact_segment(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    lccf_model_metrics_t *metrics) {
    lccf_fact_guard_t guard = make_consume_guard(instance, true);
    lccf_fact_decision_t decision;
    bool first = true;

    for (;;) {
        int rc;

        if (first) {
            rc = consume_fact_transaction(
                instance, LCCF_FACT_CONSUMER_DIRECT, &guard,
                metrics, &decision);
            if (rc != 0 || decision.route != LCCF_FACT_ROUTE_DIRECT) {
                return rc != 0 ? rc : EPROTO;
            }
            first = false;
        } else {
            rc = materialize_and_invoke_fact(instance, metrics);
            if (rc != 0) {
                return rc;
            }
        }
        publish_cell_command(instance);
        if (instance->command.kind != LCCF_MODEL_COMMAND_CONTINUE) {
            rc = fairness_service(batch, metrics);
            if (rc != 0) {
                (void)abort_fact_generation(instance);
                return rc;
            }
            return finish_fact_generation(batch, instance, metrics);
        }
        if (batch->fairness_due) {
            rc = fairness_service(batch, metrics);
            if (rc != 0) {
                (void)abort_fact_generation(instance);
                return rc;
            }
        }
    }
}

static bool mixed_instance_queues(
    const lccf_model_batch_t *batch,
    const lccf_model_instance_t *instance) {
    return (((uint64_t)instance->index + batch->round) & UINT64_C(3)) == 0U;
}

static int run_fact_cells(lccf_model_batch_t *batch,
                          lccf_model_metrics_t *metrics) {
    size_t index;

    for (index = 0U; index < batch->config.instance_count; ++index) {
        lccf_model_instance_t *instance = &batch->instances[index];
        int rc = claim_fact_tickets(batch, instance, metrics);

        if (rc != 0) {
            return rc;
        }
        if (mode_is_fact_queue(batch->config.mode) ||
            (mode_is_fact_mixed(batch->config.mode) &&
             mixed_instance_queues(batch, instance))) {
            rc = publish_ready_fact_to_queue(
                batch, instance, metrics,
                mode_is_fact_mixed(batch->config.mode));
        } else {
            rc = run_direct_fact_segment(batch, instance, metrics);
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (mode_is_fact_queue(batch->config.mode) ||
        mode_is_fact_mixed(batch->config.mode)) {
        return resume_queued_fact_cells(batch, metrics);
    }
    return mode_is_fact_fused(batch->config.mode) ? 0 : EPROTO;
}

static int remote_queue_push(lccf_model_batch_t *batch, void *item) {
    lccf_model_remote_queue_t *queue = &batch->remote_queue;
    lccf_model_remote_slot_t *slot;
    size_t position;

    if (item == NULL || queue->slots == NULL ||
        queue->capacity == 0U) {
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

static int remote_queue_pop(lccf_model_remote_queue_t *queue,
                            void **out_item) {
    lccf_model_remote_slot_t *slot;
    const size_t position =
        queue == NULL ? 0U : queue->dequeue_position;
    void *item;

    if (queue == NULL || out_item == NULL ||
        queue->slots == NULL || queue->capacity == 0U) {
        return EINVAL;
    }
    *out_item = NULL;
    slot = &queue->slots[position & queue->mask];
    if (atomic_load_explicit(
            &slot->sequence, memory_order_acquire) !=
        position + 1U) {
        return EAGAIN;
    }
    item = slot->item;
    if (item == NULL) {
        return EPROTO;
    }
    slot->item = NULL;
    atomic_store_explicit(
        &slot->sequence,
        position + queue->capacity,
        memory_order_release);
    queue->dequeue_position = position + 1U;
    *out_item = item;
    return 0;
}

static int remote_publish_waker(
    lccf_model_remote_worker_t *worker,
    lccf_model_instance_t *instance) {
    lccf_model_batch_t *batch = worker->batch;
    lccf_model_waker_t *waker;
    uint64_t generation;
    uint64_t expected;

    if (instance == NULL || instance->batch != batch ||
        instance->waker == NULL || instance->cell != NULL) {
        return EPROTO;
    }
    waker = instance->waker;
    generation = instance->frame->generation;
    expected = lccf_model_pack_state(
        generation, LCCF_MODEL_STATE_ARMED);
    lccf_model_derive_event(batch, instance, &instance->event);
    if (!atomic_compare_exchange_strong_explicit(
            &waker->state_generation,
            &expected,
            lccf_model_pack_state(
                generation, LCCF_MODEL_STATE_CLAIMED),
            memory_order_acq_rel,
            memory_order_acquire)) {
        return EPROTO;
    }
    atomic_store_explicit(
        &waker->state_generation,
        lccf_model_pack_state(
            generation, LCCF_MODEL_STATE_QUEUED),
        memory_order_release);
    if (remote_queue_push(batch, waker) != 0) {
        return EPROTO;
    }
    worker->metrics.completions += UINT64_C(1);
    worker->metrics.claims += UINT64_C(1);
    worker->metrics.queue_pushes += UINT64_C(1);
    worker->metrics.remote_pushes += UINT64_C(1);
    return 0;
}

static int remote_publish_cell(
    lccf_model_remote_worker_t *worker,
    lccf_model_instance_t *instance) {
    lccf_model_batch_t *batch = worker->batch;
    lccf_model_cell_hot_t *cell;
    uint64_t word;
    int rc;

    if (instance == NULL || instance->batch != batch ||
        instance->cell == NULL || instance->waker != NULL) {
        return EPROTO;
    }
    cell = instance->cell;
    rc = claim_cell_tickets(
        batch, instance, &worker->metrics);
    if (rc != 0) {
        return rc;
    }
    word = atomic_load_explicit(
        &cell->state_generation, memory_order_acquire);
    if (lccf_model_unpack_state(word) !=
            LCCF_MODEL_STATE_CLAIMED ||
        atomic_exchange_explicit(
            &cell->queue_owned,
            1U,
            memory_order_acq_rel) != 0U) {
        return EPROTO;
    }
    atomic_store_explicit(
        &cell->state_generation,
        lccf_model_pack_state(
            lccf_model_unpack_generation(word),
            LCCF_MODEL_STATE_QUEUED),
        memory_order_release);
    rc = remote_queue_push(batch, cell);
    if (rc != 0) {
        return rc;
    }
    worker->metrics.queue_pushes += UINT64_C(1);
    worker->metrics.remote_pushes += UINT64_C(1);
    return 0;
}

static int remote_worker_main(void *opaque) {
    lccf_model_remote_worker_t *worker = opaque;
    lccf_model_batch_t *batch = worker->batch;
    lccf_model_remote_team_t *team = &batch->remote_team;
    uint64_t observed =
        lccf_platform_event_epoch(team->start_event);

    if (observed == UINT64_MAX) {
        return EPROTO;
    }
    atomic_fetch_add_explicit(
        &team->ready_workers, 1U, memory_order_acq_rel);
    if (lccf_platform_event_signal(team->ready_event) != 0) {
        return EPROTO;
    }
    for (;;) {
        uint64_t current;
        size_t index;
        int rc;

        rc = lccf_platform_event_wait(
            team->start_event, observed);
        if (rc != 0) {
            return rc;
        }
        current =
            lccf_platform_event_epoch(team->start_event);
        if (current == UINT64_MAX || current <= observed) {
            return EPROTO;
        }
        observed = current;
        if (atomic_load_explicit(
                &team->stop, memory_order_acquire)) {
            return 0;
        }

        worker->error = 0;
        for (index = worker->index;
             index < batch->config.instance_count;
             index += team->worker_count) {
            if (batch->config.mode ==
                LCCF_MODEL_REMOTE_WAKER_QUEUE) {
                rc = remote_publish_waker(
                    worker, &batch->instances[index]);
            } else if (batch->config.mode ==
                       LCCF_MODEL_REMOTE_CAUSAL_CELL) {
                rc = remote_publish_cell(
                    worker, &batch->instances[index]);
            } else {
                rc = ENOTSUP;
            }
            if (rc != 0) {
                worker->error = rc;
                break;
            }
        }
        if (atomic_fetch_add_explicit(
                &team->completed_workers,
                1U,
                memory_order_acq_rel) +
                1U ==
            team->worker_count) {
            rc = lccf_platform_event_signal(team->done_event);
            if (rc != 0) {
                return rc;
            }
        }
    }
}

static int wait_for_remote_workers(
    lccf_platform_event_t *event,
    const _Atomic unsigned *counter,
    unsigned target) {
    while (atomic_load_explicit(counter, memory_order_acquire) <
           target) {
        const uint64_t observed =
            lccf_platform_event_epoch(event);
        int rc;

        if (observed == UINT64_MAX) {
            return EPROTO;
        }
        if (atomic_load_explicit(
                counter, memory_order_acquire) >= target) {
            break;
        }
        rc = lccf_platform_event_wait(event, observed);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int remote_team_create(lccf_model_batch_t *batch) {
    lccf_model_remote_team_t *team = &batch->remote_team;
    unsigned index;
    int rc;

    atomic_init(&team->ready_workers, 0U);
    atomic_init(&team->completed_workers, 0U);
    atomic_init(&team->stop, false);
    rc = lccf_platform_event_create(&team->start_event);
    if (rc == 0) {
        rc = lccf_platform_event_create(&team->ready_event);
    }
    if (rc == 0) {
        rc = lccf_platform_event_create(&team->done_event);
    }
    if (rc != 0) {
        return rc;
    }
    for (index = 0U;
         index < batch->config.remote_producers;
         ++index) {
        lccf_model_remote_worker_t *worker =
            &team->workers[index];

        worker->batch = batch;
        worker->index = index;
        rc = lccf_platform_thread_start(
            &worker->thread, remote_worker_main, worker);
        if (rc != 0) {
            return rc;
        }
        team->worker_count += 1U;
    }
    return wait_for_remote_workers(
        team->ready_event,
        &team->ready_workers,
        team->worker_count);
}

static void remote_team_destroy(lccf_model_batch_t *batch) {
    lccf_model_remote_team_t *team;
    unsigned index;

    if (batch == NULL) {
        return;
    }
    team = &batch->remote_team;
    if (team->worker_count != 0U) {
        atomic_store_explicit(
            &team->stop, true, memory_order_release);
        if (team->start_event != NULL) {
            (void)lccf_platform_event_signal(
                team->start_event);
        }
        for (index = 0U;
             index < team->worker_count;
             ++index) {
            if (team->workers[index].thread != NULL) {
                (void)lccf_platform_thread_join(
                    team->workers[index].thread, NULL);
                team->workers[index].thread = NULL;
            }
        }
        team->worker_count = 0U;
    }
    lccf_platform_event_destroy(team->done_event);
    lccf_platform_event_destroy(team->ready_event);
    lccf_platform_event_destroy(team->start_event);
    team->done_event = NULL;
    team->ready_event = NULL;
    team->start_event = NULL;
}

static void accumulate_metrics(
    lccf_model_metrics_t *target,
    const lccf_model_metrics_t *source) {
    target->completions += source->completions;
    target->claims += source->claims;
    target->stale_tickets += source->stale_tickets;
    target->queue_pushes += source->queue_pushes;
    target->queue_pops += source->queue_pops;
    target->resume_calls += source->resume_calls;
    target->direct_calls += source->direct_calls;
    target->forced_escapes += source->forced_escapes;
    target->remote_pushes += source->remote_pushes;
    target->fairness_samples += source->fairness_samples;
    if (source->fairness_p99_ns > target->fairness_p99_ns) {
        target->fairness_p99_ns =
            source->fairness_p99_ns;
    }
    target->hot_allocations += source->hot_allocations;
    target->facts_attempted += source->facts_attempted;
    target->facts_built += source->facts_built;
    target->facts_build_failed += source->facts_build_failed;
    target->fact_normalizations += source->fact_normalizations;
    target->fact_site_lookups += source->fact_site_lookups;
    target->fact_module_pins += source->fact_module_pins;
    target->fact_payload_pins += source->fact_payload_pins;
    target->fact_stale_losers += source->fact_stale_losers;
    target->fact_guard_rechecks += source->fact_guard_rechecks;
    target->fact_queue_forwards += source->fact_queue_forwards;
    target->fact_generation_mismatches +=
        source->fact_generation_mismatches;
    target->fact_reuse_delays += source->fact_reuse_delays;
    if (source->fact_hot_bytes > target->fact_hot_bytes) {
        target->fact_hot_bytes = source->fact_hot_bytes;
    }
    if (source->fact_sidecar_bytes > target->fact_sidecar_bytes) {
        target->fact_sidecar_bytes = source->fact_sidecar_bytes;
    }
}

static int discard_remote_items(lccf_model_batch_t *batch,
                                uint64_t count) {
    uint64_t index;

    for (index = 0U; index < count; ++index) {
        void *item;
        const int rc = remote_queue_pop(
            &batch->remote_queue, &item);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int dispatch_remote_producers(
    lccf_model_batch_t *batch,
    lccf_model_metrics_t *metrics) {
    lccf_model_remote_team_t *team = &batch->remote_team;
    lccf_model_metrics_t produced = {0};
    const size_t enqueue_position = atomic_load_explicit(
        &batch->remote_queue.enqueue_position,
        memory_order_acquire);
    const size_t maximum_position_increment =
        batch->remote_queue.capacity +
        batch->config.instance_count - 1U;
    unsigned index;
    int first_error = 0;
    int rc;

    if (team->worker_count !=
            batch->config.remote_producers ||
        team->worker_count !=
            LCCF_MODEL_REMOTE_PRODUCER_COUNT ||
        enqueue_position !=
            batch->remote_queue.dequeue_position ||
        batch->remote_queue.capacity >
            SIZE_MAX - (batch->config.instance_count - 1U) ||
        enqueue_position >
            SIZE_MAX - maximum_position_increment) {
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
            lccf_platform_event_epoch(team->done_event);

        if (done_epoch == UINT64_MAX) {
            return EPROTO;
        }
        rc = lccf_platform_event_signal(team->start_event);
        if (rc == 0) {
            rc = lccf_platform_event_wait(
                team->done_event, done_epoch);
        }
        if (rc != 0) {
            return rc;
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
        produced.remote_pushes !=
            batch->config.instance_count) {
        rc = discard_remote_items(
            batch, produced.remote_pushes);
        return rc != 0 ?
                   rc :
                   (first_error != 0 ? first_error : EPROTO);
    }
    accumulate_metrics(metrics, &produced);
    return 0;
}

static int run_remote_wakers(lccf_model_batch_t *batch,
                             lccf_model_metrics_t *metrics) {
    size_t index;
    int rc = dispatch_remote_producers(batch, metrics);

    if (rc != 0) {
        return rc;
    }
    for (index = 0U;
         index < batch->config.instance_count;
         ++index) {
        void *item;

        rc = remote_queue_pop(&batch->remote_queue, &item);
        if (rc != 0) {
            return rc;
        }
        rc = resume_one_waker(batch, item, metrics);
        if (rc != 0) {
            (void)discard_remote_items(
                batch,
                (uint64_t)batch->config.instance_count -
                    (uint64_t)index - UINT64_C(1));
            return rc;
        }
    }
    return resume_queued_wakers(batch, metrics);
}

static int run_remote_cells(lccf_model_batch_t *batch,
                            lccf_model_metrics_t *metrics) {
    size_t index;
    int rc = dispatch_remote_producers(batch, metrics);

    if (rc != 0) {
        return rc;
    }
    for (index = 0U;
         index < batch->config.instance_count;
         ++index) {
        void *item;

        rc = remote_queue_pop(&batch->remote_queue, &item);
        if (rc != 0) {
            return rc;
        }
        rc = resume_one_cell(batch, item, metrics);
        if (rc != 0) {
            (void)discard_remote_items(
                batch,
                (uint64_t)batch->config.instance_count -
                    (uint64_t)index - UINT64_C(1));
            return rc;
        }
    }
    return resume_queued_cells(batch, metrics);
}

int lccf_model_run_round(lccf_model_batch_t *batch,
                         lccf_model_metrics_t *metrics) {
    int rc;

    if (batch == NULL || metrics == NULL ||
        (!mode_uses_wakers(batch->config.mode) &&
         !mode_uses_cells(batch->config.mode)) ||
        batch->local_queue.head != batch->local_queue.tail ||
        batch->overflow_head != NULL || batch->overflow_tail != NULL) {
        return EINVAL;
    }
    rc = validate_metrics(batch, metrics);
    if (rc != 0) {
        return rc;
    }
    rc = validate_instances(batch);
    if (rc != 0) {
        return rc;
    }
    batch->local_queue.head = 0U;
    batch->local_queue.tail = 0U;
    if (mode_uses_facts(batch->config.mode)) {
        const lccf_fact_layout_t layout =
            fact_layout_for_cell_bytes(batch->config.cell_bytes);

        metrics->fact_hot_bytes = lccf_fact_layout_hot_bytes(layout);
        metrics->fact_sidecar_bytes =
            lccf_fact_layout_sidecar_bytes(layout);
    }
    switch (batch->config.mode) {
        case LCCF_MODEL_WAKER_QUEUE:
            rc = claim_and_publish_wakers(batch, metrics);
            if (rc == 0) {
                rc = resume_queued_wakers(batch, metrics);
            }
            break;
        case LCCF_MODEL_CAUSAL_CELL_QUEUE:
            rc = claim_and_publish_cells(batch, metrics);
            if (rc == 0) {
                rc = resume_queued_cells(batch, metrics);
            }
            break;
        case LCCF_MODEL_FUSED_CAUSAL_CELL:
            rc = run_fused_cells(batch, metrics, false);
            break;
        case LCCF_MODEL_BUDGETED_FUSED_CHAIN:
            rc = run_fused_cells(batch, metrics, true);
            break;
        case LCCF_MODEL_REMOTE_WAKER_QUEUE:
            rc = run_remote_wakers(batch, metrics);
            break;
        case LCCF_MODEL_REMOTE_CAUSAL_CELL:
            rc = run_remote_cells(batch, metrics);
            break;
        case LCCF_MODEL_RECOMPUTE_QUEUE:
        case LCCF_MODEL_SHARED_FACT_QUEUE:
        case LCCF_MODEL_RECOMPUTE_FUSED:
        case LCCF_MODEL_SHARED_FACT_FUSED:
        case LCCF_MODEL_MIXED_RECOMPUTE:
        case LCCF_MODEL_MIXED_SHARED_FACT:
            rc = run_fact_cells(batch, metrics);
            break;
        default:
            return ENOTSUP;
    }
    if (rc != 0) {
        return rc;
    }
    batch->round += UINT64_C(1);
    return 0;
}

static bool canonical_config_equal(const lccf_model_batch_t *lhs,
                                   const lccf_model_batch_t *rhs) {
    return lhs->config.workload == rhs->config.workload &&
           lhs->config.instance_count == rhs->config.instance_count &&
           lhs->config.frame_bytes == rhs->config.frame_bytes &&
           lhs->config.cell_bytes == rhs->config.cell_bytes &&
           lhs->config.site_count == rhs->config.site_count &&
           lhs->config.chain_length == rhs->config.chain_length &&
           lhs->config.direct_budget == rhs->config.direct_budget &&
           lhs->config.remote_producers ==
               rhs->config.remote_producers &&
           lhs->config.callback_failure_step ==
               rhs->config.callback_failure_step &&
           lhs->config.fact_queue_capacity ==
               rhs->config.fact_queue_capacity &&
           lhs->config.seed == rhs->config.seed;
}

static const lccf_model_event_t *canonical_event(
    const lccf_model_batch_t *batch,
    size_t index) {
    const lccf_model_instance_t *instance =
        &batch->instances[index];

    return mode_uses_cells(batch->config.mode) ?
               &instance->cell->event :
               &instance->event;
}

static bool canonical_ready(const lccf_model_batch_t *batch,
                            size_t index) {
    const lccf_model_instance_t *instance =
        &batch->instances[index];
    uint64_t word;

    if (mode_uses_wakers(batch->config.mode)) {
        word = atomic_load_explicit(
            &instance->waker->state_generation,
            memory_order_acquire);
    } else if (mode_uses_cells(batch->config.mode)) {
        word = atomic_load_explicit(
            &instance->cell->state_generation,
            memory_order_acquire);
    } else {
        return false;
    }
    return lccf_model_unpack_generation(word) ==
               instance->frame->generation &&
           lccf_model_unpack_state(word) ==
               LCCF_MODEL_STATE_ARMED;
}

bool lccf_model_batch_equal(const lccf_model_batch_t *lhs,
                            const lccf_model_batch_t *rhs) {
    size_t i;

    if (lhs == rhs) {
        return lhs != NULL;
    }
    if (lhs == NULL || rhs == NULL ||
        !canonical_config_equal(lhs, rhs) ||
        lhs->round != rhs->round ||
        lhs->local_queue.head != lhs->local_queue.tail ||
        rhs->local_queue.head != rhs->local_queue.tail ||
        lhs->overflow_head != NULL || lhs->overflow_tail != NULL ||
        rhs->overflow_head != NULL || rhs->overflow_tail != NULL) {
        return false;
    }
    for (i = 0U; i < lhs->config.instance_count; ++i) {
        const lccf_model_frame_core_t *lhs_frame =
            lhs->instances[i].frame;
        const lccf_model_frame_core_t *rhs_frame =
            rhs->instances[i].frame;

        if (memcmp(lhs_frame,
                   rhs_frame,
                   lhs->config.frame_bytes) != 0 ||
            memcmp(canonical_event(lhs, i),
                   canonical_event(rhs, i),
                   sizeof(lccf_model_event_t)) != 0 ||
            memcmp(&lhs->instances[i].command,
                   &rhs->instances[i].command,
                   sizeof(lhs->instances[i].command)) != 0 ||
            !canonical_ready(lhs, i) ||
            !canonical_ready(rhs, i)) {
            return false;
        }
    }
    return true;
}

static uint64_t checksum_bytes(uint64_t checksum,
                               const void *data,
                               size_t size) {
    const unsigned char *bytes = data;
    size_t i;

    for (i = 0U; i < size; ++i) {
        checksum ^=
            (uint64_t)bytes[i] +
            UINT64_C(0x9E3779B97F4A7C15);
        checksum =
            lccf_model_mix64(checksum + (uint64_t)i);
    }
    return checksum;
}

uint64_t lccf_model_checksum(const lccf_model_batch_t *batch) {
    uint64_t checksum = UINT64_C(0x6A09E667F3BCC909);
    size_t i;

    if (batch == NULL ||
        batch->local_queue.head != batch->local_queue.tail ||
        batch->overflow_head != NULL || batch->overflow_tail != NULL) {
        return 0U;
    }
    checksum =
        checksum_bytes(checksum, &batch->round, sizeof(batch->round));
    for (i = 0U; i < batch->config.instance_count; ++i) {
        checksum = checksum_bytes(
            checksum,
            batch->instances[i].frame,
            batch->config.frame_bytes);
        checksum = checksum_bytes(
            checksum,
            canonical_event(batch, i),
            sizeof(lccf_model_event_t));
        checksum = checksum_bytes(
            checksum,
            &batch->instances[i].command,
            sizeof(batch->instances[i].command));
    }
    return checksum == 0U ? UINT64_C(1) : checksum;
}

bool lccf_model_fact_references_balanced(
    const lccf_model_batch_t *batch) {
    size_t instance_index;

    if (batch == NULL || !mode_uses_facts(batch->config.mode)) {
        return false;
    }
    for (instance_index = 0U;
         instance_index < batch->config.instance_count;
         ++instance_index) {
        const lccf_model_instance_t *instance =
            &batch->instances[instance_index];
        const lccf_fact_cell_t *cell = instance->fact_cell;
        const uint64_t state_word = cell == NULL ? 0U :
            atomic_load_explicit(&cell->state_generation,
                                 memory_order_acquire);
        size_t kind;

        const lccf_fact_state_t fact_state =
            lccf_fact_unpack_state(state_word);
        const uint64_t generation =
            lccf_fact_unpack_generation(state_word);
        const bool armed = fact_state == LCCF_FACT_STATE_ARMED;
        const bool terminal = fact_state == LCCF_FACT_STATE_TERMINAL;
        size_t ticket_index;

        if (cell == NULL ||
            atomic_load_explicit(&cell->published, memory_order_acquire) ||
            lccf_fact_storage(cell) != fact_storage_at(batch, instance_index) ||
            generation != instance->frame->generation ||
            (!armed && !terminal) ||
            atomic_load_explicit(
                &cell->references[LCCF_FACT_REF_BACKEND],
                memory_order_acquire) !=
                (armed ? active_ticket_count(batch) : 0U)) {
            return false;
        }
        for (ticket_index = 0U;
             ticket_index < LCCF_FACT_MAX_TICKETS;
             ++ticket_index) {
            const uint64_t expected_owner =
                armed && ticket_index < active_ticket_count(batch) ?
                    generation : 0U;

            if (atomic_load_explicit(
                    &cell->ticket_owners[ticket_index],
                    memory_order_acquire) != expected_owner) {
                return false;
            }
        }
        for (kind = 0U; kind < LCCF_FACT_REF_COUNT; ++kind) {
            if (kind != LCCF_FACT_REF_BACKEND &&
                atomic_load_explicit(&cell->references[kind],
                                     memory_order_acquire) != 0U) {
                return false;
            }
        }
    }
    return true;
}

int lccf_model_set_trace_buffer(lccf_model_batch_t *batch,
                                lccf_model_trace_row_t *rows,
                                size_t capacity) {
    if (batch == NULL || (rows == NULL) != (capacity == 0U) ||
        batch->round != 0U || batch->trace_count != 0U) {
        return EINVAL;
    }
    batch->trace_rows = rows;
    batch->trace_capacity = capacity;
    return 0;
}

size_t lccf_model_trace_count(const lccf_model_batch_t *batch) {
    return batch == NULL ? 0U : batch->trace_count;
}

const char *lccf_model_mode_name(lccf_model_mode_t mode) {
    if (!mode_valid(mode)) {
        return NULL;
    }
    return MODE_NAMES[(unsigned)mode];
}

const char *lccf_model_workload_name(lccf_model_workload_t workload) {
    if (!workload_valid(workload)) {
        return NULL;
    }
    return WORKLOAD_NAMES[(unsigned)workload];
}

static int parse_name(const char *text,
                      const char *const *names,
                      size_t count,
                      unsigned *out) {
    size_t i;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (i = 0U; i < count; ++i) {
        if (strcmp(text, names[i]) == 0) {
            *out = (unsigned)i;
            return 0;
        }
    }
    return EINVAL;
}

int lccf_model_parse_mode(const char *text, lccf_model_mode_t *out) {
    unsigned parsed;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    rc = parse_name(text,
                    MODE_NAMES,
                    sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]),
                    &parsed);
    if (rc != 0) {
        return rc;
    }
    *out = (lccf_model_mode_t)parsed;
    return 0;
}

int lccf_model_parse_workload(const char *text,
                              lccf_model_workload_t *out) {
    unsigned parsed;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    rc = parse_name(text,
                    WORKLOAD_NAMES,
                    sizeof(WORKLOAD_NAMES) /
                        sizeof(WORKLOAD_NAMES[0]),
                    &parsed);
    if (rc != 0) {
        return rc;
    }
    *out = (lccf_model_workload_t)parsed;
    return 0;
}

int lccf_model_candidate_baseline(
    lccf_model_mode_t candidate,
    lccf_model_mode_t *out_baseline) {
    if (out_baseline == NULL) {
        return EINVAL;
    }
    switch (candidate) {
        case LCCF_MODEL_CAUSAL_CELL_QUEUE:
        case LCCF_MODEL_FUSED_CAUSAL_CELL:
        case LCCF_MODEL_BUDGETED_FUSED_CHAIN:
            *out_baseline = LCCF_MODEL_WAKER_QUEUE;
            return 0;
        case LCCF_MODEL_REMOTE_CAUSAL_CELL:
            *out_baseline =
                LCCF_MODEL_REMOTE_WAKER_QUEUE;
            return 0;
        case LCCF_MODEL_SHARED_FACT_QUEUE:
            *out_baseline = LCCF_MODEL_RECOMPUTE_QUEUE;
            return 0;
        case LCCF_MODEL_SHARED_FACT_FUSED:
            *out_baseline = LCCF_MODEL_RECOMPUTE_FUSED;
            return 0;
        case LCCF_MODEL_MIXED_SHARED_FACT:
            *out_baseline = LCCF_MODEL_MIXED_RECOMPUTE;
            return 0;
        case LCCF_MODEL_WAKER_QUEUE:
        case LCCF_MODEL_REMOTE_WAKER_QUEUE:
        case LCCF_MODEL_RECOMPUTE_QUEUE:
        case LCCF_MODEL_RECOMPUTE_FUSED:
        case LCCF_MODEL_MIXED_RECOMPUTE:
        case LCCF_MODEL_MODE_COUNT:
        default:
            return EINVAL;
    }
}
