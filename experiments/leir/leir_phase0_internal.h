#ifndef LLAM_EXPERIMENTS_LEIR_PHASE0_INTERNAL_H
#define LLAM_EXPERIMENTS_LEIR_PHASE0_INTERNAL_H

#include "leir_phase0.h"

typedef void *(*leir_phase0_calloc_fn)(size_t count, size_t size);

struct leir_phase0_program {
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_slot_kind_t slot_kinds[LEIR_PHASE0_MAX_SLOTS];
    uint16_t entry_node;
    uint16_t node_count;
    uint16_t slot_count;
    uint16_t io_node_count;
};

int leir_phase0_program_create_with_allocator(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out,
    leir_phase0_calloc_fn calloc_fn);

#endif
