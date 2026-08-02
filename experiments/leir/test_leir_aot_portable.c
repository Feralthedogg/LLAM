// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_portable.h"

#include "generated/leir_aot_connect_write.h"
#include "lccf_portable_errno.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,     \
                    __LINE__, #condition);                                 \
            return 1;                                                      \
        }                                                                  \
    } while (0)

typedef union aligned_storage {
    max_align_t alignment;
    unsigned char bytes[4096];
} aligned_storage_t;

typedef struct effect_script {
    leir_aot_portable_ticket_t *ticket;
    int connect_result;
    int connect_error;
    ssize_t write_result;
    int write_error;
    unsigned connect_calls;
    unsigned write_calls;
    unsigned order[2];
    unsigned order_count;
    bool cancel_from_connect;
    bool destroy_from_connect;
    int nested_result;
    int nested_error;
    bool arguments_valid;
} effect_script_t;

typedef struct portable_fixture {
    aligned_storage_t ticket_storage;
    aligned_storage_t module_storage;
    leir_aot_portable_ticket_t *ticket;
    leir_phase0_value_t values[7];
    struct sockaddr_in address;
    unsigned char payload[5];
    effect_script_t script;
} portable_fixture_t;

static int scripted_connect(void *context, llam_fd_t fd,
                            const struct sockaddr *address,
                            socklen_t address_length) {
    effect_script_t *script = context;
    const struct sockaddr_in *ipv4 =
        (const struct sockaddr_in *)(const void *)address;

    script->connect_calls += 1U;
    if (script->order_count < 2U) {
        script->order[script->order_count++] = 1U;
    }
    if (fd != (llam_fd_t)7 || address == NULL ||
        address_length != (socklen_t)sizeof(*ipv4) ||
        ipv4->sin_family != AF_INET ||
        ipv4->sin_port != htons(4321U)) {
        script->arguments_valid = false;
    }
    if (script->cancel_from_connect) {
        errno = 0;
        script->nested_result =
            leir_aot_portable_ticket_cancel(script->ticket);
        script->nested_error = errno;
    }
    if (script->destroy_from_connect) {
        errno = 0;
        script->nested_result =
            leir_aot_portable_ticket_destroy(script->ticket);
        script->nested_error = errno;
    }
    errno = script->connect_error;
    return script->connect_result;
}

static ssize_t scripted_write(void *context, llam_fd_t fd,
                              const void *payload,
                              size_t payload_length) {
    effect_script_t *script = context;

    script->write_calls += 1U;
    if (script->order_count < 2U) {
        script->order[script->order_count++] = 2U;
    }
    if (fd != (llam_fd_t)7 || payload == NULL ||
        payload_length != 5U ||
        memcmp(payload, "hello", 5U) != 0) {
        script->arguments_valid = false;
    }
    errno = script->write_error;
    return script->write_result;
}

static void initialize_values(portable_fixture_t *fixture) {
    memset(fixture->values, 0, sizeof(fixture->values));
    memset(&fixture->address, 0, sizeof(fixture->address));
    fixture->address.sin_family = AF_INET;
    fixture->address.sin_port = htons(4321U);
    fixture->address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memcpy(fixture->payload, "hello", sizeof(fixture->payload));
    fixture->values[0].fd = (llam_fd_t)7;
    fixture->values[1].buffer.data = &fixture->address;
    fixture->values[1].buffer.size = sizeof(fixture->address);
    fixture->values[2].u64 = sizeof(fixture->address);
    fixture->values[3].i64 = -1;
    fixture->values[4].buffer.data = fixture->payload;
    fixture->values[4].buffer.size = sizeof(fixture->payload);
    fixture->values[5].u64 = sizeof(fixture->payload);
    fixture->values[6].i64 = -1;
}

static int fixture_init(portable_fixture_t *fixture) {
    leir_aot_portable_effects_t effects;

    memset(fixture, 0, sizeof(*fixture));
    initialize_values(fixture);
    fixture->script.connect_result = 0;
    fixture->script.write_result = 3;
    fixture->script.arguments_valid = true;
    effects.context = &fixture->script;
    effects.connect = scripted_connect;
    effects.write = scripted_write;
    CHECK(leir_aot_portable_ticket_size() <=
          sizeof(fixture->ticket_storage.bytes));
    CHECK(leir_aot_portable_ticket_alignment() <= _Alignof(max_align_t));
    CHECK(leir_aot_connect_write_module_v1.instance_size <=
          sizeof(fixture->module_storage.bytes));
    CHECK(leir_aot_portable_ticket_init(
              fixture->ticket_storage.bytes,
              sizeof(fixture->ticket_storage.bytes),
              &leir_aot_connect_write_module_v1,
              fixture->module_storage.bytes,
              sizeof(fixture->module_storage.bytes), &effects) == 0);
    fixture->ticket = (leir_aot_portable_ticket_t *)(void *)
        fixture->ticket_storage.bytes;
    fixture->script.ticket = fixture->ticket;
    CHECK(leir_aot_portable_ticket_bind(
              fixture->ticket, fixture->values, 7U) == 0);
    return 0;
}

