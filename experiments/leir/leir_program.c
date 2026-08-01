// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_program.c
 * @brief Validated, immutable ownership boundary for LEIR phase-0 programs.
 *
 * @details
 * Creation copies node and slot descriptors before validation, so a successful
 * program does not borrow the descriptor arrays. The output stays @c NULL on
 * every failure and is published only after the owned copy is fully valid.
 */

#include "leir_phase0_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool slot_kind_is_valid(leir_phase0_slot_kind_t kind) {
    switch (kind) {
        case LEIR_PHASE0_SLOT_FD:
        case LEIR_PHASE0_SLOT_MUT_BUFFER:
        case LEIR_PHASE0_SLOT_CONST_BUFFER:
        case LEIR_PHASE0_SLOT_U64:
        case LEIR_PHASE0_SLOT_I64:
            return true;
    }
    return false;
}

static bool slot_is(const leir_phase0_program_t *program,
                    uint16_t slot,
                    leir_phase0_slot_kind_t kind) {
    return slot < program->slot_count &&
           program->slot_kinds[slot] == kind;
}

static bool edge_is_valid(const leir_phase0_program_t *program,
                          uint16_t edge) {
    return edge != LEIR_PHASE0_NODE_NONE &&
           edge < program->node_count;
}

static bool validate_io_node(const leir_phase0_program_t *program,
                             const leir_phase0_node_desc_t *node,
                             bool is_write) {
    bool buffer_is_valid;

    if (is_write) {
        buffer_is_valid =
            slot_is(program,
                    node->buffer_slot,
                    LEIR_PHASE0_SLOT_MUT_BUFFER) ||
            slot_is(program,
                    node->buffer_slot,
                    LEIR_PHASE0_SLOT_CONST_BUFFER);
    } else {
        buffer_is_valid =
            slot_is(program,
                    node->buffer_slot,
                    LEIR_PHASE0_SLOT_MUT_BUFFER);
    }

    return slot_is(program, node->fd_slot, LEIR_PHASE0_SLOT_FD) &&
           buffer_is_valid &&
           (slot_is(program,
                    node->length_slot,
                    LEIR_PHASE0_SLOT_U64) ||
            slot_is(program,
                    node->length_slot,
                    LEIR_PHASE0_SLOT_I64)) &&
           slot_is(program, node->result_slot, LEIR_PHASE0_SLOT_I64) &&
           edge_is_valid(program, node->on_success) &&
           edge_is_valid(program, node->on_eof) &&
           edge_is_valid(program, node->on_error);
}

static bool validate_connect_node(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node) {
    return slot_is(program, node->fd_slot, LEIR_PHASE0_SLOT_FD) &&
           slot_is(program,
                   node->buffer_slot,
                   LEIR_PHASE0_SLOT_CONST_BUFFER) &&
           slot_is(program, node->length_slot, LEIR_PHASE0_SLOT_U64) &&
           slot_is(program, node->result_slot, LEIR_PHASE0_SLOT_I64) &&
           edge_is_valid(program, node->on_success) &&
           node->on_eof == LEIR_PHASE0_NODE_NONE &&
           edge_is_valid(program, node->on_error);
}

static bool validate_terminal_node(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node) {
    return node->fd_slot == LEIR_PHASE0_NODE_NONE &&
           node->buffer_slot == LEIR_PHASE0_NODE_NONE &&
           node->length_slot == LEIR_PHASE0_NODE_NONE &&
           slot_is(program, node->result_slot, LEIR_PHASE0_SLOT_I64) &&
           node->on_success == LEIR_PHASE0_NODE_NONE &&
           node->on_eof == LEIR_PHASE0_NODE_NONE &&
           node->on_error == LEIR_PHASE0_NODE_NONE;
}

static bool validate_reachability(const leir_phase0_program_t *program) {
    bool visited[LEIR_PHASE0_MAX_NODES] = {false};
    uint16_t pending[LEIR_PHASE0_MAX_NODES];
    size_t pending_count = 0U;
    size_t i;

    visited[program->entry_node] = true;
    pending[pending_count++] = program->entry_node;
    while (pending_count > 0U) {
        uint16_t current = pending[--pending_count];
        const leir_phase0_node_desc_t *node = &program->nodes[current];
        const uint16_t edges[3] = {
            node->on_success,
            node->on_eof,
            node->on_error,
        };

        for (i = 0U; i < 3U; i += 1U) {
            uint16_t edge = edges[i];

            if (edge != LEIR_PHASE0_NODE_NONE && !visited[edge]) {
                visited[edge] = true;
                pending[pending_count++] = edge;
            }
        }
    }

    for (i = 0U; i < program->node_count; i += 1U) {
        if (!visited[i]) {
            return false;
        }
    }
    return true;
}

static bool validate_program(leir_phase0_program_t *program) {
    size_t i;

    for (i = 0U; i < program->slot_count; i += 1U) {
        if (!slot_kind_is_valid(program->slot_kinds[i])) {
            return false;
        }
    }

    for (i = 0U; i < program->node_count; i += 1U) {
        const leir_phase0_node_desc_t *node = &program->nodes[i];

        switch (node->opcode) {
            case LEIR_PHASE0_OP_READ:
            case LEIR_PHASE0_OP_READ_EXACT:
                if (!validate_io_node(program, node, false)) {
                    return false;
                }
                program->io_node_count += 1U;
                break;
            case LEIR_PHASE0_OP_WRITE:
            case LEIR_PHASE0_OP_WRITE_ALL:
                if (!validate_io_node(program, node, true)) {
                    return false;
                }
                program->io_node_count += 1U;
                break;
            case LEIR_PHASE0_OP_CONNECT:
                if (!validate_connect_node(program, node)) {
                    return false;
                }
                program->io_node_count += 1U;
                break;
            case LEIR_PHASE0_OP_RETURN:
            case LEIR_PHASE0_OP_FAIL:
                if (!validate_terminal_node(program, node)) {
                    return false;
                }
                break;
            default:
                return false;
        }
    }
    return validate_reachability(program);
}

int leir_phase0_program_create_with_allocator(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out,
    leir_phase0_calloc_fn calloc_fn) {
    leir_phase0_program_t *program;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (desc == NULL || desc->nodes == NULL ||
        desc->slot_kinds == NULL || calloc_fn == NULL ||
        desc->node_count == 0U ||
        desc->node_count > LEIR_PHASE0_MAX_NODES ||
        desc->slot_count == 0U ||
        desc->slot_count > LEIR_PHASE0_MAX_SLOTS ||
        desc->entry_node >= desc->node_count) {
        errno = EINVAL;
        return -1;
    }

    program = calloc_fn(1U, sizeof(*program));
    if (program == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(program->nodes,
           desc->nodes,
           desc->node_count * sizeof(program->nodes[0]));
    memcpy(program->slot_kinds,
           desc->slot_kinds,
           desc->slot_count * sizeof(program->slot_kinds[0]));
    program->entry_node = desc->entry_node;
    program->node_count = (uint16_t)desc->node_count;
    program->slot_count = (uint16_t)desc->slot_count;

    if (!validate_program(program)) {
        free(program);
        errno = EINVAL;
        return -1;
    }
    *out = program;
    return 0;
}

int leir_phase0_program_create(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out) {
    return leir_phase0_program_create_with_allocator(desc, out, calloc);
}

void leir_phase0_program_destroy(leir_phase0_program_t *program) {
    free(program);
}
