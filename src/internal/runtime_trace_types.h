// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file src/internal/runtime_trace_types.h
 * @brief Private scheduler trace-event storage layout.
 */

#ifndef LLAM_RUNTIME_TRACE_TYPES_H
#define LLAM_RUNTIME_TRACE_TYPES_H

/** @brief Compact scheduler trace event stored in a per-shard ring buffer. */
typedef struct llam_trace_event {
    atomic_uint_fast64_t ts_ns;
    atomic_uint_fast64_t task_id;
    atomic_uint kind;
    atomic_uint from_state;
    atomic_uint to_state;
    atomic_uint reason;
    atomic_uint shard;
} llam_trace_event_t;

#endif
