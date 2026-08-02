// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_linux_metadata_test.h"

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_completion.h"
#include "leir_aot_linux.h"
#include "leir_test_support.h"

#include "lccf_fact.h"
#include "llam/runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_LINUX

int leir_aot_linux_metadata_test_run(void) {
    return 0;
}

#else

enum {
    METADATA_PAYLOAD_SIZE = 64,
    METADATA_SLOT_COUNT = 7,
    METADATA_STORAGE_SIZE = 32768,
};

typedef union metadata_storage {
    max_align_t alignment;
    unsigned char bytes[METADATA_STORAGE_SIZE];
} metadata_storage_t;

typedef struct invalid_metadata_instance {
    leir_phase0_value_t values[METADATA_SLOT_COUNT];
    struct sockaddr_storage address;
    socklen_t address_length;
    size_t payload_length;
    uint64_t generation;
    uint32_t connect_continuation;
    uint32_t write_continuation;
    unsigned resume_calls;
    bool bound;
} invalid_metadata_instance_t;

typedef struct invalid_metadata_case {
    llam_fd_t client;
    leir_phase0_value_t values[METADATA_SLOT_COUNT];
    leir_phase0_value_t values_out[METADATA_SLOT_COUNT];
    unsigned char payload[METADATA_PAYLOAD_SIZE];
    metadata_storage_t ticket_storage;
    invalid_metadata_instance_t instance;
    leir_aot_linux_ticket_t *ticket;
    leir_aot_linux_metrics_t continuation_metrics;
    leir_aot_linux_metrics_t generation_metrics;
    leir_aot_resume_result_v1_t resume;
    int continuation_result;
    int continuation_errno;
    int generation_result;
    int generation_errno;
    int destroy_result;
    int destroy_errno;
} invalid_metadata_case_t;

