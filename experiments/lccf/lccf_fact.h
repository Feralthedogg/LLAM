/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LCCF_FACT_H
#define LLAM_EXPERIMENTS_LCCF_FACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCCF_FACT_VERSION 1U
#define LCCF_FACT_MAX_GENERATION (UINT64_MAX >> 4U)
#ifdef ESTALE
#define LCCF_FACT_ESTALE ESTALE
#else
#define LCCF_FACT_ESTALE 2001
#endif

typedef enum lccf_fact_layout {
    LCCF_FACT_LAYOUT_SPLIT64_64 = 0,
    LCCF_FACT_LAYOUT_SPLIT96_64,
    LCCF_FACT_LAYOUT_UNIFIED128,
    LCCF_FACT_LAYOUT_COUNT
} lccf_fact_layout_t;

typedef enum lccf_fact_state {
    LCCF_FACT_STATE_ARMED = 1,
    LCCF_FACT_STATE_BUILDING,
    LCCF_FACT_STATE_READY,
    LCCF_FACT_STATE_RUNNING_DIRECT,
    LCCF_FACT_STATE_QUEUED,
    LCCF_FACT_STATE_RUNNING_QUEUED,
    LCCF_FACT_STATE_TERMINAL,
    LCCF_FACT_STATE_COUNT
} lccf_fact_state_t;

typedef enum lccf_fact_source {
    LCCF_FACT_SOURCE_LINUX_CQE = 0,
    LCCF_FACT_SOURCE_KQUEUE,
    LCCF_FACT_SOURCE_IOCP,
    LCCF_FACT_SOURCE_TIMER,
    LCCF_FACT_SOURCE_CANCEL,
    LCCF_FACT_SOURCE_EXTERNAL,
    LCCF_FACT_SOURCE_STOP,
    LCCF_FACT_SOURCE_COUNT
} lccf_fact_source_t;

typedef enum lccf_fact_event_kind {
    LCCF_FACT_EVENT_IO = 1,
    LCCF_FACT_EVENT_TIMER,
    LCCF_FACT_EVENT_CANCEL,
    LCCF_FACT_EVENT_EXTERNAL,
    LCCF_FACT_EVENT_STOP,
    LCCF_FACT_EVENT_FAIL,
    LCCF_FACT_EVENT_COUNT
} lccf_fact_event_kind_t;

typedef enum lccf_fact_consumer {
    LCCF_FACT_CONSUMER_DIRECT = 0,
    LCCF_FACT_CONSUMER_QUEUE
} lccf_fact_consumer_t;

typedef enum lccf_fact_route {
    LCCF_FACT_ROUTE_DIRECT = 0,
    LCCF_FACT_ROUTE_QUEUE,
    LCCF_FACT_ROUTE_FORWARD,
    LCCF_FACT_ROUTE_DEFER
} lccf_fact_route_t;

typedef enum lccf_fact_ref_kind {
    LCCF_FACT_REF_CALLBACK = 0,
    LCCF_FACT_REF_QUEUE,
    LCCF_FACT_REF_BACKEND,
    LCCF_FACT_REF_EXTERNAL,
    LCCF_FACT_REF_PAYLOAD,
    LCCF_FACT_REF_MODULE,
    LCCF_FACT_REF_COUNT
} lccf_fact_ref_kind_t;

enum {
    LCCF_FACT_RAW_KQUEUE_ERROR = 1U << 0,
    LCCF_FACT_RAW_IOCP_SUCCESS = 1U << 1,
    LCCF_FACT_TICKET_MALFORMED = 1U << 8,
    LCCF_FACT_TICKET_INVALID_SITE = 1U << 9,
    LCCF_FACT_TICKET_FAIL_MODULE_PIN = 1U << 10,
    LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN = 1U << 11
};

enum {
    LCCF_FACT_GUARD_DIRECT_ENABLED = UINT64_C(1) << 0,
    LCCF_FACT_GUARD_MODULE_ENABLED = UINT64_C(1) << 1,
    LCCF_FACT_GUARD_BACKEND_CAPABLE = UINT64_C(1) << 2,
    LCCF_FACT_GUARD_STOP = UINT64_C(1) << 3,
    LCCF_FACT_GUARD_FAIRNESS_DUE = UINT64_C(1) << 4,
    LCCF_FACT_GUARD_TRACE = UINT64_C(1) << 5,
    LCCF_FACT_GUARD_SHARD_PAUSED = UINT64_C(1) << 6,
    LCCF_FACT_GUARD_SHARD_OFFLINE = UINT64_C(1) << 7,
    LCCF_FACT_GUARD_CALLBACK_ACTIVE = UINT64_C(1) << 8,
    LCCF_FACT_GUARD_QUEUE_PRESSURE = UINT64_C(1) << 9,
    LCCF_FACT_GUARD_MIGRATING = UINT64_C(1) << 10
};

enum {
    LCCF_FACT_ESCAPE_WRONG_SHARD = UINT64_C(1) << 0,
    LCCF_FACT_ESCAPE_BUDGET = UINT64_C(1) << 1,
    LCCF_FACT_ESCAPE_FAIRNESS = UINT64_C(1) << 2,
    LCCF_FACT_ESCAPE_TRACE = UINT64_C(1) << 3,
    LCCF_FACT_ESCAPE_STOP = UINT64_C(1) << 4,
    LCCF_FACT_ESCAPE_MIGRATION = UINT64_C(1) << 5,
    LCCF_FACT_ESCAPE_MODULE_POLICY = UINT64_C(1) << 6,
    LCCF_FACT_ESCAPE_BACKEND_CAPABILITY = UINT64_C(1) << 7,
    LCCF_FACT_ESCAPE_CALLBACK_ACTIVE = UINT64_C(1) << 8,
    LCCF_FACT_ESCAPE_QUEUE_PRESSURE = UINT64_C(1) << 9,
    LCCF_FACT_ESCAPE_SHARD_STATE = UINT64_C(1) << 10
};

