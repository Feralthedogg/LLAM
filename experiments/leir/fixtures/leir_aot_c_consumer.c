// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/fixtures/leir_aot_c_consumer.c
 * @brief Standalone C consumer of the generated LEIR AOT module ABI.
 */

#include "generated/leir_aot_connect_write.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct consumer_capture {
    llam_fd_t expected_fd;
    struct sockaddr_storage expected_address;
    socklen_t expected_address_length;
    const unsigned char *expected_payload;
    size_t expected_payload_length;
    uint64_t previous_generation;
    uint64_t observed_generation;
    uint32_t connect_error_continuation;
    uint32_t write_continuation;
    unsigned calls;
} consumer_capture_t;

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
    consumer_capture_t *capture = context;

    if (capture == NULL || fd != capture->expected_fd ||
        address == NULL ||
        address_length != capture->expected_address_length ||
        memcmp(
            address,
            &capture->expected_address,
            (size_t)address_length) != 0 ||
        payload != capture->expected_payload ||
        payload_length != capture->expected_payload_length ||
        generation == 0U ||
        generation <= capture->previous_generation ||
        connect_error_continuation == write_continuation) {
        errno = EPROTO;
        return -1;
    }
    capture->observed_generation = generation;
    capture->connect_error_continuation =
        connect_error_continuation;
    capture->write_continuation = write_continuation;
    capture->calls += 1U;
    return 0;
}

static void fill_values(
    leir_phase0_value_t values[7],
    consumer_capture_t *capture) {
    memset(values, 0, sizeof(leir_phase0_value_t) * 7U);
    values[0].fd = capture->expected_fd;
    values[1].buffer.data = &capture->expected_address;
    values[1].buffer.size =
        (size_t)capture->expected_address_length;
    values[2].u64 = (uint64_t)capture->expected_address_length;
    values[3].i64 = -1;
    values[4].buffer.data =
        (void *)(uintptr_t)capture->expected_payload;
    values[4].buffer.size = capture->expected_payload_length;
    values[5].u64 = (uint64_t)capture->expected_payload_length;
    values[6].i64 = -1;
}

int main(void) {
    static const unsigned char payload[] = {
        0x4cU, 0x45U, 0x49U, 0x52U,
        0x2dU, 0x41U, 0x4fU, 0x54U,
    };
    const leir_aot_module_v1_t *module =
        &leir_aot_connect_write_module_v1;
    leir_aot_backend_v1_t backend;
    leir_phase0_value_t values[7];
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    consumer_capture_t capture;
    void *instance;
    int failed = 1;

    memset(&capture, 0, sizeof(capture));
    capture.expected_fd = (llam_fd_t)7;
    capture.expected_address.ss_family = AF_INET;
    capture.expected_address_length =
        (socklen_t)sizeof(capture.expected_address);
    capture.expected_payload = payload;
    capture.expected_payload_length = sizeof(payload);
    memset(&backend, 0, sizeof(backend));
    backend.abi_version = LEIR_AOT_MODULE_ABI_V1;
    backend.struct_size = sizeof(backend);
    backend.backend_kind = LEIR_AOT_BACKEND_LINUX_IO_URING;
    backend.context = &capture;
    backend.prepare_connect_write = capture_connect_write;

    if (module->abi_version != LEIR_AOT_MODULE_ABI_V1 ||
        module->struct_size < sizeof(*module) ||
        module->semantic_digest != UINT64_C(0xca50ff7ddbf91222) ||
        module->instance_size == 0U ||
        module->instance_alignment == 0U ||
        module->slot_count != 7U) {
        fputs("generated module descriptor mismatch\n", stderr);
        return 1;
    }
    instance = malloc(module->instance_size);
    if (instance == NULL ||
        (uintptr_t)instance % module->instance_alignment != 0U) {
        fputs("generated module allocation mismatch\n", stderr);
        free(instance);
        return 1;
    }
    memset(instance, 0, module->instance_size);
    fill_values(values, &capture);
    memset(outputs, 0, sizeof(outputs));
    memset(&resume, 0, sizeof(resume));
    if (module->bind(
            instance, module->instance_size, values, 7U) != 0 ||
        module->prepare(instance, &backend) != 0 ||
        capture.calls != 1U ||
        module->resume(
            instance,
            capture.write_continuation,
            (int64_t)sizeof(payload),
            &resume) != 0 ||
        resume.action != LEIR_AOT_RESUME_RETURN ||
        resume.error_code != 0 ||
        module->copy_outputs(instance, outputs, 7U) != 0 ||
        outputs[3].i64 != 0 ||
        outputs[6].i64 != (int64_t)sizeof(payload)) {
        fputs("generated module success contract mismatch\n", stderr);
        goto cleanup;
    }

    capture.previous_generation = capture.observed_generation;
    capture.expected_fd = (llam_fd_t)8;
    fill_values(values, &capture);
    if (module->bind(
            instance, module->instance_size, values, 7U) != 0 ||
        module->prepare(instance, &backend) != 0 ||
        capture.calls != 2U ||
        capture.observed_generation <= capture.previous_generation) {
        fputs("generated module generation contract mismatch\n", stderr);
        goto cleanup;
    }

    capture.previous_generation = capture.observed_generation;
    capture.expected_fd = (llam_fd_t)9;
    fill_values(values, &capture);
    errno = 0;
    if (module->bind(
            instance, module->instance_size, values, 7U) != 0) {
        fputs("generated module cancellation bind failed\n", stderr);
        goto cleanup;
    }
    module->cancel(instance);
    if (module->prepare(instance, &backend) == 0 || errno != ECANCELED ||
        capture.calls != 2U) {
        fputs("generated module cancellation contract mismatch\n", stderr);
        goto cleanup;
    }
    failed = 0;

cleanup:
    free(instance);
    if (failed == 0) {
        puts("LEIR AOT standalone C consumer passed");
    }
    return failed;
}
