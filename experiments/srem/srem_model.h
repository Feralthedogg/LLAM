// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_SREM_MODEL_H
#define LLAM_EXPERIMENTS_SREM_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SREM_MODEL_MAX_TILE_WIDTH 32U
#define SREM_MODEL_MAX_SITES 8U
#define SREM_MODEL_MIN_FRAME_BYTES 64U
#define SREM_MODEL_MAX_FRAME_BYTES 256U
#define SREM_MODEL_REMOTE_PRODUCER_COUNT 2U

typedef enum srem_model_mode {
    SREM_MODEL_WAKER_FRAME = 0,
    SREM_MODEL_TILE_SCALAR = 1,
    SREM_MODEL_TILE_VECTOR = 2,
    SREM_MODEL_ADAPTIVE = 3,
    SREM_MODEL_REMOTE_WAKER_FRAME = 4,
    SREM_MODEL_REMOTE_ADAPTIVE = 5,
} srem_model_mode_t;

typedef enum srem_model_workload {
    SREM_MODEL_HTTP_PIPELINE = 0,
    SREM_MODEL_RPC_PIPELINE = 1,
    SREM_MODEL_DIVERGENT_CANCEL = 2,
    SREM_MODEL_MIXED_FAIRNESS = 3,
} srem_model_workload_t;

typedef struct srem_model_config {
    srem_model_workload_t workload;
    srem_model_mode_t mode;
    size_t instance_count;
    size_t frame_bytes;
    unsigned tile_width;
    unsigned active_lanes;
    unsigned site_count;
    unsigned divergence_eighths;
    unsigned vector_threshold;
    unsigned remote_producers;
    uint64_t seed;
} srem_model_config_t;

typedef struct srem_model_metrics {
    uint64_t completions;
    uint64_t claims;
    uint64_t stale_tickets;
    uint64_t duplicate_tickets;
    uint64_t queue_pushes;
    uint64_t queue_pops;
    uint64_t resume_calls;
    uint64_t tile_dispatches;
    uint64_t scalar_lanes;
    uint64_t vector_lanes;
    uint64_t vector_blocks;
    uint64_t forced_escapes;
    uint64_t remote_pushes;
    uint64_t fairness_samples;
    uint64_t fairness_p99_gap;
    uint64_t hot_allocations;
} srem_model_metrics_t;

typedef struct srem_model_batch srem_model_batch_t;

int srem_model_batch_create(const srem_model_config_t *config,
                            srem_model_batch_t **out_batch);
void srem_model_batch_destroy(srem_model_batch_t *batch);
int srem_model_batch_reset(srem_model_batch_t *batch);
int srem_model_batch_begin_measurement(srem_model_batch_t *batch);
int srem_model_run_round(srem_model_batch_t *batch,
                         srem_model_metrics_t *metrics);
bool srem_model_batch_equal(const srem_model_batch_t *lhs,
                            const srem_model_batch_t *rhs);
uint64_t srem_model_checksum(const srem_model_batch_t *batch);
const char *srem_model_mode_name(srem_model_mode_t mode);
const char *srem_model_workload_name(srem_model_workload_t workload);
int srem_model_parse_mode(const char *text, srem_model_mode_t *out);
int srem_model_parse_workload(const char *text,
                              srem_model_workload_t *out);
int srem_model_candidate_baseline(
    srem_model_mode_t candidate,
    srem_model_mode_t *out_baseline);

#endif
