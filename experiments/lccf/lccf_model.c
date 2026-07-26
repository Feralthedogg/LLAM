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
};

static const char *const WORKLOAD_NAMES[] = {
    "completion_io_pipeline",
    "completion_rpc_state",
    "completion_timer_cancel",
    "completion_mixed_fairness",
};

static bool mode_valid(lccf_model_mode_t mode) {
    return mode >= LCCF_MODEL_WAKER_QUEUE &&
           mode <= LCCF_MODEL_REMOTE_CAUSAL_CELL;
}

static bool workload_valid(lccf_model_workload_t workload) {
    return workload >= LCCF_MODEL_COMPLETION_IO_PIPELINE &&
           workload <= LCCF_MODEL_COMPLETION_MIXED_FAIRNESS;
}

static bool mode_uses_wakers(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_WAKER_QUEUE;
}

static bool mode_uses_cells(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_CAUSAL_CELL_QUEUE ||
           mode == LCCF_MODEL_FUSED_CAUSAL_CELL ||
           mode == LCCF_MODEL_BUDGETED_FUSED_CHAIN;
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
        &cell->backend_refs, count, memory_order_release);
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
    memset(&instance->event, 0, sizeof(instance->event));
    memset(&instance->command, 0, sizeof(instance->command));
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
    event->reserved = 0U;
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
        config->remote_producers > 2U) {
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

    queue_capacity = next_power_of_two(config->instance_count);
    if (queue_capacity == 0U ||
        !allocation_size_valid(queue_capacity, sizeof(void *))) {
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
    batch->local_queue.slots =
        calloc(queue_capacity, sizeof(*batch->local_queue.slots));
    if (batch->frame_storage == NULL || batch->instances == NULL ||
        batch->local_queue.slots == NULL ||
        (mode_uses_wakers(config->mode) &&
         batch->wakers == NULL) ||
        (mode_uses_cells(config->mode) &&
         (batch->cell_storage == NULL || batch->tickets == NULL))) {
        lccf_model_batch_destroy(batch);
        return ENOMEM;
    }
    batch->local_queue.capacity = queue_capacity;
    batch->local_queue.mask = queue_capacity - 1U;
    batch->ops = lccf_model_get_workload_ops(config->workload);
    if (batch->ops == NULL) {
        lccf_model_batch_destroy(batch);
        return EPROTO;
    }
    for (i = 0U; i < config->instance_count; ++i) {
        if (initialize_instance(batch, i, true) != 0) {
            lccf_model_batch_destroy(batch);
            return EPROTO;
        }
    }
    *out_batch = batch;
    return 0;
}

void lccf_model_batch_destroy(lccf_model_batch_t *batch) {
    if (batch == NULL) {
        return;
    }
    free(batch->local_queue.slots);
    free(batch->tickets);
    free(batch->wakers);
    free(batch->cell_storage);
    free(batch->instances);
    free(batch->frame_storage);
    free(batch);
}

int lccf_model_batch_reset(lccf_model_batch_t *batch) {
    size_t i;

    if (batch == NULL || batch->local_queue.slots == NULL ||
        batch->local_queue.head != batch->local_queue.tail) {
        return EINVAL;
    }
    memset(batch->local_queue.slots,
           0,
           batch->local_queue.capacity *
               sizeof(*batch->local_queue.slots));
    batch->local_queue.head = 0U;
    batch->local_queue.tail = 0U;
    batch->round = 0U;
    batch->callback_depth = 0U;
    batch->maximum_callback_depth = 0U;
    batch->fairness_tick = 0U;
    batch->fairness_due_tick = 0U;
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
    if (!metric_room(metrics->completions, instances) ||
        !metric_room(metrics->claims, instances) ||
        !metric_room(metrics->queue_pushes, callback_count) ||
        !metric_room(metrics->queue_pops, callback_count) ||
        !metric_room(metrics->resume_calls, callback_count) ||
        !metric_room(metrics->direct_calls, callback_count) ||
        !metric_room(metrics->forced_escapes, callback_count) ||
        !metric_room(metrics->fairness_samples, callback_count) ||
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
                                     memory_order_acquire) != refs) {
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

static void fairness_note_callback(lccf_model_batch_t *batch) {
    batch->fairness_tick += UINT64_C(1);
    if (batch->config.workload ==
            LCCF_MODEL_COMPLETION_MIXED_FAIRNESS &&
        !batch->fairness_due &&
        (batch->fairness_tick & UINT64_C(31)) == 0U) {
        batch->fairness_due = true;
        batch->fairness_due_tick = batch->fairness_tick;
    }
}

static void fairness_service(lccf_model_batch_t *batch,
                             lccf_model_metrics_t *metrics) {
    uint64_t latency;
    unsigned bucket;

    if (!batch->fairness_due) {
        return;
    }
    latency =
        batch->fairness_tick - batch->fairness_due_tick +
        UINT64_C(1);
    bucket = fairness_bucket(latency);
    batch->fairness_histogram[bucket] += UINT64_C(1);
    batch->fairness_services += UINT64_C(1);
    metrics->fairness_samples += UINT64_C(1);
    batch->fairness_due = false;
    update_fairness_p99(batch, metrics);
}

static int execute_resume_callback(
    lccf_model_batch_t *batch,
    lccf_model_instance_t *instance,
    const lccf_model_event_t *event,
    unsigned site,
    bool direct,
    lccf_model_metrics_t *metrics) {
    lccf_model_resume_fn resume;

    if (site >= batch->config.site_count ||
        batch->callback_depth == UINT_MAX) {
        return EPROTO;
    }
    resume = batch->ops->resume_sites[site];
    if (resume == NULL) {
        return EPROTO;
    }
    memset(&instance->command, 0, sizeof(instance->command));
    batch->callback_depth += 1U;
    if (batch->callback_depth > batch->maximum_callback_depth) {
        batch->maximum_callback_depth = batch->callback_depth;
    }
    fairness_note_callback(batch);
    resume(instance->frame,
           event,
           &instance->command,
           &batch->config,
           site);
    batch->callback_depth -= 1U;
    metrics->resume_calls += UINT64_C(1);
    if (direct) {
        metrics->direct_calls += UINT64_C(1);
    }
    return command_valid(batch, instance) ? 0 : EPROTO;
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

static int resume_queued_wakers(lccf_model_batch_t *batch,
                                lccf_model_metrics_t *metrics) {
    lccf_model_waker_t *waker;

    while ((waker = queue_pop(&batch->local_queue)) != NULL) {
        lccf_model_instance_t *instance = waker->instance;
        lccf_model_frame_core_t *frame = instance->frame;
        const uint64_t generation = frame->generation;
        const unsigned site = waker->resume_site;

        if (site >= batch->config.site_count) {
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
        fairness_service(batch, metrics);
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
        uint64_t expected;

        if (ticket == NULL || ticket->target != cell) {
            return EPROTO;
        }
        expected = lccf_model_pack_state(
            ticket->generation, LCCF_MODEL_STATE_ARMED);
        if (atomic_compare_exchange_strong_explicit(
                &cell->state_generation,
                &expected,
                lccf_model_pack_state(
                    ticket->generation,
                    LCCF_MODEL_STATE_CLAIMED),
                memory_order_acq_rel,
                memory_order_acquire)) {
            if (won) {
                return EPROTO;
            }
            cell->event = ticket->event;
            cell->next_site = instance->frame->site;
            metrics->completions += UINT64_C(1);
            metrics->claims += UINT64_C(1);
            won = true;
        } else {
            metrics->stale_tickets += UINT64_C(1);
        }
        if (retire_backend_reference(cell) != 0) {
            return EPROTO;
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
        return EOVERFLOW;
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

static int resume_queued_cells(lccf_model_batch_t *batch,
                               lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell;

    while ((cell = queue_pop(&batch->local_queue)) != NULL) {
        lccf_model_instance_t *instance = cell->instance;
        uint64_t word;
        unsigned site;

        if (instance == NULL || instance->batch != batch ||
            instance->cell != cell) {
            return EPROTO;
        }
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
        fairness_service(batch, metrics);
        publish_cell_command(instance);

        if (instance->command.kind ==
            LCCF_MODEL_COMMAND_CONTINUE) {
            const int rc = enqueue_claimed_cell(
                batch, cell, metrics, false);

            if (rc != 0) {
                return rc;
            }
        } else if (rearm_cell_generation(batch, instance) != 0) {
            return EPROTO;
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
            fairness_service(batch, metrics);
            return rearm_cell_generation(batch, instance);
        }
        if (budgeted &&
            (direct_count >= batch->config.direct_budget ||
             batch->fairness_due)) {
            rc = enqueue_claimed_cell(
                batch, cell, metrics, true);
            if (rc == 0) {
                fairness_service(batch, metrics);
            }
            return rc;
        }
        expected_state = LCCF_MODEL_STATE_RUNNING;
    }
}

static int resume_budgeted_escape_cells(
    lccf_model_batch_t *batch,
    lccf_model_metrics_t *metrics) {
    lccf_model_cell_hot_t *cell;

    while ((cell = queue_pop(&batch->local_queue)) != NULL) {
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
        fairness_service(batch, metrics);
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

int lccf_model_run_round(lccf_model_batch_t *batch,
                         lccf_model_metrics_t *metrics) {
    int rc;

    if (batch == NULL || metrics == NULL ||
        (!mode_uses_wakers(batch->config.mode) &&
         !mode_uses_cells(batch->config.mode)) ||
        batch->local_queue.head != batch->local_queue.tail) {
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
        case LCCF_MODEL_REMOTE_CAUSAL_CELL:
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
        rhs->local_queue.head != rhs->local_queue.tail) {
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
        batch->local_queue.head != batch->local_queue.tail) {
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
