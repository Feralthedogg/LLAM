// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_TEST_NATIVE_FIXTURE_H
#define LLAM_EXPERIMENTS_LEIR_TEST_NATIVE_FIXTURE_H

#include "leir_native_segment.h"
#include "leir_test_support.h"

#include <stdbool.h>
#include <stddef.h>

#define TEST_FD_SLOT 0U
#define TEST_MUT_BUFFER_SLOT 1U
#define TEST_CONST_BUFFER_SLOT 2U
#define TEST_LENGTH_SLOT 3U
#define TEST_FIRST_RESULT_SLOT 4U
#define TEST_STORAGE_BYTES 8192U

typedef union test_instance_storage {
    max_align_t alignment;
    unsigned char bytes[TEST_STORAGE_BYTES];
} test_instance_storage_t;

typedef struct bind_fixture {
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    test_instance_storage_t storage;
    leir_native_instance_t *instance;
    leir_phase0_value_t values[LEIR_PHASE0_MAX_SLOTS];
    unsigned char mut_buffer[64];
    unsigned char const_buffer[64];
    llam_fd_t pair[2];
    size_t value_count;
    bool signed_length;
} bind_fixture_t;

leir_phase0_node_desc_t terminal_node(
    leir_phase0_opcode_t opcode,
    uint16_t result_slot);
int create_two_step_program(
    bool signed_length,
    leir_phase0_program_t **out);
int bind_fixture_init_mode(
    bind_fixture_t *fixture,
    bool signed_length,
    leir_native_mode_t mode);
int bind_fixture_init(
    bind_fixture_t *fixture,
    bool signed_length);
void bind_fixture_destroy(bind_fixture_t *fixture);
int expect_bind_failure(
    bind_fixture_t *fixture,
    int expected_errno);

#endif