static int assert_common_metrics(
    const leir_aot_portable_metrics_t *metrics,
    uint64_t effect_calls,
    uint32_t continuation) {
    CHECK(metrics->activations == 1U);
    CHECK(metrics->logical_operations == 2U);
    CHECK(metrics->effect_calls == effect_calls);
    CHECK(metrics->terminal_publications == 1U);
    CHECK(metrics->interpreter_dispatches == 0U);
    CHECK(metrics->normalizations == 1U);
    CHECK(metrics->site_lookups == 1U);
    CHECK(metrics->hot_allocations == 0U);
    CHECK(metrics->resumed_continuation == continuation);
    CHECK(metrics->generation != 0U);
    return 0;
}

static int test_partial_write_success(void) {
    portable_fixture_t fixture;
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t metrics;

    CHECK(fixture_init(&fixture) == 0);
    memset(outputs, 0, sizeof(outputs));
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &metrics) == 0);
    CHECK(fixture.script.arguments_valid);
    CHECK(fixture.script.connect_calls == 1U);
    CHECK(fixture.script.write_calls == 1U);
    CHECK(fixture.script.order_count == 2U);
    CHECK(fixture.script.order[0] == 1U);
    CHECK(fixture.script.order[1] == 2U);
    CHECK(outputs[3].i64 == 0);
    CHECK(outputs[6].i64 == 3);
    CHECK(resume.action == LEIR_AOT_RESUME_RETURN);
    CHECK(resume.error_code == 0);
    CHECK(assert_common_metrics(
              &metrics, 2U,
              LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);
    return 0;
}

static int test_connect_refusal(void) {
    portable_fixture_t fixture;
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t metrics;

    CHECK(fixture_init(&fixture) == 0);
    fixture.script.connect_result = -1;
    fixture.script.connect_error = ECONNREFUSED;
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &metrics) == 0);
    CHECK(fixture.script.connect_calls == 1U);
    CHECK(fixture.script.write_calls == 0U);
    CHECK(outputs[3].i64 == -1);
    CHECK(outputs[6].i64 == -1);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == ECONNREFUSED);
    CHECK(assert_common_metrics(
              &metrics, 1U,
              LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);
    return 0;
}

static int run_write_failure(ssize_t write_result, int write_error,
                             int expected_error) {
    portable_fixture_t fixture;
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t metrics;

    CHECK(fixture_init(&fixture) == 0);
    fixture.script.write_result = write_result;
    fixture.script.write_error = write_error;
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &metrics) == 0);
    CHECK(fixture.script.connect_calls == 1U);
    CHECK(fixture.script.write_calls == 1U);
    CHECK(outputs[3].i64 == -1);
    CHECK(outputs[6].i64 == -1);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == expected_error);
    CHECK(assert_common_metrics(
              &metrics, 2U,
              LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);
    return 0;
}

static int test_write_failures(void) {
    CHECK(run_write_failure(-1, EPIPE, EPIPE) == 0);
    CHECK(run_write_failure(0, 0, EIO) == 0);
    CHECK(run_write_failure(6, 0, EPROTO) == 0);
    return 0;
}

static int test_bound_and_in_hook_cancellation(void) {
    portable_fixture_t fixture;
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t metrics;

    CHECK(fixture_init(&fixture) == 0);
    CHECK(leir_aot_portable_ticket_cancel(fixture.ticket) == 0);
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &metrics) == 0);
    CHECK(fixture.script.connect_calls == 0U);
    CHECK(fixture.script.write_calls == 0U);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == ECANCELED);
    CHECK(assert_common_metrics(
              &metrics, 0U,
              LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);

    CHECK(fixture_init(&fixture) == 0);
    fixture.script.cancel_from_connect = true;
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &metrics) == 0);
    CHECK(fixture.script.nested_result == 0);
    CHECK(fixture.script.nested_error == 0);
    CHECK(fixture.script.connect_calls == 1U);
    CHECK(fixture.script.write_calls == 0U);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == ECANCELED);
    CHECK(assert_common_metrics(
              &metrics, 1U,
              LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);
    return 0;
}

static int test_destroy_while_running_and_rebind(void) {
    portable_fixture_t fixture;
    leir_phase0_value_t outputs[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t first;
    leir_aot_portable_metrics_t second;

    CHECK(fixture_init(&fixture) == 0);
    fixture.script.destroy_from_connect = true;
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &first) == 0);
    CHECK(fixture.script.nested_result == -1);
    CHECK(fixture.script.nested_error == EBUSY);
    fixture.script.destroy_from_connect = false;
    fixture.script.connect_calls = 0U;
    fixture.script.write_calls = 0U;
    fixture.script.order_count = 0U;
    fixture.script.write_result = 2;
    CHECK(leir_aot_portable_ticket_bind(
              fixture.ticket, fixture.values, 7U) == 0);
    CHECK(leir_aot_portable_ticket_run(
              fixture.ticket, outputs, 7U, &resume, &second) == 0);
    CHECK(second.generation > first.generation);
    CHECK(outputs[6].i64 == 2);
    CHECK(assert_common_metrics(
              &second, 2U,
              LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) == 0);
    CHECK(leir_aot_portable_ticket_destroy(fixture.ticket) == 0);
    return 0;
}

int main(void) {
    CHECK(test_partial_write_success() == 0);
    CHECK(test_connect_refusal() == 0);
    CHECK(test_write_failures() == 0);
    CHECK(test_bound_and_in_hook_cancellation() == 0);
    CHECK(test_destroy_while_running_and_rebind() == 0);
    puts("LEIR portable AOT executor tests passed");
    return 0;
}
