// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_COMPLETION_H
#define LLAM_EXPERIMENTS_LEIR_AOT_COMPLETION_H

#include "leir_aot_module.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEIR_AOT_COMPLETION_SITE_CAPACITY 8U

typedef enum leir_aot_completion_route {
    LEIR_AOT_COMPLETION_DIRECT = 0,
    LEIR_AOT_COMPLETION_QUEUE = 1,
} leir_aot_completion_route_t;

enum {
    LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED = UINT64_C(1) << 0,
    LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE = UINT64_C(1) << 2,
    LEIR_AOT_COMPLETION_GUARD_STOP = UINT64_C(1) << 3,
    LEIR_AOT_COMPLETION_GUARD_FAIRNESS_DUE = UINT64_C(1) << 4,
    LEIR_AOT_COMPLETION_GUARD_TRACE = UINT64_C(1) << 5,
    LEIR_AOT_COMPLETION_GUARD_SHARD_PAUSED = UINT64_C(1) << 6,
    LEIR_AOT_COMPLETION_GUARD_SHARD_OFFLINE = UINT64_C(1) << 7,
    LEIR_AOT_COMPLETION_GUARD_CALLBACK_ACTIVE = UINT64_C(1) << 8,
    LEIR_AOT_COMPLETION_GUARD_QUEUE_PRESSURE = UINT64_C(1) << 9,
    LEIR_AOT_COMPLETION_GUARD_MIGRATING = UINT64_C(1) << 10,
};

typedef struct leir_aot_completion_record {
    uint64_t generation;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    uint32_t captured_home_shard;
    uint32_t source_node;
    uint32_t continuation;
    uint32_t source_kind;
    uint32_t event_kind;
    int32_t error_code;
} leir_aot_completion_record_t;

typedef struct leir_aot_completion_metrics {
    uint64_t publications;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t direct_consumes;
    uint64_t queued_consumes;
    uint64_t duplicate_rejections;
    uint64_t stale_rejections;
    uint64_t aborts;
} leir_aot_completion_metrics_t;

typedef struct leir_aot_completion leir_aot_completion_t;

size_t leir_aot_completion_size(void);
size_t leir_aot_completion_alignment(void);

int leir_aot_completion_init(
    void *storage,
    size_t storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size);

int leir_aot_completion_arm(
    leir_aot_completion_t *completion,
    uint64_t generation);

int leir_aot_completion_publish(
    leir_aot_completion_t *completion,
    const leir_aot_completion_record_t *record);

/*
 * Returns zero after the generated continuation reaches terminal state,
 * EAGAIN after queue transfer or deferral, and a positive errno otherwise.
 * The generation argument is mandatory so a delayed queue delivery cannot
 * consume a newly armed generation.
 */
int leir_aot_completion_consume(
    leir_aot_completion_t *completion,
    uint64_t generation,
    leir_aot_completion_route_t route,
    uint64_t guard_flags,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out);

int leir_aot_completion_cancel(
    leir_aot_completion_t *completion,
    uint32_t continuation);

int leir_aot_completion_set_module_available(
    leir_aot_completion_t *completion,
    bool available);

int leir_aot_completion_destroy(
    leir_aot_completion_t *completion);

void leir_aot_completion_metrics(
    const leir_aot_completion_t *completion,
    leir_aot_completion_metrics_t *out);

#ifdef __cplusplus
}
#endif

#endif
