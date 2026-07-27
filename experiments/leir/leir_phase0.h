#ifndef LLAM_EXPERIMENTS_LEIR_PHASE0_H
#define LLAM_EXPERIMENTS_LEIR_PHASE0_H

#include "runtime_internal.h"

#include <stddef.h>
#include <stdint.h>

#define LEIR_PHASE0_MAX_NODES 32U
#define LEIR_PHASE0_MAX_SLOTS 16U
#define LEIR_PHASE0_NODE_NONE UINT16_MAX

typedef enum leir_phase0_opcode {
    LEIR_PHASE0_OP_READ = 0,
    LEIR_PHASE0_OP_READ_EXACT = 1,
    LEIR_PHASE0_OP_WRITE = 2,
    LEIR_PHASE0_OP_WRITE_ALL = 3,
    LEIR_PHASE0_OP_RETURN = 4,
    LEIR_PHASE0_OP_FAIL = 5,
} leir_phase0_opcode_t;

typedef enum leir_phase0_slot_kind {
    LEIR_PHASE0_SLOT_FD = 0,
    LEIR_PHASE0_SLOT_MUT_BUFFER = 1,
    LEIR_PHASE0_SLOT_CONST_BUFFER = 2,
    LEIR_PHASE0_SLOT_U64 = 3,
    LEIR_PHASE0_SLOT_I64 = 4,
} leir_phase0_slot_kind_t;

typedef struct leir_phase0_buffer {
    void *data;
    size_t size;
} leir_phase0_buffer_t;

typedef union leir_phase0_value {
    llam_fd_t fd;
    leir_phase0_buffer_t buffer;
    uint64_t u64;
    int64_t i64;
} leir_phase0_value_t;

typedef struct leir_phase0_node_desc {
    uint16_t opcode;
    uint16_t fd_slot;
    uint16_t buffer_slot;
    uint16_t length_slot;
    uint16_t result_slot;
    uint16_t on_success;
    uint16_t on_eof;
    uint16_t on_error;
} leir_phase0_node_desc_t;

typedef struct leir_phase0_program_desc {
    const leir_phase0_node_desc_t *nodes;
    const leir_phase0_slot_kind_t *slot_kinds;
    size_t node_count;
    size_t slot_count;
    uint16_t entry_node;
} leir_phase0_program_desc_t;

typedef struct leir_phase0_program leir_phase0_program_t;

int leir_phase0_program_create(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out);
void leir_phase0_program_destroy(leir_phase0_program_t *program);

#endif
