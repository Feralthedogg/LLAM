// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_NATIVE_PLAN_H
#define LLAM_EXPERIMENTS_LEIR_NATIVE_PLAN_H

#include "leir_phase0.h"

#include <stdint.h>

#define LEIR_NATIVE_MAX_OPS 8U

typedef enum leir_native_step_kind {
    LEIR_NATIVE_STEP_RECV = 0,
    LEIR_NATIVE_STEP_SEND = 1,
} leir_native_step_kind_t;

typedef struct leir_native_step {
    uint16_t kind;
    uint16_t fd_slot;
    uint16_t buffer_slot;
    uint16_t length_slot;
    uint16_t result_slot;
} leir_native_step_t;

typedef struct leir_native_plan {
    leir_native_step_t steps[LEIR_NATIVE_MAX_OPS];
    uint16_t step_count;
    uint16_t return_node;
    uint16_t result_slot;
} leir_native_plan_t;

int leir_native_plan_compile(
    const leir_phase0_program_t *program,
    leir_native_plan_t *out);

#endif
