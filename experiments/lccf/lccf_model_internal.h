// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_LCCF_MODEL_INTERNAL_H
#define LLAM_EXPERIMENTS_LCCF_MODEL_INTERNAL_H

#include "lccf_model.h"

#include <stdatomic.h>

typedef enum lccf_model_state {
    LCCF_MODEL_STATE_IDLE = 0,
    LCCF_MODEL_STATE_ARMED = 1,
    LCCF_MODEL_STATE_CLAIMED = 2,
    LCCF_MODEL_STATE_QUEUED = 3,
    LCCF_MODEL_STATE_RUNNING = 4,
    LCCF_MODEL_STATE_TERMINAL = 5,
} lccf_model_state_t;

typedef enum lccf_model_event_kind {
    LCCF_MODEL_EVENT_IO = 1,
    LCCF_MODEL_EVENT_TIMEOUT = 2,
    LCCF_MODEL_EVENT_CANCEL = 3,
    LCCF_MODEL_EVENT_TIMER = 4,
    LCCF_MODEL_EVENT_INTERNAL = 5,
} lccf_model_event_kind_t;

typedef struct lccf_model_frame_core {
    uint64_t state0;
    uint64_t state1;
    uint64_t state2;
    uint64_t output;
    uint64_t generation;
    uint64_t command_word;
    uint32_t site;
    uint32_t steps;
    uint32_t terminal;
    uint32_t reserved;
} lccf_model_frame_core_t;

_Static_assert(sizeof(lccf_model_frame_core_t) == 64U,
               "LCCF frame core must occupy exactly 64 bytes");

typedef struct lccf_model_event {
    uint64_t word0;
    uint64_t word1;
    uint32_t kind;
    uint32_t reserved;
} lccf_model_event_t;

typedef struct lccf_model_command {
    uint64_t output;
    uint32_t next_site;
    uint32_t kind;
} lccf_model_command_t;

struct lccf_model_batch;
typedef struct lccf_model_instance lccf_model_instance_t;

typedef struct lccf_model_waker {
    _Atomic uint64_t state_generation;
    lccf_model_instance_t *instance;
    uint32_t resume_site;
    uint32_t reserved;
} lccf_model_waker_t;

typedef void (*lccf_model_resume_fn)(
    lccf_model_frame_core_t *frame,
    const lccf_model_event_t *event,
    lccf_model_command_t *command,
    const lccf_model_config_t *config,
    unsigned resume_site);

typedef struct lccf_model_workload_ops {
    lccf_model_resume_fn resume_sites[LCCF_MODEL_MAX_SITES];
} lccf_model_workload_ops_t;

struct lccf_model_instance {
    struct lccf_model_batch *batch;
    lccf_model_frame_core_t *frame;
    lccf_model_waker_t *waker;
    lccf_model_event_t event;
    lccf_model_command_t command;
    uint32_t index;
    uint32_t reserved;
};

typedef struct lccf_model_local_queue {
    void **slots;
    size_t capacity;
    size_t mask;
    size_t head;
    size_t tail;
} lccf_model_local_queue_t;

struct lccf_model_batch {
    lccf_model_config_t config;
    uint64_t round;
    unsigned char *frame_storage;
    lccf_model_instance_t *instances;
    lccf_model_waker_t *wakers;
    lccf_model_local_queue_t local_queue;
    const lccf_model_workload_ops_t *ops;
};

uint64_t lccf_model_mix64(uint64_t value);
uint64_t lccf_model_pack_state(uint64_t generation,
                               lccf_model_state_t state);
uint64_t lccf_model_unpack_generation(uint64_t word);
lccf_model_state_t lccf_model_unpack_state(uint64_t word);
void lccf_model_derive_event(const lccf_model_batch_t *batch,
                             const lccf_model_instance_t *instance,
                             lccf_model_event_t *event);
const lccf_model_workload_ops_t *
lccf_model_get_workload_ops(lccf_model_workload_t workload);

#endif
