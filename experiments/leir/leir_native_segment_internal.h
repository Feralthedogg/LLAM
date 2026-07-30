// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_NATIVE_SEGMENT_INTERNAL_H
#define LLAM_EXPERIMENTS_LEIR_NATIVE_SEGMENT_INTERNAL_H

#include "leir_native_segment.h"
#include "leir_phase0_internal.h"
#include "io/runtime_io_api_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_segment_linux_internal.h"

_Static_assert(
    LEIR_NATIVE_MAX_BATCH_SEGMENTS ==
        LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS,
    "public and Linux native batch limits must match");

typedef struct leir_native_fixed_buffer {
    void *external;
    void *scratch;
    size_t logical_size;
    size_t registered_size;
    size_t recv_copy_size;
    unsigned first_recv_operation;
    uint16_t slot;
    bool recv_written;
} leir_native_fixed_buffer_t;

typedef struct leir_native_linux_fixed_state {
    leir_native_fixed_buffer_t
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned buffer_count;
    uint8_t
        op_buffer_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_resource_lease_t lease;
    llam_runtime_t *owner_runtime;
    uint64_t owner_runtime_id;
} leir_native_linux_fixed_state_t;

typedef struct leir_native_linux_state {
    llam_linux_native_segment_t segment;
    llam_fd_t
        pinned_fds[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned pinned_fd_count;
    uint8_t
        op_fd_indices[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    leir_native_linux_fixed_state_t *fixed;
} leir_native_linux_state_t;
#endif

struct leir_native_instance {
    const leir_phase0_program_t *program;
    leir_native_plan_t plan;
    leir_phase0_value_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_native_mode_t mode;
    atomic_uint activity;
    atomic_uint bound;
    struct leir_native_linux_state *linux_state;
};

_Static_assert(
    sizeof(leir_native_instance_t) <= 384U,
    "common native instance exceeded its layout budget");

int leir_native_step_length(
    const leir_phase0_program_t *program,
    const leir_native_step_t *step,
    const leir_phase0_value_t *values,
    uint32_t *length_out);

#if LLAM_RUNTIME_BACKEND_LINUX
_Static_assert(
    sizeof(leir_native_linux_state_t) <= 896U,
    "Linux native core state exceeded its layout budget");
_Static_assert(
    sizeof(leir_native_linux_fixed_state_t) <= 512U,
    "Linux native fixed state exceeded its layout budget");

int leir_native_linux_state_build(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values,
    leir_native_linux_state_t **state_out);
int leir_native_linux_state_detach_fixed(
    leir_native_linux_state_t *state);
void leir_native_linux_state_free(
    leir_native_linux_state_t *state);
int leir_native_linux_state_attach_fixed(
    leir_native_instance_t *instance,
    llam_runtime_t *runtime,
    llam_node_t *node);
void leir_native_linux_state_copy_fixed_inputs(
    leir_native_instance_t *instance);
void leir_native_linux_state_copy_fixed_outputs(
    leir_native_instance_t *instance,
    bool activated);
#endif

#endif
