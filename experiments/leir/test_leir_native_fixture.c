// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#include "test_leir_native_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
#include <sys/socket.h>
#endif

leir_phase0_node_desc_t terminal_node(
    leir_phase0_opcode_t opcode,
    uint16_t result_slot) {
    leir_phase0_node_desc_t node;

    memset(&node, 0, sizeof(node));
    node.opcode = (uint16_t)opcode;
    node.fd_slot = LEIR_PHASE0_NODE_NONE;
    node.buffer_slot = LEIR_PHASE0_NODE_NONE;
    node.length_slot = LEIR_PHASE0_NODE_NONE;
    node.result_slot = result_slot;
    node.on_success = LEIR_PHASE0_NODE_NONE;
    node.on_eof = LEIR_PHASE0_NODE_NONE;
    node.on_error = LEIR_PHASE0_NODE_NONE;
    return node;
}

int create_two_step_program(
    bool signed_length,
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t slots[6];
    leir_phase0_program_desc_t desc;
    unsigned i;

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[TEST_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[TEST_MUT_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[TEST_CONST_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_CONST_BUFFER;
    slots[TEST_LENGTH_SLOT] = signed_length
        ? LEIR_PHASE0_SLOT_I64
        : LEIR_PHASE0_SLOT_U64;
    slots[TEST_FIRST_RESULT_SLOT] = LEIR_PHASE0_SLOT_I64;
    slots[TEST_FIRST_RESULT_SLOT + 1U] =
        LEIR_PHASE0_SLOT_I64;

    for (i = 0U; i < 2U; i += 1U) {
        nodes[i].opcode = (uint16_t)(
            i == 0U
                ? LEIR_PHASE0_OP_WRITE_ALL
                : LEIR_PHASE0_OP_READ_EXACT);
        nodes[i].fd_slot = TEST_FD_SLOT;
        nodes[i].buffer_slot = (uint16_t)(
            i == 0U
                ? TEST_CONST_BUFFER_SLOT
                : TEST_MUT_BUFFER_SLOT);
        nodes[i].length_slot = TEST_LENGTH_SLOT;
        nodes[i].result_slot =
            (uint16_t)(TEST_FIRST_RESULT_SLOT + i);
        nodes[i].on_success = (uint16_t)(i + 1U);
        nodes[i].on_eof = 3U;
        nodes[i].on_error = 3U;
    }
    nodes[2] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        TEST_FIRST_RESULT_SLOT + 1U);
    nodes[3] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        TEST_FIRST_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = 6U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

int bind_fixture_init_mode(
    bind_fixture_t *fixture,
    bool signed_length,
    leir_native_mode_t mode) {
    int socket_type = SOCK_STREAM;

    memset(fixture, 0, sizeof(*fixture));
    fixture->pair[0] = LLAM_INVALID_FD;
    fixture->pair[1] = LLAM_INVALID_FD;
#if defined(__linux__)
    socket_type = SOCK_SEQPACKET;
#endif
    if (create_two_step_program(
            signed_length, &fixture->program) != 0 ||
        leir_native_plan_compile(
            fixture->program, &fixture->plan) != 0 ||
        leir_native_instance_size() >
            sizeof(fixture->storage.bytes) ||
        leir_native_instance_init(
            fixture->storage.bytes,
            sizeof(fixture->storage.bytes),
            fixture->program,
            &fixture->plan,
            mode) != 0 ||
        leir_test_socketpair_type(
            socket_type, fixture->pair) != 0) {
        return -1;
    }

    fixture->instance =
        (leir_native_instance_t *)fixture->storage.bytes;
    fixture->value_count = 6U;
    fixture->signed_length = signed_length;
    fixture->values[TEST_FD_SLOT].fd = fixture->pair[0];
    fixture->values[TEST_MUT_BUFFER_SLOT].buffer.data =
        fixture->mut_buffer;
    fixture->values[TEST_MUT_BUFFER_SLOT].buffer.size =
        sizeof(fixture->mut_buffer);
    fixture->values[TEST_CONST_BUFFER_SLOT].buffer.data =
        fixture->const_buffer;
    fixture->values[TEST_CONST_BUFFER_SLOT].buffer.size =
        sizeof(fixture->const_buffer);
    if (signed_length) {
        fixture->values[TEST_LENGTH_SLOT].i64 = 16;
    } else {
        fixture->values[TEST_LENGTH_SLOT].u64 = 16U;
    }
    return 0;
}

int bind_fixture_init(
    bind_fixture_t *fixture,
    bool signed_length) {
    return bind_fixture_init_mode(
        fixture,
        signed_length,
        LEIR_NATIVE_MODE_LINK);
}

void bind_fixture_destroy(bind_fixture_t *fixture) {
    if (fixture->instance != NULL) {
        (void)leir_native_instance_destroy(fixture->instance);
        fixture->instance = NULL;
    }
    leir_test_close(&fixture->pair[0]);
    leir_test_close(&fixture->pair[1]);
    leir_phase0_program_destroy(fixture->program);
    fixture->program = NULL;
}

int expect_bind_failure(
    bind_fixture_t *fixture,
    int expected_errno) {
    errno = 0;
    if (leir_native_instance_bind(
            fixture->instance,
            fixture->values,
            fixture->value_count) == 0 ||
        errno != expected_errno) {
        fprintf(
            stderr,
            "bind failure mismatch expected=%d actual=%d\n",
            expected_errno,
            errno);
        return 1;
    }
    return 0;
}
