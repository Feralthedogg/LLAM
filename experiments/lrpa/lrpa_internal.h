/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LRPA_INTERNAL_H
#define LLAM_EXPERIMENTS_LRPA_INTERNAL_H

#include "lrpa.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HANDLE lrpa_platform_thread_t;
typedef CRITICAL_SECTION lrpa_platform_mutex_t;
typedef CONDITION_VARIABLE lrpa_platform_cond_t;
#else
#include <pthread.h>
typedef pthread_t lrpa_platform_thread_t;
typedef pthread_mutex_t lrpa_platform_mutex_t;
typedef pthread_cond_t lrpa_platform_cond_t;
#endif

typedef int (*lrpa_thread_fn)(void *argument);

typedef struct lrpa_platform_gate {
    lrpa_platform_mutex_t mutex;
    lrpa_platform_cond_t condition;
    bool open;
    bool aborted;
} lrpa_platform_gate_t;

typedef struct lrpa_platform_barrier {
    lrpa_platform_mutex_t mutex;
    lrpa_platform_cond_t condition;
    uint32_t participants;
    uint32_t arrivals;
    uint64_t generation;
    bool broken;
} lrpa_platform_barrier_t;

typedef struct lrpa_completion_cell {
    atomic_uint outcome;
    atomic_uint winner_count;
    atomic_uint terminal_count;
    atomic_uint live_nodes;
    atomic_uint stale_completion;
    atomic_uint payload_visible;
    atomic_ullong generation;
    uint32_t object_id;
} lrpa_completion_cell_t;

typedef struct lrpa_context lrpa_context_t;

typedef struct lrpa_actor {
    lrpa_context_t *context;
    uint32_t actor_id;
    uint32_t lane_id;
    uint32_t worker_id;
    bool started;
    bool joined;
} lrpa_actor_t;

typedef struct lrpa_run_options {
    uint32_t fail_setup_actor;
    uint32_t stall_actor;
} lrpa_run_options_t;

struct lrpa_context {
    lrpa_manifest_t manifest;
    lrpa_lane_t *lanes;
    lrpa_completion_cell_t *cells;
    lrpa_actor_t *actors;
    lrpa_platform_thread_t *threads;
    lrpa_trace_entry_t *trace_entries;
    lrpa_trace_t trace;
    lrpa_platform_gate_t launch_gate;
    lrpa_platform_barrier_t start_barrier;
    lrpa_platform_barrier_t finish_barrier;
    atomic_bool abort_requested;
    atomic_uint armed_actors;
    atomic_uint release_observed_armed;
    uint32_t actor_count;
    uint32_t active_cell_count;
    bool initialized;
    bool launch_gate_initialized;
    bool start_barrier_initialized;
    bool finish_barrier_initialized;
    lrpa_run_options_t options;
};

lrpa_status_t lrpa_context_init(lrpa_context_t *context,
                                const lrpa_manifest_t *manifest,
                                const lrpa_run_options_t *options);
lrpa_status_t lrpa_context_run(lrpa_context_t *context,
                               lrpa_result_t *result);
lrpa_status_t lrpa_context_reset(lrpa_context_t *context);
void lrpa_context_destroy(lrpa_context_t *context);

uint64_t lrpa_platform_monotonic_ns(void);
void lrpa_platform_yield(void);
void lrpa_platform_spin(uint32_t iterations);
lrpa_status_t lrpa_platform_thread_create(lrpa_platform_thread_t *thread,
                                          lrpa_thread_fn function,
                                          void *argument);
lrpa_status_t lrpa_platform_thread_join(lrpa_platform_thread_t thread);
lrpa_status_t lrpa_platform_gate_init(lrpa_platform_gate_t *gate);
void lrpa_platform_gate_open(lrpa_platform_gate_t *gate, bool aborted);
bool lrpa_platform_gate_wait(lrpa_platform_gate_t *gate);
void lrpa_platform_gate_destroy(lrpa_platform_gate_t *gate);
lrpa_status_t lrpa_platform_barrier_init(lrpa_platform_barrier_t *barrier,
                                         uint32_t participants);
bool lrpa_platform_barrier_wait(lrpa_platform_barrier_t *barrier,
                                uint64_t timeout_ns);
void lrpa_platform_barrier_break(lrpa_platform_barrier_t *barrier);
void lrpa_platform_barrier_destroy(lrpa_platform_barrier_t *barrier);

void lrpa_trace_event(lrpa_context_t *context, uint32_t lane,
                      uint32_t actor, uint32_t event_kind,
                      uint32_t object_id, uint64_t generation,
                      uint32_t state_before, uint32_t state_after,
                      int32_t result, uintptr_t debug_address);

void lrpa_select_prepare_round(lrpa_context_t *context, uint32_t round);
void lrpa_select_actor_step(lrpa_actor_t *actor, uint32_t round);
lrpa_status_t lrpa_select_verify_round(lrpa_context_t *context,
                                       uint32_t round,
                                       lrpa_failure_t *failure,
                                       lrpa_result_t *result);
void lrpa_select_drain_round(lrpa_context_t *context);

#endif
