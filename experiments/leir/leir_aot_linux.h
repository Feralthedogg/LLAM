// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_LINUX_H
#define LLAM_EXPERIMENTS_LEIR_AOT_LINUX_H

#include "leir_aot_module.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct leir_aot_linux_ticket leir_aot_linux_ticket_t;

typedef struct leir_aot_linux_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t queue_publications;
    uint64_t terminal_publications;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t interpreter_dispatches;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
    uint64_t prepare_ns;
    uint64_t ring_ns;
    uint64_t resume_ns;
    uint32_t first_error_operation;
    uint32_t resumed_continuation;
} leir_aot_linux_metrics_t;

size_t leir_aot_linux_ticket_size(void);
size_t leir_aot_linux_ticket_alignment(void);

int leir_aot_linux_ticket_init(
    void *ticket_storage,
    size_t ticket_storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size);

int leir_aot_linux_ticket_bind(
    leir_aot_linux_ticket_t *ticket,
    const leir_phase0_value_t *values,
    size_t value_count);

int leir_aot_linux_ticket_run(
    leir_aot_linux_ticket_t *ticket,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out,
    leir_aot_linux_metrics_t *metrics_out);

int leir_aot_linux_ticket_destroy(
    leir_aot_linux_ticket_t *ticket);

#ifdef __cplusplus
}
#endif

#endif
