// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_plan.h"

#include <errno.h>
#include <stdio.h>

static int create_connect_write_program(
    leir_phase0_opcode_t write_opcode,
    bool consume_connect_result,
    leir_phase0_program_t **out) {
    const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_CONNECT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = 3U,
        },
        {
            .opcode = (uint16_t)write_opcode,
            .fd_slot = 0U,
            .buffer_slot = 4U,
            .length_slot = consume_connect_result ? 3U : 5U,
            .result_slot = 6U,
            .on_success = 2U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
    };
    const leir_phase0_program_desc_t desc = {
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = sizeof(nodes) / sizeof(nodes[0]),
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };

    return leir_phase0_program_create(&desc, out);
}

static int create_exact_receive_send_program(
    leir_phase0_program_t **out) {
    const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    const leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_READ_EXACT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_WRITE_ALL,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 4U,
            .on_success = 2U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 4U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 4U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
    };
    const leir_phase0_program_desc_t desc = {
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = sizeof(nodes) / sizeof(nodes[0]),
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };

    return leir_phase0_program_create(&desc, out);
}

static int test_connect_write_forms_one_kernel_segment(void) {
    leir_phase0_program_t *program = NULL;
    leir_aot_plan_t plan;
    int failed;

    if (create_connect_write_program(
            LEIR_PHASE0_OP_WRITE, false, &program) != 0) {
        return 1;
    }
    failed = leir_aot_plan_compile(program, &plan) != 0 ||
             plan.segment_count != 1U ||
             plan.kernel_chain_edges != 1U ||
             plan.completion_barriers != 1U ||
             plan.graph_breaks != 0U ||
             plan.return_node != 2U ||
             plan.segments[0].first_node != 0U ||
             plan.segments[0].last_node != 1U ||
             plan.segments[0].continuation_node != 2U ||
             plan.segments[0].operation_count != 2U ||
             plan.segments[0].exit_class !=
                 LEIR_AOT_EDGE_COMPLETION_BARRIER;
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_exact_receive_send_retains_completion_barrier(void) {
    leir_phase0_program_t *program = NULL;
    leir_aot_plan_t plan;
    int failed;

    if (create_exact_receive_send_program(&program) != 0) {
        return 1;
    }
    failed = leir_aot_plan_compile(program, &plan) != 0 ||
             plan.segment_count != 2U ||
             plan.kernel_chain_edges != 0U ||
             plan.completion_barriers != 2U ||
             plan.segments[0].first_node != 0U ||
             plan.segments[0].last_node != 0U ||
             plan.segments[0].continuation_node != 1U ||
             plan.segments[1].first_node != 1U ||
             plan.segments[1].last_node != 1U ||
             plan.segments[1].continuation_node != 2U;
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_exact_write_and_result_dependency_are_barriers(void) {
    leir_phase0_program_t *program = NULL;
    leir_aot_plan_t plan;

    if (create_connect_write_program(
            LEIR_PHASE0_OP_WRITE_ALL, false, &program) != 0) {
        return 1;
    }
    if (leir_aot_plan_compile(program, &plan) != 0 ||
        plan.segment_count != 2U ||
        plan.kernel_chain_edges != 0U) {
        leir_phase0_program_destroy(program);
        return 1;
    }
    leir_phase0_program_destroy(program);
    program = NULL;
    if (create_connect_write_program(
            LEIR_PHASE0_OP_WRITE, true, &program) != 0) {
        return 1;
    }
    if (leir_aot_plan_compile(program, &plan) != 0 ||
        plan.segment_count != 2U ||
        plan.kernel_chain_edges != 0U) {
        leir_phase0_program_destroy(program);
        return 1;
    }
    leir_phase0_program_destroy(program);
    return 0;
}

int main(void) {
    if (test_connect_write_forms_one_kernel_segment() != 0) {
        fputs("test_connect_write_forms_one_kernel_segment failed\n",
              stderr);
        return 1;
    }
    if (test_exact_receive_send_retains_completion_barrier() != 0) {
        fputs("test_exact_receive_send_retains_completion_barrier failed\n",
              stderr);
        return 1;
    }
    if (test_exact_write_and_result_dependency_are_barriers() != 0) {
        fputs("test_exact_write_and_result_dependency_are_barriers failed\n",
              stderr);
        return 1;
    }
    puts("LEIR AOT semantic-barrier planner tests passed");
    return 0;
}
