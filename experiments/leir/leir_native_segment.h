// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_NATIVE_SEGMENT_H
#define LLAM_EXPERIMENTS_LEIR_NATIVE_SEGMENT_H

#include "leir_native_plan.h"

#include <stddef.h>
#include <stdint.h>

enum {
    LEIR_NATIVE_MAX_BATCH_SEGMENTS = 8U,
};

typedef enum leir_native_mode {
    LEIR_NATIVE_MODE_LINK = 0,
    LEIR_NATIVE_MODE_LINK_CQE_SKIP = 1,
    LEIR_NATIVE_MODE_FIXED_LINK = 2,
    LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP = 3,
} leir_native_mode_t;

typedef struct leir_native_instance leir_native_instance_t;
typedef void (*leir_native_test_hook_fn)(void *context);

typedef struct leir_native_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
    uint16_t first_error_operation;
} leir_native_metrics_t;

typedef struct leir_native_batch_metrics {
    uint64_t activations;
    uint64_t segments;
    uint64_t queue_publications;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t operation_sqes;
    uint64_t operation_cqes;
    uint64_t cancel_sqes;
    uint64_t cancel_cqes;
    uint64_t hot_allocations;
} leir_native_batch_metrics_t;

size_t leir_native_instance_size(void);
int leir_native_instance_init(
    void *storage,
    size_t storage_size,
    const leir_phase0_program_t *program,
    const leir_native_plan_t *plan,
    leir_native_mode_t mode);
int leir_native_instance_bind(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values,
    size_t value_count);
int leir_native_instance_destroy(
    leir_native_instance_t *instance);
int leir_native_instance_run(
    leir_native_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_native_metrics_t *metrics_out);
int leir_native_batch_run(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    leir_native_batch_metrics_t *batch_metrics_out);
void leir_native_test_set_bind_before_claim_hook(
    leir_native_test_hook_fn hook,
    void *context);
size_t leir_native_test_linux_core_size(void);
size_t leir_native_test_linux_fixed_size(void);
const void *leir_native_test_linux_state_address(
    const leir_native_instance_t *instance);
const void *leir_native_test_linux_fixed_address(
    const leir_native_instance_t *instance);

#endif