typedef struct lccf_fact_core {
    uint64_t generation;
    uint64_t fact_id;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    int32_t error_code;
    uint32_t event_kind;
    uint32_t source_kind;
    uint32_t captured_home_shard;
    uint32_t source_node;
    uint32_t site_index;
} lccf_fact_core_t;

_Static_assert(sizeof(lccf_fact_core_t) == 64U,
               "LCCF fact core must occupy 64 bytes");

typedef struct lccf_fact_ticket {
    uint64_t generation;
    int64_t raw_result;
    uint64_t raw_aux;
    uint64_t payload_word;
    uint64_t stable_flags;
    uint32_t raw_flags;
    int32_t raw_error;
    uint32_t event_kind;
    uint32_t source_kind;
    uint32_t captured_home_shard;
    uint32_t source_node;
    uint32_t site_index;
    uint32_t site_count;
} lccf_fact_ticket_t;

typedef struct lccf_fact_guard {
    uint64_t flags;
    uint64_t budget_remaining;
    uint32_t current_home_shard;
    uint32_t consuming_shard;
} lccf_fact_guard_t;

typedef struct lccf_fact_decision {
    lccf_fact_route_t route;
    uint64_t escape_reasons;
    uint32_t destination_shard;
} lccf_fact_decision_t;

typedef struct lccf_fact_counters {
    uint64_t claim_attempts;
    uint64_t fact_builds;
    uint64_t fact_build_failures;
    uint64_t normalization_calls;
    uint64_t site_lookups;
    uint64_t module_pins;
    uint64_t payload_pins;
    uint64_t stale_losers;
    uint64_t guard_rechecks;
    uint64_t queue_forwards;
    uint64_t generation_mismatches;
    uint64_t reuse_delays;
} lccf_fact_counters_t;

typedef struct lccf_fact_split64_64_layout {
    uint64_t hot_words[8];
    uint64_t sidecar_words[8];
} lccf_fact_split64_64_layout_t;

typedef struct lccf_fact_split96_64_layout {
    uint64_t hot_words[12];
    uint64_t sidecar_words[8];
} lccf_fact_split96_64_layout_t;

typedef struct lccf_fact_unified128_layout {
    uint64_t words[16];
} lccf_fact_unified128_layout_t;

typedef struct lccf_fact_cell {
    _Atomic uint64_t state_generation;
    _Atomic uint32_t references[LCCF_FACT_REF_COUNT];
    lccf_fact_core_t fact;
    lccf_fact_ticket_t raw_ticket;
    lccf_fact_layout_t layout;
    uint32_t ticket_count;
    bool shared;
    bool published;
} lccf_fact_cell_t;

uint64_t lccf_fact_pack_state(uint64_t generation,
                              lccf_fact_state_t state);
uint64_t lccf_fact_unpack_generation(uint64_t word);
lccf_fact_state_t lccf_fact_unpack_state(uint64_t word);

int lccf_fact_cell_init(lccf_fact_cell_t *cell, uint64_t generation,
                        lccf_fact_layout_t layout,
                        uint32_t ticket_count);
size_t lccf_fact_layout_hot_bytes(lccf_fact_layout_t layout);
size_t lccf_fact_layout_sidecar_bytes(lccf_fact_layout_t layout);
int lccf_fact_cell_arm(lccf_fact_cell_t *cell, uint64_t generation,
                       uint32_t ticket_count);
int lccf_fact_ticket_from_logical(
    lccf_fact_source_t source, lccf_fact_event_kind_t event_kind,
    uint64_t generation, int64_t result, int32_t error_code,
    uint64_t payload_word, uint32_t site_index, uint32_t site_count,
    uint32_t captured_home_shard, uint32_t source_node,
    lccf_fact_ticket_t *out_ticket);
int lccf_fact_normalize(const lccf_fact_ticket_t *ticket,
                        uint64_t fact_id,
                        lccf_fact_core_t *out_fact);
int lccf_fact_try_publish(lccf_fact_cell_t *cell,
                          const lccf_fact_ticket_t *ticket,
                          bool shared,
                          lccf_fact_counters_t *counters,
                          bool *out_won);
int lccf_fact_acquire(const lccf_fact_cell_t *cell,
                      uint64_t generation,
                      lccf_fact_core_t *out_fact);
int lccf_fact_materialize(const lccf_fact_cell_t *cell,
                          uint64_t generation,
                          lccf_fact_counters_t *counters,
                          lccf_fact_core_t *out_fact);
int lccf_fact_consume(lccf_fact_cell_t *cell, uint64_t generation,
                      lccf_fact_consumer_t consumer,
                      const lccf_fact_guard_t *guard,
                      lccf_fact_counters_t *counters,
                      lccf_fact_decision_t *out_decision,
                      lccf_fact_core_t *out_fact);
int lccf_fact_yield_to_queue(lccf_fact_cell_t *cell,
                             uint64_t generation);
int lccf_fact_finish(lccf_fact_cell_t *cell, uint64_t generation,
                     bool terminal, uint64_t next_generation,
                     lccf_fact_counters_t *counters);
int lccf_fact_retain(lccf_fact_cell_t *cell,
                     lccf_fact_ref_kind_t kind);
int lccf_fact_release(lccf_fact_cell_t *cell,
                      lccf_fact_ref_kind_t kind);
bool lccf_fact_can_reuse(const lccf_fact_cell_t *cell);
int lccf_fact_module_unregister(const lccf_fact_cell_t *cell);

#ifdef __cplusplus
}
#endif

#endif
