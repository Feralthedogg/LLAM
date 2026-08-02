// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/leir_bindings.c
 * @brief Activation binding validation for the LEIR phase-0 engine.
 */

#include "leir_phase0_internal.h"

#include <stdint.h>

#if LLAM_PLATFORM_WINDOWS
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

static bool fd_is_valid(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    return !LLAM_FD_IS_INVALID(fd);
#else
    return fd >= 0;
#endif
}

bool leir_phase0_bindings_are_valid(
    const leir_phase0_program_t *program,
    const leir_phase0_value_t *values) {
    size_t i;

    for (i = 0U; i < program->slot_count; i += 1U) {
        switch (program->slot_kinds[i]) {
            case LEIR_PHASE0_SLOT_FD:
                if (!fd_is_valid(values[i].fd)) {
                    return false;
                }
                break;
            case LEIR_PHASE0_SLOT_MUT_BUFFER:
            case LEIR_PHASE0_SLOT_CONST_BUFFER:
                if (values[i].buffer.data == NULL &&
                    values[i].buffer.size != 0U) {
                    return false;
                }
                break;
            case LEIR_PHASE0_SLOT_U64:
            case LEIR_PHASE0_SLOT_I64:
                break;
        }
    }

    for (i = 0U; i < program->node_count; i += 1U) {
        const leir_phase0_node_desc_t *node = &program->nodes[i];
        leir_phase0_slot_kind_t length_kind;
        uint64_t requested;
        size_t capacity;

        switch (node->opcode) {
            case LEIR_PHASE0_OP_READ:
            case LEIR_PHASE0_OP_READ_EXACT:
            case LEIR_PHASE0_OP_WRITE:
            case LEIR_PHASE0_OP_WRITE_ALL:
                length_kind = program->slot_kinds[node->length_slot];
                if (length_kind == LEIR_PHASE0_SLOT_I64) {
                    if (values[node->length_slot].i64 < 0) {
                        break;
                    }
                    requested =
                        (uint64_t)values[node->length_slot].i64;
                } else {
                    requested = values[node->length_slot].u64;
                }
                capacity = values[node->buffer_slot].buffer.size;
                if (requested > SIZE_MAX ||
                    (size_t)requested > capacity ||
                    (requested != 0U &&
                     values[node->buffer_slot].buffer.data == NULL)) {
                    return false;
                }
                break;
            case LEIR_PHASE0_OP_CONNECT:
                requested = values[node->length_slot].u64;
                capacity = values[node->buffer_slot].buffer.size;
                if (requested == 0U ||
                    requested > (uint64_t)sizeof(struct sockaddr_storage) ||
                    requested > (uint64_t)capacity ||
                    values[node->buffer_slot].buffer.data == NULL) {
                    return false;
                }
                break;
            case LEIR_PHASE0_OP_RETURN:
            case LEIR_PHASE0_OP_FAIL:
                break;
            default:
                return false;
        }
    }
    return true;
}
