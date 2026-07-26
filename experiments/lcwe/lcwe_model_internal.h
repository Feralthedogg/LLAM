// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_LCWE_MODEL_INTERNAL_H
#define LLAM_EXPERIMENTS_LCWE_MODEL_INTERNAL_H

#include "lcwe_model.h"

typedef struct lcwe_model_frame {
    uint64_t state0;
    uint64_t state1;
    uint64_t state2;
    uint64_t state3;
    uint64_t output;
    uint64_t generation;
    uint32_t site;
    uint32_t steps;
} lcwe_model_frame_t;

typedef struct lcwe_model_event {
    uint64_t word0;
    uint64_t word1;
    uint32_t kind;
    uint32_t reserved;
} lcwe_model_event_t;

typedef struct lcwe_model_command {
    uint64_t output;
    uint32_t next_site;
    uint32_t kind;
} lcwe_model_command_t;

typedef struct lcwe_model_ticket {
    lcwe_model_frame_t *frame;
    lcwe_model_event_t event;
    lcwe_model_command_t command;
    uint64_t generation;
    uint32_t site;
    uint32_t lifecycle;
    uint32_t frame_index;
    uint32_t reserved;
} lcwe_model_ticket_t;

typedef void (*lcwe_resume_one_fn)(lcwe_model_frame_t *,
                                   const lcwe_model_event_t *,
                                   lcwe_model_command_t *,
                                   unsigned site_count);

typedef struct lcwe_model_workload_ops {
    lcwe_resume_one_fn resume_one;
} lcwe_model_workload_ops_t;

struct lcwe_model_batch {
    lcwe_model_workload_t workload;
    lcwe_model_mode_t mode;
    size_t instance_count;
    unsigned site_count;
    uint64_t seed;
    uint64_t round;
    lcwe_model_frame_t *frames;
    lcwe_model_ticket_t *tickets;
    lcwe_model_ticket_t **ready;
    const lcwe_model_workload_ops_t *ops;
};

uint64_t lcwe_model_mix64(uint64_t value);
void lcwe_model_derive_event(uint64_t seed,
                             uint32_t frame_index,
                             const lcwe_model_frame_t *frame,
                             lcwe_model_event_t *event);
const lcwe_model_workload_ops_t *
lcwe_model_get_workload_ops(lcwe_model_workload_t workload);

#endif
