// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_PLAN_H
#define LLAM_EXPERIMENTS_LEIR_AOT_PLAN_H

#include "leir_phase0.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEIR_AOT_MAX_SEGMENTS LEIR_PHASE0_MAX_NODES

typedef enum leir_aot_edge_class {
    LEIR_AOT_EDGE_KERNEL_CHAIN = 0,
    LEIR_AOT_EDGE_COMPLETION_BARRIER = 1,
    LEIR_AOT_EDGE_GRAPH_BREAK = 2,
} leir_aot_edge_class_t;

typedef struct leir_aot_segment_desc {
    uint16_t first_node;
    uint16_t last_node;
    uint16_t continuation_node;
    uint16_t operation_count;
    uint16_t exit_class;
} leir_aot_segment_desc_t;

typedef struct leir_aot_plan {
    leir_aot_segment_desc_t segments[LEIR_AOT_MAX_SEGMENTS];
    uint16_t segment_count;
    uint16_t return_node;
    uint16_t kernel_chain_edges;
    uint16_t completion_barriers;
    uint16_t graph_breaks;
} leir_aot_plan_t;

int leir_aot_plan_compile(
    const leir_phase0_program_t *program,
    leir_aot_plan_t *out);

#ifdef __cplusplus
}
#endif

#endif
