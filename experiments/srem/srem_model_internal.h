// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_SREM_MODEL_INTERNAL_H
#define LLAM_EXPERIMENTS_SREM_MODEL_INTERNAL_H

#include "srem_model.h"
#include "srem_platform.h"

#include <stdatomic.h>

typedef enum srem_model_waker_state {
    SREM_MODEL_WAKER_IDLE = 0,
    SREM_MODEL_WAKER_ARMED = 1,
    SREM_MODEL_WAKER_QUEUED = 2,
    SREM_MODEL_WAKER_RUNNING = 3,
    SREM_MODEL_WAKER_TERMINAL = 4,
} srem_model_waker_state_t;

typedef enum srem_model_event_kind {
    SREM_MODEL_EVENT_READ = 1,
    SREM_MODEL_EVENT_WRITE = 2,
    SREM_MODEL_EVENT_TIMEOUT = 3,
    SREM_MODEL_EVENT_CANCEL = 4,
    SREM_MODEL_EVENT_INTERNAL = 5,
} srem_model_event_kind_t;

typedef enum srem_model_effect_kind {
    SREM_MODEL_EFFECT_READ = 1,
    SREM_MODEL_EFFECT_WRITE = 2,
    SREM_MODEL_EFFECT_TIMER = 3,
    SREM_MODEL_EFFECT_YIELD = 4,
    SREM_MODEL_EFFECT_COMPLETE = 5,
    SREM_MODEL_EFFECT_ESCAPE = 6,
} srem_model_effect_kind_t;

typedef struct srem_model_frame_core {
    uint64_t field[6];
    uint32_t generation;
    uint16_t site;
    uint16_t steps;
    uint32_t terminal;
    uint32_t flags;
} srem_model_frame_core_t;

_Static_assert(sizeof(srem_model_frame_core_t) == 64U,
               "SREM frame core must occupy exactly 64 bytes");

typedef struct srem_model_event {
    uint64_t word0;
    uint64_t word1;
    int32_t result;
    uint16_t site;
    uint8_t kind;
    uint8_t divergent;
} srem_model_event_t;

_Static_assert(sizeof(srem_model_event_t) == 24U,
               "SREM event must occupy exactly 24 bytes");

typedef struct srem_model_effect {
    uint64_t argument0;
    uint64_t argument1;
    uint64_t argument2;
    uint32_t operation;
    uint16_t next_site;
    uint16_t flags;
} srem_model_effect_t;

_Static_assert(sizeof(srem_model_effect_t) == 32U,
               "SREM effect descriptor must occupy exactly 32 bytes");

typedef struct srem_model_waker {
    _Atomic uint64_t state_generation;
    uint32_t instance_index;
    uint16_t resume_site;
    uint16_t reserved;
} srem_model_waker_t;

typedef struct srem_model_ticket {
    srem_model_event_t event;
    uint32_t instance_index;
    uint32_t generation;
} srem_model_ticket_t;

typedef void (*srem_model_resume_fn)(
    srem_model_frame_core_t *frame,
    const srem_model_event_t *event,
    srem_model_effect_t *effect,
    const srem_model_config_t *config,
    unsigned resume_site);

typedef struct srem_model_workload_ops {
    srem_model_resume_fn resume_sites[SREM_MODEL_MAX_SITES];
} srem_model_workload_ops_t;

typedef struct srem_model_index_queue {
    uint32_t *slots;
    size_t capacity;
    size_t mask;
    size_t head;
    size_t tail;
} srem_model_index_queue_t;

typedef struct srem_model_tile {
    uint32_t ready_mask[SREM_MODEL_MAX_SITES];
    uint32_t pending_mask;
    uint32_t valid_mask;
    uint32_t queued;
    uint32_t running;
    uint32_t reserved;
} srem_model_tile_t;

typedef struct srem_model_tile_view {
    unsigned width;
    unsigned resume_site;
    uint32_t active_mask;
    uint64_t *field[6];
    uint32_t *generation;
    uint16_t *site;
    uint16_t *steps;
    uint32_t *terminal;
    uint32_t *flags;
    uint64_t *event_word0;
    uint64_t *event_word1;
    int32_t *event_result;
    uint16_t *event_site;
    uint8_t *event_kind;
    uint8_t *event_divergent;
    uint64_t *effect_argument[3];
    uint32_t *effect_operation;
    uint16_t *effect_next_site;
    uint16_t *effect_flags;
} srem_model_tile_view_t;

