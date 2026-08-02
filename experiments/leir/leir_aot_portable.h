// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_PORTABLE_H
#define LLAM_EXPERIMENTS_LEIR_AOT_PORTABLE_H

#include "leir_aot_module.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*leir_aot_portable_connect_fn)(
    void *context,
    llam_fd_t fd,
    const struct sockaddr *address,
    socklen_t address_length);

typedef ssize_t (*leir_aot_portable_write_fn)(
    void *context,
    llam_fd_t fd,
    const void *payload,
    size_t payload_length);

typedef struct leir_aot_portable_effects {
    void *context;
    leir_aot_portable_connect_fn connect;
    leir_aot_portable_write_fn write;
} leir_aot_portable_effects_t;

typedef struct leir_aot_portable_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t effect_calls;
    uint64_t terminal_publications;
    uint64_t interpreter_dispatches;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t hot_allocations;
    uint64_t generation;
    uint32_t resumed_continuation;
} leir_aot_portable_metrics_t;

typedef struct leir_aot_portable_ticket leir_aot_portable_ticket_t;

size_t leir_aot_portable_ticket_size(void);
size_t leir_aot_portable_ticket_alignment(void);

int leir_aot_portable_ticket_init(
    void *ticket_storage,
    size_t ticket_storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size,
    const leir_aot_portable_effects_t *effects);

int leir_aot_portable_ticket_bind(
    leir_aot_portable_ticket_t *ticket,
    const leir_phase0_value_t *values,
    size_t value_count);

int leir_aot_portable_ticket_run(
    leir_aot_portable_ticket_t *ticket,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out,
    leir_aot_portable_metrics_t *metrics_out);

int leir_aot_portable_ticket_cancel(
    leir_aot_portable_ticket_t *ticket);

int leir_aot_portable_ticket_destroy(
    leir_aot_portable_ticket_t *ticket);

#ifdef __cplusplus
}
#endif

#endif
