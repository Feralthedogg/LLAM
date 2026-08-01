// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

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

typedef struct lcwe_model_capsule {
    _Alignas(64) uint64_t state0[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state1[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state2[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state3[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t event0[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t event1[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t output[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t site[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t next_site[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t steps[LCWE_MODEL_MAX_LANES];
} lcwe_model_capsule_t;

typedef struct lcwe_model_aosoa {
    uint64_t *state0;
    uint64_t *state1;
    uint64_t *state2;
    uint64_t *state3;
    uint64_t *output;
    uint64_t *generation;
    uint64_t *event0;
    uint64_t *event1;
    uint64_t *command_output;
    uint32_t *site;
    uint32_t *steps;
    uint32_t *event_kind;
    uint32_t *next_site;
    uint32_t *command_kind;
    uint32_t *lifecycle;
} lcwe_model_aosoa_t;

typedef void (*lcwe_resume_one_fn)(lcwe_model_frame_t *,
                                   const lcwe_model_event_t *,
                                   lcwe_model_command_t *,
                                   unsigned site_count);
typedef void (*lcwe_resume_pointers_fn)(lcwe_model_ticket_t *const *,
                                        unsigned lane_count,
                                        unsigned site_count);
typedef void (*lcwe_resume_capsule_fn)(lcwe_model_capsule_t *,
                                       unsigned lane_count,
                                       unsigned site_count);
typedef void (*lcwe_resume_aosoa_fn)(lcwe_model_aosoa_t *,
                                     size_t first,
                                     unsigned lane_count,
                                     unsigned site_count);

typedef struct lcwe_model_workload_ops {
    lcwe_resume_one_fn resume_one;
    lcwe_resume_pointers_fn resume_pointers;
    lcwe_resume_capsule_fn resume_capsule;
    lcwe_resume_aosoa_fn resume_aosoa;
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
    lcwe_model_ticket_t **grouped;
    size_t site_counts[LCWE_MODEL_MAX_SITES];
    size_t site_offsets[LCWE_MODEL_MAX_SITES + 1U];
    size_t site_cursor[LCWE_MODEL_MAX_SITES];
    lcwe_model_capsule_t capsule;
    lcwe_model_aosoa_t *aosoa;
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
