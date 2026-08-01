/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LRPA_H
#define LLAM_EXPERIMENTS_LRPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LRPA_MANIFEST_VERSION 1U
#define LRPA_RESULT_VERSION 1U
#define LRPA_MAX_LANES 64U
#define LRPA_MAX_WORKERS 4U
#define LRPA_MAX_PERTURBATIONS 64U
#define LRPA_MAX_TRACE_CAPACITY 1048576U

typedef enum lrpa_status {
    LRPA_STATUS_OK = 0,
    LRPA_STATUS_INVALID_ARGUMENT,
    LRPA_STATUS_INVALID_VERSION,
    LRPA_STATUS_INVALID_DIMENSIONS,
    LRPA_STATUS_INVALID_ENUM,
    LRPA_STATUS_OVERFLOW,
    LRPA_STATUS_DUPLICATE_OBJECT,
    LRPA_STATUS_MALFORMED_PERTURBATION,
    LRPA_STATUS_FAULT_UNAVAILABLE,
    LRPA_STATUS_INVALID_TRANSITION,
    LRPA_STATUS_OUT_OF_MEMORY,
    LRPA_STATUS_PLATFORM_ERROR,
    LRPA_STATUS_TIMEOUT,
    LRPA_STATUS_ORACLE_FAILURE
} lrpa_status_t;

typedef enum lrpa_gadget {
    LRPA_GADGET_NONE = 0,
    LRPA_GADGET_SELECT_COMPLETION = 1,
    LRPA_GADGET_COUNT
} lrpa_gadget_t;

typedef enum lrpa_coupling {
    LRPA_COUPLING_INDEPENDENT = 0,
    LRPA_COUPLING_SHARED_SHARD,
    LRPA_COUPLING_SHARED_OBJECT,
    LRPA_COUPLING_RING,
    LRPA_COUPLING_BIPARTITE,
    LRPA_COUPLING_COLORED_GRAPH,
    LRPA_COUPLING_MIXED_BACKEND,
    LRPA_COUPLING_COUNT
} lrpa_coupling_t;

typedef enum lrpa_fault {
    LRPA_FAULT_NONE = 0,
    LRPA_FAULT_SELECT_SKIP_WINNER_CAS,
    LRPA_FAULT_STALE_GENERATION_REUSE,
    LRPA_FAULT_COUNT
} lrpa_fault_t;

typedef enum lrpa_step_kind {
    LRPA_STEP_YIELD = 0,
    LRPA_STEP_SPIN,
    LRPA_STEP_BARRIER,
    LRPA_STEP_TRIGGER,
    LRPA_STEP_CANCEL,
    LRPA_STEP_CLOSE,
    LRPA_STEP_TIMER_OFFSET,
    LRPA_STEP_HOST_WAKE,
    LRPA_STEP_REQUEST_STOP,
    LRPA_STEP_AFFINITY_ROTATE,
    LRPA_STEP_KIND_COUNT
} lrpa_step_kind_t;

typedef enum lrpa_lane_phase {
    LRPA_LANE_ALLOCATED = 0,
    LRPA_LANE_SETUP,
    LRPA_LANE_ARMED,
    LRPA_LANE_RELEASED,
    LRPA_LANE_RACING,
    LRPA_LANE_DRAINING,
    LRPA_LANE_VERIFIED,
    LRPA_LANE_DESTROYED,
    LRPA_LANE_PHASE_COUNT
} lrpa_lane_phase_t;

typedef enum lrpa_outcome {
    LRPA_OUTCOME_NONE = 0,
    LRPA_OUTCOME_SEND,
    LRPA_OUTCOME_CLOSE,
    LRPA_OUTCOME_CANCEL,
    LRPA_OUTCOME_TIMEOUT,
    LRPA_OUTCOME_COUNT
} lrpa_outcome_t;

enum {
    LRPA_ALLOW_SEND = 1U << LRPA_OUTCOME_SEND,
    LRPA_ALLOW_CLOSE = 1U << LRPA_OUTCOME_CLOSE,
    LRPA_ALLOW_CANCEL = 1U << LRPA_OUTCOME_CANCEL,
    LRPA_ALLOW_TIMEOUT = 1U << LRPA_OUTCOME_TIMEOUT,
    LRPA_ALLOW_ALL = LRPA_ALLOW_SEND | LRPA_ALLOW_CLOSE |
                     LRPA_ALLOW_CANCEL | LRPA_ALLOW_TIMEOUT
};

