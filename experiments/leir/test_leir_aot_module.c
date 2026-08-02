// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "generated/leir_aot_connect_write.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

typedef union aligned_instance_storage {
    max_align_t alignment;
    unsigned char bytes[1024];
} aligned_instance_storage_t;

typedef struct prepare_capture {
    unsigned calls;
    llam_fd_t fd;
    struct sockaddr_storage address;
    socklen_t address_length;
    unsigned char payload[32];
    size_t payload_length;
    uint64_t generation;
    uint32_t connect_continuation;
    uint32_t write_continuation;
} prepare_capture_t;

static int capture_connect_write(
    void *context,
    llam_fd_t fd,
    const struct sockaddr *address,
    socklen_t address_length,
    const void *payload,
    size_t payload_length,
    uint64_t generation,
    uint32_t connect_error_continuation,
    uint32_t write_continuation) {
    prepare_capture_t *capture = context;

    if (capture == NULL || address == NULL ||
        address_length > (socklen_t)sizeof(capture->address) ||
        payload_length > sizeof(capture->payload) ||
        (payload == NULL && payload_length != 0U)) {
        errno = EINVAL;
        return -1;
    }
    capture->calls += 1U;
    capture->fd = fd;
    memcpy(&capture->address, address, (size_t)address_length);
    capture->address_length = address_length;
    if (payload_length != 0U) {
        memcpy(capture->payload, payload, payload_length);
    }
    capture->payload_length = payload_length;
    capture->generation = generation;
    capture->connect_continuation = connect_error_continuation;
    capture->write_continuation = write_continuation;
    return 0;
}

static void initialize_values(
    leir_phase0_value_t values[7],
    struct sockaddr_in *address,
    unsigned char payload[5]) {
    memset(values, 0, 7U * sizeof(values[0]));
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(4321U);
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memcpy(payload, "hello", 5U);
    values[0].fd = (llam_fd_t)7;
    values[1].buffer.data = address;
    values[1].buffer.size = sizeof(*address);
    values[2].u64 = sizeof(*address);
    values[3].i64 = -1;
    values[4].buffer.data = payload;
    values[4].buffer.size = 5U;
    values[5].u64 = 5U;
    values[6].i64 = -1;
}

static int test_module_prepares_owned_connect_write(void) {
    const leir_aot_module_v1_t *module =
        &leir_aot_connect_write_module_v1;
    aligned_instance_storage_t storage;
    leir_phase0_value_t values[7];
    leir_phase0_value_t outputs[7];
    struct sockaddr_in address;
    unsigned char payload[5];
    prepare_capture_t capture;
    leir_aot_backend_v1_t backend;
    leir_aot_resume_result_v1_t resume;

    memset(&storage, 0, sizeof(storage));
    memset(&capture, 0, sizeof(capture));
    initialize_values(values, &address, payload);
    backend = (leir_aot_backend_v1_t){
        .abi_version = LEIR_AOT_MODULE_ABI_V1,
        .struct_size = sizeof(backend),
        .backend_kind = LEIR_AOT_BACKEND_LINUX_IO_URING,
        .context = &capture,
        .prepare_connect_write = capture_connect_write,
    };
    if (module->abi_version != LEIR_AOT_MODULE_ABI_V1 ||
        module->struct_size != sizeof(*module) ||
        module->backend_kind != LEIR_AOT_BACKEND_LINUX_IO_URING ||
        module->semantic_digest == 0U ||
        module->instance_size > sizeof(storage.bytes) ||
        module->instance_alignment > _Alignof(max_align_t) ||
        module->slot_count != 7U ||
        module->bind(storage.bytes,
                     sizeof(storage.bytes),
                     values,
                     7U) != 0) {
        return 1;
    }
    address.sin_port = htons(9999U);
    if (module->prepare(storage.bytes, &backend) != 0 ||
        capture.calls != 1U ||
        capture.fd != (llam_fd_t)7 ||
        capture.address_length != sizeof(address) ||
        ((struct sockaddr_in *)(void *)&capture.address)->sin_port !=
            htons(4321U) ||
        capture.payload_length != 5U ||
        memcmp(capture.payload, "hello", 5U) != 0 ||
        capture.generation == 0U ||
        capture.connect_continuation !=
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR ||
        capture.write_continuation !=
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) {
        return 1;
    }
    if (module->resume(
            storage.bytes,
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT,
            3,
            &resume) != 0 ||
        resume.action != LEIR_AOT_RESUME_RETURN ||
        resume.error_code != 0 ||
        module->copy_outputs(
            storage.bytes, outputs, 7U) != 0 ||
        outputs[3].i64 != 0 || outputs[6].i64 != 3) {
        return 1;
    }
    return 0;
}

static int test_module_reports_connect_failure_and_cancel(void) {
    const leir_aot_module_v1_t *module =
        &leir_aot_connect_write_module_v1;
    aligned_instance_storage_t storage;
    leir_phase0_value_t values[7];
    leir_phase0_value_t outputs[7];
    struct sockaddr_in address;
    unsigned char payload[5];
    leir_aot_resume_result_v1_t resume;

    memset(&storage, 0, sizeof(storage));
    initialize_values(values, &address, payload);
    if (module->bind(storage.bytes,
                     sizeof(storage.bytes),
                     values,
                     7U) != 0 ||
        module->resume(
            storage.bytes,
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR,
            -ECONNREFUSED,
            &resume) != 0 ||
        resume.action != LEIR_AOT_RESUME_FAIL ||
        resume.error_code != ECONNREFUSED ||
        module->copy_outputs(
            storage.bytes, outputs, 7U) != 0 ||
        outputs[3].i64 != -1 || outputs[6].i64 != -1) {
        return 1;
    }

    if (module->bind(storage.bytes,
                     sizeof(storage.bytes),
                     values,
                     7U) != 0) {
        return 1;
    }
    module->cancel(storage.bytes);
    if (module->resume(
            storage.bytes,
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT,
            5,
            &resume) != 0 ||
        resume.action != LEIR_AOT_RESUME_FAIL ||
        resume.error_code != ECANCELED) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_module_prepares_owned_connect_write() != 0) {
        fputs("test_module_prepares_owned_connect_write failed\n", stderr);
        return 1;
    }
    if (test_module_reports_connect_failure_and_cancel() != 0) {
        fputs("test_module_reports_connect_failure_and_cancel failed\n",
              stderr);
        return 1;
    }
    puts("LEIR generated AOT module tests passed");
    return 0;
}
