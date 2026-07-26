// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_SREM_MODEL_INTERNAL_H
#define LLAM_EXPERIMENTS_SREM_MODEL_INTERNAL_H

#include "srem_model.h"

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
    uint64_t state_generation;
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

struct srem_model_batch {
    srem_model_config_t config;
    uint64_t round;
    unsigned char *frame_storage;
    srem_model_effect_t *effects;
    srem_model_event_t *events;
    srem_model_waker_t *wakers;
    srem_model_ticket_t *tickets;
    srem_model_index_queue_t local_queue;
    const srem_model_workload_ops_t *ops;
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

#endif