typedef struct srem_model_remote_slot {
    _Atomic size_t sequence;
    uint32_t item;
} srem_model_remote_slot_t;

typedef struct srem_model_remote_queue {
    srem_model_remote_slot_t *slots;
    size_t capacity;
    size_t mask;
    _Atomic size_t enqueue_position;
    size_t dequeue_position;
} srem_model_remote_queue_t;

typedef struct srem_model_remote_worker {
    struct srem_model_batch *batch;
    srem_platform_thread_t *thread;
    srem_model_metrics_t metrics;
    unsigned index;
    int error;
    int affinity_result;
} srem_model_remote_worker_t;

typedef struct srem_model_remote_team {
    srem_platform_event_t *start_event;
    srem_platform_event_t *ready_event;
    srem_platform_event_t *done_event;
    _Atomic unsigned ready_workers;
    _Atomic unsigned completed_workers;
    _Atomic bool stop;
    unsigned worker_count;
    srem_model_remote_worker_t
        workers[SREM_MODEL_REMOTE_PRODUCER_COUNT];
} srem_model_remote_team_t;

struct srem_model_batch {
    srem_model_config_t config;
    uint64_t round;
    unsigned char *frame_storage;
    srem_model_effect_t *effects;
    srem_model_event_t *events;
    srem_model_waker_t *wakers;
    srem_model_ticket_t *tickets;
    srem_model_index_queue_t local_queue;
    srem_model_remote_queue_t remote_queue;
    srem_model_remote_team_t remote_team;
    uint32_t *remote_active_indices;
    size_t remote_active_count;
    _Atomic uint32_t *remote_ready_masks;
    _Atomic uint32_t *remote_pending_masks;
    _Atomic uint32_t *remote_published;
    const srem_model_workload_ops_t *ops;
    size_t tile_count;
    size_t tile_slot_count;
    srem_model_tile_t *tiles;
    uint64_t *tile_fields;
    uint32_t *tile_generations;
    uint16_t *tile_sites;
    uint16_t *tile_steps;
    uint32_t *tile_terminal;
    uint32_t *tile_flags;
    uint64_t *tile_event_word0;
    uint64_t *tile_event_word1;
    int32_t *tile_event_result;
    uint16_t *tile_event_site;
    uint8_t *tile_event_kind;
    uint8_t *tile_event_divergent;
    uint64_t *tile_effect_arguments;
    uint32_t *tile_effect_operation;
    uint16_t *tile_effect_next_site;
    uint16_t *tile_effect_flags;
    uint64_t *fairness_due_ticks;
    uint64_t *fairness_histogram;
    size_t fairness_histogram_size;
    uint64_t fairness_tick;
    uint64_t fairness_sample_count;
};

uint64_t srem_model_mix64(uint64_t value);
uint64_t srem_model_pack_waker(uint32_t generation,
                               srem_model_waker_state_t state);
uint32_t srem_model_unpack_generation(uint64_t word);
srem_model_waker_state_t srem_model_unpack_waker_state(uint64_t word);
srem_model_frame_core_t *srem_model_frame_at(
    const srem_model_batch_t *batch,
    size_t index);
void srem_model_derive_event(const srem_model_batch_t *batch,
                             size_t index,
                             unsigned site,
                             srem_model_event_t *event);
const srem_model_workload_ops_t *
srem_model_get_workload_ops(srem_model_workload_t workload);
int srem_model_make_ticket(const srem_model_batch_t *batch,
                           size_t index,
                           srem_model_ticket_t *out_ticket);
int srem_model_tile_admit_ticket(srem_model_batch_t *batch,
                                 const srem_model_ticket_t *ticket,
                                 srem_model_metrics_t *metrics);
int srem_model_tile_drain(srem_model_batch_t *batch,
                          srem_model_metrics_t *metrics);
uint32_t *srem_model_tile_generation_at(srem_model_batch_t *batch,
                                        size_t index);
int srem_model_run_vector_superblock(
    srem_model_workload_t workload,
    const srem_model_config_t *config,
    srem_model_tile_view_t *view);

#endif