typedef enum lrpa_oracle_id {
    LRPA_ORACLE_NONE = 0,
    LRPA_ORACLE_EXACTLY_ONE_WINNER,
    LRPA_ORACLE_ALLOWED_OUTCOME,
    LRPA_ORACLE_PAYLOAD_OWNERSHIP,
    LRPA_ORACLE_LIVE_NODE_DRAIN,
    LRPA_ORACLE_STALE_GENERATION,
    LRPA_ORACLE_GLOBAL_ACCOUNTING,
    LRPA_ORACLE_ID_COUNT
} lrpa_oracle_id_t;

typedef struct lrpa_perturbation {
    lrpa_step_kind_t kind;
    uint64_t lane_mask;
    uint32_t sequence;
    int64_t value;
} lrpa_perturbation_t;

typedef struct lrpa_manifest {
    uint32_t version;
    lrpa_gadget_t gadget;
    lrpa_coupling_t coupling;
    uint32_t lane_count;
    uint32_t worker_count;
    uint32_t rounds;
    uint32_t flags;
    uint32_t queue_capacity;
    uint32_t perturbation_count;
    lrpa_fault_t fault_id;
    uint32_t allowed_outcomes;
    uint64_t seed;
    uint64_t timeout_ns;
    uint64_t perturbation_hash;
    uint32_t object_ids[LRPA_MAX_LANES];
    lrpa_perturbation_t perturbations[LRPA_MAX_PERTURBATIONS];
} lrpa_manifest_t;

typedef struct lrpa_lane {
    uint32_t id;
    atomic_uint phase;
    atomic_uint winner_count;
    atomic_uint terminal_count;
    atomic_uint invariant_failures;
    uint64_t local_seed;
    uint64_t trace_checksum;
    void *gadget_state;
} lrpa_lane_t;

typedef struct lrpa_trace_entry {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint32_t lane;
    uint32_t actor;
    uint32_t event_kind;
    uint32_t object_id;
    uint64_t generation;
    uint32_t state_before;
    uint32_t state_after;
    int32_t result;
    uintptr_t debug_address;
} lrpa_trace_entry_t;

typedef struct lrpa_trace {
    lrpa_trace_entry_t *entries;
    size_t capacity;
    atomic_size_t next;
    atomic_bool truncated;
} lrpa_trace_t;

typedef struct lrpa_failure {
    lrpa_oracle_id_t oracle;
    uint64_t expected;
    uint64_t actual;
    uint64_t generation_first;
    uint64_t generation_second;
    uint32_t object_id;
    uint64_t timestamp_ns;
    uintptr_t debug_address;
} lrpa_failure_t;

typedef struct lrpa_result {
    uint32_t version;
    lrpa_status_t status;
    uint64_t signature;
    uint64_t seed;
    uint64_t lane_executions;
    uint64_t elapsed_ns;
    uint32_t lane_count;
    uint32_t rounds_completed;
    uint32_t first_failure_round;
    uint32_t failures;
    uint64_t armed_total;
    uint64_t winner_total;
    uint64_t cancel_total;
    uint64_t timeout_total;
    uint64_t discard_total;
    size_t trace_entries;
    bool trace_truncated;
    bool cleanup_complete;
    lrpa_failure_t first_failure;
} lrpa_result_t;

lrpa_status_t lrpa_manifest_validate(const lrpa_manifest_t *manifest);
lrpa_status_t lrpa_manifest_generate_perturbations(lrpa_manifest_t *manifest);
uint64_t lrpa_perturbation_hash(const lrpa_perturbation_t *steps,
                                size_t count);
bool lrpa_checked_multiply_size(size_t left, size_t right,
                                size_t *result_out);

void lrpa_lane_init(lrpa_lane_t *lane, uint32_t id, uint64_t seed);
lrpa_status_t lrpa_lane_transition(lrpa_lane_t *lane,
                                   lrpa_lane_phase_t expected,
                                   lrpa_lane_phase_t desired);

void lrpa_trace_bind(lrpa_trace_t *trace, lrpa_trace_entry_t *entries,
                     size_t capacity);
bool lrpa_trace_record(lrpa_trace_t *trace,
                       const lrpa_trace_entry_t *entry);

uint64_t lrpa_failure_signature(const lrpa_manifest_t *manifest,
                                const lrpa_failure_t *failure);

const char *lrpa_status_name(lrpa_status_t status);
const char *lrpa_coupling_name(lrpa_coupling_t coupling);
const char *lrpa_fault_name(lrpa_fault_t fault);

#ifdef __cplusplus
}
#endif

#endif