static int invalid_metadata_bind(
    void *storage,
    size_t storage_size,
    const leir_phase0_value_t *values,
    size_t value_count) {
    invalid_metadata_instance_t *instance = storage;
    uint64_t address_length;
    uint64_t payload_length;

    if (instance == NULL || values == NULL ||
        storage_size < sizeof(*instance) ||
        value_count != METADATA_SLOT_COUNT) {
        errno = EINVAL;
        return -1;
    }
    address_length = values[2].u64;
    payload_length = values[5].u64;
    if (address_length == 0U ||
        address_length > sizeof(instance->address) ||
        address_length > values[1].buffer.size ||
        values[1].buffer.data == NULL ||
        payload_length > values[4].buffer.size ||
        (payload_length != 0U && values[4].buffer.data == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(instance->values, values, sizeof(instance->values));
    memcpy(&instance->address, values[1].buffer.data,
           (size_t)address_length);
    instance->address_length = (socklen_t)address_length;
    instance->payload_length = (size_t)payload_length;
    instance->bound = true;
    return 0;
}

static int invalid_metadata_prepare(
    void *storage,
    const leir_aot_backend_v1_t *backend) {
    invalid_metadata_instance_t *instance = storage;

    if (instance == NULL || !instance->bound || backend == NULL ||
        backend->prepare_connect_write == NULL) {
        errno = EINVAL;
        return -1;
    }
    return backend->prepare_connect_write(
        backend->context,
        instance->values[0].fd,
        (const struct sockaddr *)(const void *)&instance->address,
        instance->address_length,
        instance->values[4].buffer.data,
        instance->payload_length,
        instance->generation,
        instance->connect_continuation,
        instance->write_continuation);
}

static int invalid_metadata_resume(
    void *storage,
    uint32_t continuation,
    int64_t result,
    leir_aot_resume_result_v1_t *resume_out) {
    invalid_metadata_instance_t *instance = storage;

    (void)continuation;
    (void)result;
    if (instance == NULL || resume_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    instance->resume_calls += 1U;
    resume_out->action = LEIR_AOT_RESUME_FAIL;
    resume_out->error_code = EPROTO;
    return 0;
}

static int invalid_metadata_copy(
    const void *storage,
    leir_phase0_value_t *values_out,
    size_t value_count) {
    const invalid_metadata_instance_t *instance = storage;

    if (instance == NULL || values_out == NULL ||
        value_count != METADATA_SLOT_COUNT) {
        errno = EINVAL;
        return -1;
    }
    memcpy(values_out, instance->values, sizeof(instance->values));
    return 0;
}

static void invalid_metadata_cancel(void *storage) {
    invalid_metadata_instance_t *instance = storage;

    if (instance != NULL) {
        instance->bound = false;
    }
}

static const leir_aot_module_v1_t invalid_metadata_module = {
    .abi_version = LEIR_AOT_MODULE_ABI_V1,
    .struct_size = sizeof(leir_aot_module_v1_t),
    .backend_kind = LEIR_AOT_MODULE_BACKEND_AGNOSTIC,
    .semantic_digest = UINT64_C(0x494e56414c494431),
    .instance_size = sizeof(invalid_metadata_instance_t),
    .instance_alignment = _Alignof(invalid_metadata_instance_t),
    .slot_count = METADATA_SLOT_COUNT,
    .bind = invalid_metadata_bind,
    .prepare = invalid_metadata_prepare,
    .resume = invalid_metadata_resume,
    .copy_outputs = invalid_metadata_copy,
    .cancel = invalid_metadata_cancel,
};

static void invalid_metadata_task(void *argument) {
    invalid_metadata_case_t *test_case = argument;
    invalid_metadata_instance_t *instance = &test_case->instance;

    instance->generation = 1U;
    instance->connect_continuation =
        LEIR_AOT_COMPLETION_SITE_CAPACITY;
    instance->write_continuation =
        LEIR_AOT_CONNECT_WRITE_WRITE_RESULT;
    if (leir_aot_linux_ticket_bind(
            test_case->ticket, test_case->values,
            METADATA_SLOT_COUNT) != 0) {
        test_case->continuation_result = -2;
        test_case->continuation_errno = errno;
        return;
    }
    errno = 0;
    test_case->continuation_result = leir_aot_linux_ticket_run(
        test_case->ticket, test_case->values_out,
        METADATA_SLOT_COUNT, &test_case->resume,
        &test_case->continuation_metrics);
    test_case->continuation_errno = errno;

    instance->generation = LCCF_FACT_MAX_GENERATION + 1U;
    instance->connect_continuation =
        LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR;
    instance->write_continuation =
        LEIR_AOT_CONNECT_WRITE_WRITE_RESULT;
    if (leir_aot_linux_ticket_bind(
            test_case->ticket, test_case->values,
            METADATA_SLOT_COUNT) != 0) {
        test_case->generation_result = -2;
        test_case->generation_errno = errno;
        return;
    }
    errno = 0;
    test_case->generation_result = leir_aot_linux_ticket_run(
        test_case->ticket, test_case->values_out,
        METADATA_SLOT_COUNT, &test_case->resume,
        &test_case->generation_metrics);
    test_case->generation_errno = errno;
    errno = 0;
    test_case->destroy_result =
        leir_aot_linux_ticket_destroy(test_case->ticket);
    test_case->destroy_errno = errno;
}

static bool no_kernel_work(
    const leir_aot_linux_metrics_t *metrics) {
    return metrics->activations == 0U &&
           metrics->logical_operations == 0U &&
           metrics->queue_publications == 0U &&
           metrics->terminal_publications == 0U &&
           metrics->normalizations == 0U &&
           metrics->site_lookups == 0U &&
           metrics->interpreter_dispatches == 0U &&
           metrics->prepared_sqes == 0U &&
           metrics->observed_cqes == 0U &&
           metrics->suppressed_success_cqes == 0U &&
           metrics->task_parks == 0U &&
           metrics->terminal_wakes == 0U &&
           metrics->hot_allocations == 0U;
}

int leir_aot_linux_metadata_test_run(void) {
    invalid_metadata_case_t test_case;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_task_t *task = NULL;
    bool runtime_started = false;
    int result = 1;

    memset(&test_case, 0, sizeof(test_case));
    memset(&options, 0, sizeof(options));
    test_case.client = LLAM_INVALID_FD;
    test_case.continuation_result = -2;
    test_case.generation_result = -2;
    test_case.destroy_result = -2;
    if (leir_test_tcp_refused_address(
            &address, &address_length) != 0 ||
        leir_test_tcp_client(&test_case.client) != 0) {
        perror("Linux AOT invalid metadata fixtures");
        goto cleanup;
    }
    leir_test_fill_pattern(
        test_case.payload, sizeof(test_case.payload),
        UINT64_C(0x494e56414c4944));
    if (leir_test_connect_write_values(
            test_case.values, METADATA_SLOT_COUNT,
            test_case.client, &address, address_length,
            test_case.payload, sizeof(test_case.payload)) != 0 ||
        leir_aot_linux_ticket_init(
            test_case.ticket_storage.bytes,
            sizeof(test_case.ticket_storage.bytes),
            &invalid_metadata_module, &test_case.instance,
            sizeof(test_case.instance)) != 0) {
        perror("Linux AOT invalid metadata init");
        goto cleanup;
    }
    test_case.ticket = (leir_aot_linux_ticket_t *)(void *)
        test_case.ticket_storage.bytes;
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        if (leir_test_backend_is_unavailable(errno)) {
            fprintf(stderr,
                    "SKIP: Linux invalid metadata runtime unavailable: %s\n",
                    strerror(errno));
            result = 0;
        } else {
            perror("Linux AOT invalid metadata runtime");
        }
        goto cleanup;
    }
    runtime_started = true;
    task = llam_spawn(invalid_metadata_task, &test_case, NULL);
    if (task == NULL || llam_run() != 0 || llam_join(task) != 0) {
        perror("Linux AOT invalid metadata run");
        goto cleanup;
    }
    task = NULL;
    if (test_case.continuation_result != -1 ||
        test_case.continuation_errno != EINVAL ||
        test_case.generation_result != -1 ||
        test_case.generation_errno != EINVAL ||
        test_case.destroy_result != 0 ||
        test_case.destroy_errno != 0 ||
        test_case.instance.resume_calls != 0U ||
        !no_kernel_work(&test_case.continuation_metrics) ||
        !no_kernel_work(&test_case.generation_metrics)) {
        fputs("Linux AOT invalid metadata escaped preflight\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started && task != NULL) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
        (void)llam_join(task);
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_test_close(&test_case.client);
    return result;
}

#endif
