// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_aot_plan.c
 * @brief Partition a validated LEIR success path at semantic barriers.
 */

#include "leir_aot_plan.h"

#include "leir_phase0_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

static int fail_plan(void) {
    errno = EINVAL;
    return -1;
}

static bool node_is_effect(const leir_phase0_node_desc_t *node) {
    switch (node->opcode) {
        case LEIR_PHASE0_OP_READ:
        case LEIR_PHASE0_OP_READ_EXACT:
        case LEIR_PHASE0_OP_WRITE:
        case LEIR_PHASE0_OP_WRITE_ALL:
        case LEIR_PHASE0_OP_CONNECT:
            return true;
        default:
            return false;
    }
}

static bool node_is_terminal_fail(
    const leir_phase0_program_t *program,
    uint16_t node_index) {
    return node_index < program->node_count &&
           program->nodes[node_index].opcode == LEIR_PHASE0_OP_FAIL;
}

static bool node_is_terminal_return(
    const leir_phase0_program_t *program,
    uint16_t node_index) {
    return node_index < program->node_count &&
           program->nodes[node_index].opcode == LEIR_PHASE0_OP_RETURN;
}

static bool effect_branches_are_bounded(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node) {
    if (!node_is_terminal_fail(program, node->on_error)) {
        return false;
    }
    if (node->opcode == LEIR_PHASE0_OP_CONNECT) {
        return node->on_eof == LEIR_PHASE0_NODE_NONE;
    }
    return node_is_terminal_fail(program, node->on_eof);
}

static bool successor_consumes_result(
    const leir_phase0_node_desc_t *node,
    const leir_phase0_node_desc_t *successor) {
    return node->result_slot == successor->fd_slot ||
           node->result_slot == successor->buffer_slot ||
           node->result_slot == successor->length_slot;
}

static leir_aot_edge_class_t classify_success_edge(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node,
    const leir_phase0_node_desc_t *successor) {
    if (node->opcode == LEIR_PHASE0_OP_CONNECT &&
        successor->opcode == LEIR_PHASE0_OP_WRITE &&
        !successor_consumes_result(node, successor) &&
        node_is_terminal_return(program, successor->on_success)) {
        return LEIR_AOT_EDGE_KERNEL_CHAIN;
    }
    return LEIR_AOT_EDGE_COMPLETION_BARRIER;
}

static int append_segment(
    leir_aot_plan_t *plan,
    uint16_t first_node,
    uint16_t last_node,
    uint16_t continuation_node,
    uint16_t operation_count,
    leir_aot_edge_class_t exit_class) {
    leir_aot_segment_desc_t *segment;

    if (plan->segment_count >= LEIR_AOT_MAX_SEGMENTS ||
        operation_count == 0U) {
        return fail_plan();
    }
    segment = &plan->segments[plan->segment_count];
    segment->first_node = first_node;
    segment->last_node = last_node;
    segment->continuation_node = continuation_node;
    segment->operation_count = operation_count;
    segment->exit_class = (uint16_t)exit_class;
    plan->segment_count += 1U;
    if (exit_class == LEIR_AOT_EDGE_COMPLETION_BARRIER) {
        plan->completion_barriers += 1U;
    } else if (exit_class == LEIR_AOT_EDGE_GRAPH_BREAK) {
        plan->graph_breaks += 1U;
    }
    return 0;
}

int leir_aot_plan_compile(
    const leir_phase0_program_t *program,
    leir_aot_plan_t *out) {
    leir_aot_plan_t plan;
    bool visited[LEIR_PHASE0_MAX_NODES] = {false};
    uint16_t first_node;
    uint16_t current;
    uint16_t operation_count = 0U;

    if (program == NULL || out == NULL ||
        program->node_count == 0U ||
        program->node_count > LEIR_PHASE0_MAX_NODES ||
        program->entry_node >= program->node_count) {
        return fail_plan();
    }

    memset(&plan, 0, sizeof(plan));
    first_node = program->entry_node;
    current = program->entry_node;
    for (;;) {
        const leir_phase0_node_desc_t *node;
        const leir_phase0_node_desc_t *successor;
        uint16_t next;
        leir_aot_edge_class_t edge_class;

        if (current >= program->node_count || visited[current]) {
            return fail_plan();
        }
        visited[current] = true;
        node = &program->nodes[current];
        if (node->opcode == LEIR_PHASE0_OP_RETURN) {
            if (operation_count != 0U || plan.segment_count == 0U) {
                return fail_plan();
            }
            plan.return_node = current;
            *out = plan;
            return 0;
        }
        if (!node_is_effect(node) ||
            !effect_branches_are_bounded(program, node)) {
            return fail_plan();
        }
        operation_count += 1U;
        next = node->on_success;
        if (next >= program->node_count) {
            return fail_plan();
        }
        successor = &program->nodes[next];
        if (successor->opcode == LEIR_PHASE0_OP_RETURN) {
            if (append_segment(
                    &plan,
                    first_node,
                    current,
                    next,
                    operation_count,
                    LEIR_AOT_EDGE_COMPLETION_BARRIER) != 0) {
                return -1;
            }
            operation_count = 0U;
            current = next;
            continue;
        }
        if (!node_is_effect(successor)) {
            return fail_plan();
        }
        edge_class = classify_success_edge(program, node, successor);
        if (edge_class == LEIR_AOT_EDGE_KERNEL_CHAIN) {
            plan.kernel_chain_edges += 1U;
            current = next;
            continue;
        }
        if (append_segment(
                &plan,
                first_node,
                current,
                next,
                operation_count,
                edge_class) != 0) {
            return -1;
        }
        first_node = next;
        operation_count = 0U;
        current = next;
    }
}
