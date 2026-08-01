// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_LCCF_MODEL_H
#define LLAM_EXPERIMENTS_LCCF_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCCF_MODEL_MAX_SITES 8U
#define LCCF_MODEL_MIN_FRAME_BYTES 64U
#define LCCF_MODEL_MAX_FRAME_BYTES 256U
#define LCCF_MODEL_MIN_CELL_BYTES 64U
#define LCCF_MODEL_MAX_CELL_BYTES 128U
#define LCCF_MODEL_MAX_CHAIN_LENGTH 1024U
#define LCCF_MODEL_MAX_DIRECT_BUDGET 1024U

typedef enum lccf_model_mode {
    LCCF_MODEL_WAKER_QUEUE = 0,
    LCCF_MODEL_CAUSAL_CELL_QUEUE = 1,
    LCCF_MODEL_FUSED_CAUSAL_CELL = 2,
    LCCF_MODEL_BUDGETED_FUSED_CHAIN = 3,
    LCCF_MODEL_REMOTE_WAKER_QUEUE = 4,
    LCCF_MODEL_REMOTE_CAUSAL_CELL = 5,
    LCCF_MODEL_RECOMPUTE_QUEUE = 6,
    LCCF_MODEL_SHARED_FACT_QUEUE = 7,
    LCCF_MODEL_RECOMPUTE_FUSED = 8,
    LCCF_MODEL_SHARED_FACT_FUSED = 9,
    LCCF_MODEL_MIXED_RECOMPUTE = 10,
    LCCF_MODEL_MIXED_SHARED_FACT = 11,
    LCCF_MODEL_SHARED_EVENT_QUEUE = 12,
    LCCF_MODEL_SHARED_EVENT_FUSED = 13,
    LCCF_MODEL_MIXED_SHARED_EVENT = 14,
    LCCF_MODEL_MODE_COUNT,
} lccf_model_mode_t;

typedef enum lccf_model_workload {
    LCCF_MODEL_COMPLETION_IO_PIPELINE = 0,
    LCCF_MODEL_COMPLETION_RPC_STATE = 1,
    LCCF_MODEL_COMPLETION_TIMER_CANCEL = 2,
    LCCF_MODEL_COMPLETION_MIXED_FAIRNESS = 3,
} lccf_model_workload_t;

typedef enum lccf_model_command_kind {
    LCCF_MODEL_COMMAND_CONTINUE = 1,
    LCCF_MODEL_COMMAND_WAIT_IO = 2,
    LCCF_MODEL_COMMAND_WAIT_TIMER = 3,
    LCCF_MODEL_COMMAND_YIELD = 4,
    LCCF_MODEL_COMMAND_COMPLETE = 5,
    LCCF_MODEL_COMMAND_FAIL = 6,
} lccf_model_command_kind_t;

typedef struct lccf_model_config {
    lccf_model_workload_t workload;
    lccf_model_mode_t mode;
    size_t instance_count;
    size_t frame_bytes;
    size_t cell_bytes;
    unsigned site_count;
    unsigned direct_budget;
    unsigned chain_length;
    unsigned remote_producers;
    unsigned callback_failure_step;
    size_t fact_queue_capacity;
    uint64_t seed;
} lccf_model_config_t;

typedef struct lccf_model_trace_row {
    uint64_t generation;
    uint64_t callback_ordinal;
    uint64_t event_word0;
    uint64_t event_word1;
    uint64_t command_output;
    uint32_t instance_index;
    uint32_t site_index;
    uint32_t event_kind;
    int32_t error_code;
    uint32_t command_next_site;
    uint32_t command_kind;
} lccf_model_trace_row_t;

typedef struct lccf_model_metrics {
    uint64_t completions;
    uint64_t claims;
    uint64_t stale_tickets;
    uint64_t queue_pushes;
    uint64_t queue_pops;
    uint64_t resume_calls;
    uint64_t direct_calls;
    uint64_t forced_escapes;
    uint64_t remote_pushes;
    uint64_t fairness_samples;
    uint64_t fairness_p99_ns;
    uint64_t hot_allocations;
    uint64_t facts_attempted;
    uint64_t facts_built;
    uint64_t facts_build_failed;
    uint64_t fact_normalizations;
    uint64_t fact_site_lookups;
    uint64_t fact_module_pins;
    uint64_t fact_payload_pins;
    uint64_t fact_stale_losers;
    uint64_t fact_guard_rechecks;
    uint64_t fact_queue_forwards;
    uint64_t fact_generation_mismatches;
    uint64_t fact_reuse_delays;
    uint64_t fact_hot_bytes;
    uint64_t fact_sidecar_bytes;
    uint64_t fact_overflow_pushes;
    uint64_t fact_overflow_pops;
} lccf_model_metrics_t;

typedef struct lccf_model_batch lccf_model_batch_t;

int lccf_model_batch_create(const lccf_model_config_t *config,
                            lccf_model_batch_t **out_batch);
void lccf_model_batch_destroy(lccf_model_batch_t *batch);
int lccf_model_batch_reset(lccf_model_batch_t *batch);
int lccf_model_run_round(lccf_model_batch_t *batch,
                         lccf_model_metrics_t *metrics);
bool lccf_model_batch_equal(const lccf_model_batch_t *lhs,
                            const lccf_model_batch_t *rhs);
uint64_t lccf_model_checksum(const lccf_model_batch_t *batch);
bool lccf_model_fact_references_balanced(
    const lccf_model_batch_t *batch);
int lccf_model_set_trace_buffer(lccf_model_batch_t *batch,
                                lccf_model_trace_row_t *rows,
                                size_t capacity);
size_t lccf_model_trace_count(const lccf_model_batch_t *batch);
const char *lccf_model_mode_name(lccf_model_mode_t mode);
const char *lccf_model_workload_name(lccf_model_workload_t workload);
int lccf_model_parse_mode(const char *text, lccf_model_mode_t *out);
int lccf_model_parse_workload(const char *text,
                              lccf_model_workload_t *out);
int lccf_model_candidate_baseline(
    lccf_model_mode_t candidate,
    lccf_model_mode_t *out_baseline);

#endif
