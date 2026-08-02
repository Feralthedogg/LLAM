// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_MODULE_H
#define LLAM_EXPERIMENTS_LEIR_AOT_MODULE_H

#include "leir_phase0.h"

#include <stddef.h>
#include <stdint.h>

#define LEIR_AOT_MODULE_ABI_V1 1U
#define LEIR_AOT_MODULE_BACKEND_AGNOSTIC 0U
#define LEIR_AOT_BACKEND_LINUX_IO_URING 1U
#define LEIR_AOT_BACKEND_PORTABLE 2U

typedef enum leir_aot_resume_action_v1 {
    LEIR_AOT_RESUME_RETURN = 0,
    LEIR_AOT_RESUME_FAIL = 1,
} leir_aot_resume_action_v1_t;

typedef struct leir_aot_resume_result_v1 {
    uint32_t action;
    int error_code;
} leir_aot_resume_result_v1_t;

typedef int (*leir_aot_prepare_connect_write_v1_fn)(
    void *context,
    llam_fd_t fd,
    const struct sockaddr *address,
    socklen_t address_length,
    const void *payload,
    size_t payload_length,
    uint64_t generation,
    uint32_t connect_error_continuation,
    uint32_t write_continuation);

typedef struct leir_aot_backend_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t backend_kind;
    void *context;
    leir_aot_prepare_connect_write_v1_fn prepare_connect_write;
} leir_aot_backend_v1_t;

typedef struct leir_aot_module_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t backend_kind;
    uint64_t semantic_digest;
    size_t instance_size;
    size_t instance_alignment;
    size_t slot_count;
    int (*bind)(
        void *instance,
        size_t instance_size,
        const leir_phase0_value_t *values,
        size_t value_count);
    int (*prepare)(
        void *instance,
        const leir_aot_backend_v1_t *backend);
    int (*resume)(
        void *instance,
        uint32_t continuation,
        int64_t result,
        leir_aot_resume_result_v1_t *resume_out);
    int (*copy_outputs)(
        const void *instance,
        leir_phase0_value_t *values_out,
        size_t value_count);
    void (*cancel)(void *instance);
} leir_aot_module_v1_t;

#endif
