#include "leir_native_plan.h"

#include "leir_phase0_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

static int fail_plan(void) {
    errno = EINVAL;
    return -1;
}

static bool operation_count_is_supported(unsigned count) {
    return count == 1U || count == 2U ||
           count == 4U || count == 8U;
}

static bool node_is_terminal_fail(
    const leir_phase0_program_t *program,
    uint16_t index) {
    return index < program->node_count &&
           program->nodes[index].opcode == LEIR_PHASE0_OP_FAIL;
}

static bool slot_was_prior_result(
    const bool prior_results[LEIR_PHASE0_MAX_SLOTS],
    uint16_t slot) {
    return slot < LEIR_PHASE0_MAX_SLOTS &&
           prior_results[slot];
}

int leir_native_plan_compile(
    const leir_phase0_program_t *program,
    leir_native_plan_t *out) {
    leir_native_plan_t plan;
    bool visited[LEIR_PHASE0_MAX_NODES] = {false};
    bool prior_results[LEIR_PHASE0_MAX_SLOTS] = {false};
    uint16_t current;

    if (program == NULL || out == NULL ||
        program->node_count == 0U ||
        program->node_count > LEIR_PHASE0_MAX_NODES ||
        program->slot_count == 0U ||
        program->slot_count > LEIR_PHASE0_MAX_SLOTS ||
        program->entry_node >= program->node_count) {
        return fail_plan();
    }

    memset(&plan, 0, sizeof(plan));
    current = program->entry_node;
    for (;;) {
        const leir_phase0_node_desc_t *node;
        leir_native_step_t *step;
        leir_native_step_kind_t kind;

        if (current >= program->node_count || visited[current]) {
            return fail_plan();
        }
        visited[current] = true;
        node = &program->nodes[current];

        if (node->opcode == LEIR_PHASE0_OP_RETURN) {
            if (!operation_count_is_supported(plan.step_count) ||
                plan.step_count == 0U ||
                node->result_slot !=
                    plan.steps[plan.step_count - 1U].result_slot) {
                return fail_plan();
            }
            plan.return_node = current;
            plan.result_slot = node->result_slot;
            *out = plan;
            return 0;
        }
        if (node->opcode == LEIR_PHASE0_OP_READ_EXACT) {
            kind = LEIR_NATIVE_STEP_RECV;
        } else if (node->opcode == LEIR_PHASE0_OP_WRITE_ALL) {
            kind = LEIR_NATIVE_STEP_SEND;
        } else {
            return fail_plan();
        }

        if (plan.step_count >= LEIR_NATIVE_MAX_OPS ||
            !node_is_terminal_fail(program, node->on_eof) ||
            !node_is_terminal_fail(program, node->on_error) ||
            slot_was_prior_result(
                prior_results, node->fd_slot) ||
            slot_was_prior_result(
                prior_results, node->buffer_slot) ||
            slot_was_prior_result(
                prior_results, node->length_slot) ||
            node->result_slot >= program->slot_count ||
            node->on_success >= program->node_count) {
            return fail_plan();
        }

        step = &plan.steps[plan.step_count];
        step->kind = (uint16_t)kind;
        step->fd_slot = node->fd_slot;
        step->buffer_slot = node->buffer_slot;
        step->length_slot = node->length_slot;
        step->result_slot = node->result_slot;
        prior_results[node->result_slot] = true;
        plan.step_count += 1U;
        current = node->on_success;
    }
}
