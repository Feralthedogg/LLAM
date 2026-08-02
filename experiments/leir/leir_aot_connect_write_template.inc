// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LEIR_AOT_CONNECT_WRITE_DIGEST
#error "LEIR_AOT_CONNECT_WRITE_DIGEST must be defined by generated code"
#endif

#include "generated/leir_aot_connect_write.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct leir_aot_connect_write_instance {
    uint64_t magic;
    uint64_t generation;
    leir_phase0_value_t slots[7];
    struct sockaddr_storage address;
    socklen_t address_length;
    size_t payload_length;
    bool bound;
    bool cancelled;
} leir_aot_connect_write_instance_t;

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static int bind_connect_write(
    void *storage,
    size_t storage_size,
    const leir_phase0_value_t *values,
    size_t value_count) {
    leir_aot_connect_write_instance_t *instance = storage;
    uint64_t old_magic = 0U;
    uint64_t old_generation = 0U;
    uint64_t address_length;
    uint64_t payload_length;

    if (storage == NULL || values == NULL || value_count != 7U ||
        storage_size < sizeof(*instance) ||
        (uintptr_t)storage %
                _Alignof(leir_aot_connect_write_instance_t) != 0U ||
        LLAM_FD_IS_INVALID(values[0].fd)) {
        return fail_with_errno(EINVAL);
    }
    address_length = values[2].u64;
    payload_length = values[5].u64;
    if (address_length == 0U ||
        address_length > sizeof(instance->address) ||
        address_length > values[1].buffer.size ||
        values[1].buffer.data == NULL ||
        payload_length > SIZE_MAX ||
        payload_length > values[4].buffer.size ||
        (payload_length != 0U && values[4].buffer.data == NULL)) {
        return fail_with_errno(EINVAL);
    }

    memcpy(&old_magic, storage, sizeof(old_magic));
    if (old_magic == LEIR_AOT_CONNECT_WRITE_DIGEST) {
        memcpy(&old_generation,
               (const unsigned char *)storage + sizeof(old_magic),
               sizeof(old_generation));
    }
    memset(instance, 0, sizeof(*instance));
    instance->magic = LEIR_AOT_CONNECT_WRITE_DIGEST;
    instance->generation =
        old_generation == UINT64_MAX ? 1U : old_generation + 1U;
    memcpy(instance->slots, values, sizeof(instance->slots));
    memcpy(&instance->address,
           values[1].buffer.data,
           (size_t)address_length);
    instance->address_length = (socklen_t)address_length;
    instance->payload_length = (size_t)payload_length;
    instance->bound = true;
    return 0;
}

static int prepare_connect_write(
    void *storage,
    const leir_aot_backend_v1_t *backend) {
    leir_aot_connect_write_instance_t *instance = storage;

    if (instance == NULL || !instance->bound ||
        instance->magic != LEIR_AOT_CONNECT_WRITE_DIGEST ||
        backend == NULL ||
        backend->abi_version != LEIR_AOT_MODULE_ABI_V1 ||
        backend->struct_size < sizeof(*backend) ||
        backend->backend_kind != LEIR_AOT_BACKEND_LINUX_IO_URING ||
        backend->prepare_connect_write == NULL) {
        return fail_with_errno(EINVAL);
    }
    if (instance->cancelled) {
        return fail_with_errno(ECANCELED);
    }
    return backend->prepare_connect_write(
        backend->context,
        instance->slots[0].fd,
        (const struct sockaddr *)(const void *)&instance->address,
        instance->address_length,
        instance->slots[4].buffer.data,
        instance->payload_length,
        instance->generation,
        LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR,
        LEIR_AOT_CONNECT_WRITE_WRITE_RESULT);
}

static int error_from_cqe_result(int64_t result) {
    if (result >= 0 || result < -(int64_t)INT_MAX) {
        return EPROTO;
    }
    return (int)-result;
}

static void publish_failure(
    leir_aot_connect_write_instance_t *instance,
    int error_code,
    leir_aot_resume_result_v1_t *resume_out) {
    instance->slots[3].i64 = -1;
    instance->slots[6].i64 = -1;
    resume_out->action = LEIR_AOT_RESUME_FAIL;
    resume_out->error_code = error_code;
}

static int resume_connect_write(
    void *storage,
    uint32_t continuation,
    int64_t result,
    leir_aot_resume_result_v1_t *resume_out) {
    leir_aot_connect_write_instance_t *instance = storage;

    if (instance == NULL || resume_out == NULL || !instance->bound ||
        instance->magic != LEIR_AOT_CONNECT_WRITE_DIGEST) {
        return fail_with_errno(EINVAL);
    }
    if (instance->cancelled) {
        publish_failure(instance, ECANCELED, resume_out);
        return 0;
    }
    if (continuation == LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) {
        publish_failure(
            instance, error_from_cqe_result(result), resume_out);
        return 0;
    }
    if (continuation != LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) {
        publish_failure(instance, EPROTO, resume_out);
        return 0;
    }

    instance->slots[3].i64 = 0;
    if (result < 0) {
        publish_failure(
            instance, error_from_cqe_result(result), resume_out);
        return 0;
    }
    if ((result == 0 && instance->payload_length != 0U) ||
        (uint64_t)result > (uint64_t)instance->payload_length) {
        publish_failure(
            instance,
            result == 0 ? EIO : EPROTO,
            resume_out);
        return 0;
    }
    instance->slots[6].i64 = result;
    resume_out->action = LEIR_AOT_RESUME_RETURN;
    resume_out->error_code = 0;
    return 0;
}

static int copy_connect_write_outputs(
    const void *storage,
    leir_phase0_value_t *values_out,
    size_t value_count) {
    const leir_aot_connect_write_instance_t *instance = storage;

    if (instance == NULL || values_out == NULL || value_count != 7U ||
        !instance->bound ||
        instance->magic != LEIR_AOT_CONNECT_WRITE_DIGEST) {
        return fail_with_errno(EINVAL);
    }
    memcpy(values_out, instance->slots, sizeof(instance->slots));
    return 0;
}

static void cancel_connect_write(void *storage) {
    leir_aot_connect_write_instance_t *instance = storage;

    if (instance != NULL && instance->bound &&
        instance->magic == LEIR_AOT_CONNECT_WRITE_DIGEST) {
        instance->cancelled = true;
    }
}

const leir_aot_module_v1_t leir_aot_connect_write_module_v1 = {
    .abi_version = LEIR_AOT_MODULE_ABI_V1,
    .struct_size = sizeof(leir_aot_module_v1_t),
    .backend_kind = LEIR_AOT_BACKEND_LINUX_IO_URING,
    .semantic_digest = LEIR_AOT_CONNECT_WRITE_DIGEST,
    .instance_size = sizeof(leir_aot_connect_write_instance_t),
    .instance_alignment = _Alignof(leir_aot_connect_write_instance_t),
    .slot_count = 7U,
    .bind = bind_connect_write,
    .prepare = prepare_connect_write,
    .resume = resume_connect_write,
    .copy_outputs = copy_connect_write_outputs,
    .cancel = cancel_connect_write,
};
