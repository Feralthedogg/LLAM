#ifndef LLAM_EXPERIMENTS_LEIR_PHASE0_INTERNAL_H
#define LLAM_EXPERIMENTS_LEIR_PHASE0_INTERNAL_H

#include "leir_phase0.h"

#include <stdatomic.h>

typedef void *(*leir_phase0_calloc_fn)(size_t count, size_t size);

typedef enum leir_phase0_advance_result {
    LEIR_PHASE0_ADVANCE_TERMINAL = 0,
    LEIR_PHASE0_ADVANCE_NEEDS_BACKEND = 1,
    LEIR_PHASE0_ADVANCE_ERROR = 2,
} leir_phase0_advance_result_t;

struct leir_phase0_program {
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_slot_kind_t slot_kinds[LEIR_PHASE0_MAX_SLOTS];
    uint16_t entry_node;
    uint16_t node_count;
    uint16_t slot_count;
    uint16_t io_node_count;
};

struct leir_phase0_instance {
    const leir_phase0_program_t *program;
    leir_phase0_value_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_run_opts_t opts;
    leir_phase0_metrics_t metrics;
    _Atomic(llam_io_req_t *) req;
    _Atomic(llam_task_t *) task;
    atomic_uint cancel_requested;
    atomic_uint running;
    atomic_uint terminal;
    atomic_uint_fast64_t activation_generation;
    atomic_uint_fast64_t request_generation;
    uint16_t current_node;
    size_t node_progress;
    unsigned inline_left;
    int terminal_error;
};

int leir_phase0_program_create_with_allocator(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out,
    leir_phase0_calloc_fn calloc_fn);

#endif
