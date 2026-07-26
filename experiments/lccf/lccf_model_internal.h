// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_LCCF_MODEL_INTERNAL_H
#define LLAM_EXPERIMENTS_LCCF_MODEL_INTERNAL_H

#include "lccf_model.h"
#include "lccf_platform.h"

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

typedef struct lccf_model_cell_hot {
    _Atomic uint64_t state_generation;
    lccf_model_instance_t *instance;
    lccf_model_event_t event;
    uint64_t command_word;
    uint32_t home_shard;
    uint32_t next_site;
    _Atomic uint32_t queue_owned;
    _Atomic uint32_t backend_refs;
} lccf_model_cell_hot_t;

_Static_assert(sizeof(lccf_model_cell_hot_t) == 64U,
               "LCCF causal hot fields must occupy one cache line");

typedef struct lccf_model_ticket {
    lccf_model_cell_hot_t *target;
    uint64_t generation;
    lccf_model_event_t event;
    uint32_t instance_index;
    uint32_t ticket_index;
} lccf_model_ticket_t;

#define LCCF_MODEL_TICKETS_PER_INSTANCE 3U
#define LCCF_MODEL_REMOTE_PRODUCER_COUNT 2U

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
    lccf_model_cell_hot_t *cell;
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

typedef struct lccf_model_remote_slot {
    _Atomic size_t sequence;
    void *item;
} lccf_model_remote_slot_t;

typedef struct lccf_model_remote_queue {
    lccf_model_remote_slot_t *slots;
    size_t capacity;
    size_t mask;
    _Atomic size_t enqueue_position;
    size_t dequeue_position;
} lccf_model_remote_queue_t;

typedef struct lccf_model_remote_worker {
    struct lccf_model_batch *batch;
    lccf_platform_thread_t *thread;
    lccf_model_metrics_t metrics;
    unsigned index;
    int error;
} lccf_model_remote_worker_t;

typedef struct lccf_model_remote_team {
    lccf_platform_event_t *start_event;
    lccf_platform_event_t *ready_event;
    lccf_platform_event_t *done_event;
    _Atomic unsigned ready_workers;
    _Atomic unsigned completed_workers;
    _Atomic bool stop;
    unsigned worker_count;
    lccf_model_remote_worker_t
        workers[LCCF_MODEL_REMOTE_PRODUCER_COUNT];
} lccf_model_remote_team_t;

struct lccf_model_batch {
    lccf_model_config_t config;
    uint64_t round;
    unsigned char *frame_storage;
    unsigned char *cell_storage;
    lccf_model_instance_t *instances;
    lccf_model_waker_t *wakers;
    lccf_model_ticket_t *tickets;
    lccf_model_local_queue_t local_queue;
    lccf_model_remote_queue_t remote_queue;
    lccf_model_remote_team_t remote_team;
    const lccf_model_workload_ops_t *ops;
    unsigned callback_depth;
    unsigned maximum_callback_depth;
    uint64_t fairness_tick;
    uint64_t fairness_due_ns;
    uint64_t fairness_services;
    uint64_t fairness_histogram[64];
    bool fairness_due;
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
lccf_model_cell_hot_t *lccf_model_cell_at(
    const lccf_model_batch_t *batch,
    size_t index);
lccf_model_ticket_t *lccf_model_ticket_at(
    const lccf_model_batch_t *batch,
    size_t instance_index,
    unsigned ticket_index);
int lccf_model_try_claim_ticket(
    const lccf_model_ticket_t *ticket,
    bool *out_won);
int lccf_model_resume_claimed_cell(
    lccf_model_batch_t *batch,
    size_t instance_index,
    lccf_model_metrics_t *metrics);

#endif
