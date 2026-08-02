// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_completion.h"

#include "lccf_fact.h"
#include "lccf_representation.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,     \
                    __LINE__, #condition);                                 \
            return 1;                                                      \
        }                                                                  \
    } while (0)

typedef union completion_storage {
    leir_aot_storage_align_t alignment;
    unsigned char bytes[4096];
} completion_storage_t;

enum {
    COMPLETION_REARM_GENERATIONS = 4096,
};

typedef struct spy_instance {
    unsigned resume_calls;
    unsigned copy_calls;
    unsigned cancel_calls;
    uint32_t continuation;
    int64_t result;
    bool cancelled;
    bool fail_resume;
    bool fail_copy;
    leir_phase0_value_t value;
} spy_instance_t;

static int spy_bind(void *storage, size_t storage_size,
                    const leir_phase0_value_t *values,
                    size_t value_count) {
    (void)storage;
    (void)storage_size;
    (void)values;
    (void)value_count;
    return 0;
}

static int spy_prepare(void *storage,
                       const leir_aot_backend_v1_t *backend) {
    (void)storage;
    (void)backend;
    return 0;
}

static int spy_resume(void *storage, uint32_t continuation,
                      int64_t result,
                      leir_aot_resume_result_v1_t *resume_out) {
    spy_instance_t *instance = storage;

    if (instance == NULL || resume_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    instance->resume_calls += 1U;
    instance->continuation = continuation;
    instance->result = instance->cancelled ? -(int64_t)ECANCELED : result;
    if (instance->fail_resume) {
        errno = EIO;
        return -1;
    }
    instance->value.i64 = instance->result;
    resume_out->action = instance->result < 0 ? LEIR_AOT_RESUME_FAIL
                                              : LEIR_AOT_RESUME_RETURN;
    resume_out->error_code = instance->result < 0
                                 ? (int)-instance->result
                                 : 0;
    return 0;
}

static int spy_copy_outputs(const void *storage,
                            leir_phase0_value_t *values_out,
                            size_t value_count) {
    spy_instance_t *instance = (spy_instance_t *)(uintptr_t)storage;

    if (instance == NULL || values_out == NULL || value_count != 1U) {
        errno = EINVAL;
        return -1;
    }
    instance->copy_calls += 1U;
    if (instance->fail_copy) {
        errno = ENOSPC;
        return -1;
    }
    values_out[0] = instance->value;
    return 0;
}

static void spy_cancel(void *storage) {
    spy_instance_t *instance = storage;

    if (instance != NULL) {
        instance->cancel_calls += 1U;
        instance->cancelled = true;
    }
}

static const leir_aot_module_v1_t spy_module = {
    .abi_version = LEIR_AOT_MODULE_ABI_V1,
    .struct_size = sizeof(leir_aot_module_v1_t),
    .backend_kind = LEIR_AOT_MODULE_BACKEND_AGNOSTIC,
    .semantic_digest = UINT64_C(0x5d42f27968ee6b91),
    .instance_size = sizeof(spy_instance_t),
    .instance_alignment = _Alignof(spy_instance_t),
    .slot_count = 1U,
    .bind = spy_bind,
    .prepare = spy_prepare,
    .resume = spy_resume,
    .copy_outputs = spy_copy_outputs,
    .cancel = spy_cancel,
};

static leir_aot_completion_record_t record_for(uint64_t generation,
                                                int64_t result) {
    leir_aot_completion_record_t record;

    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.result = result;
    record.continuation = 2U;
    record.source_kind = LCCF_FACT_SOURCE_EXTERNAL;
    record.event_kind = LCCF_FACT_EVENT_IO;
    return record;
}

static int initialize_completion(completion_storage_t *storage,
                                 spy_instance_t *instance,
                                 leir_aot_completion_t **completion_out) {
    memset(storage, 0, sizeof(*storage));
    memset(instance, 0, sizeof(*instance));
    CHECK(leir_aot_completion_size() <= sizeof(storage->bytes));
    CHECK(leir_aot_completion_alignment() <=
          _Alignof(leir_aot_storage_align_t));
    CHECK(leir_aot_completion_init(
              storage->bytes, sizeof(storage->bytes), &spy_module,
              instance, sizeof(*instance)) == 0);
    *completion_out = (leir_aot_completion_t *)(void *)storage->bytes;
    return 0;
}

static int test_shared_event_publication_and_direct_consume(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_record_t record = record_for(7U, 4);
    leir_aot_completion_metrics_t metrics;
    leir_aot_resume_result_v1_t resume;
    const uint64_t direct_guard =
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    CHECK(sizeof(lccf_event_core_t) == 48U);
    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    CHECK(leir_aot_completion_arm(completion, 7U) == 0);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.publications == 1U);
    CHECK(metrics.normalizations == 1U);
    CHECK(metrics.site_lookups == 0U);
    CHECK(instance.resume_calls == 0U);
    CHECK(instance.copy_calls == 0U);

    CHECK(leir_aot_completion_consume(
              completion, 7U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(instance.resume_calls == 1U);
    CHECK(instance.copy_calls == 1U);
    CHECK(instance.continuation == 2U);
    CHECK(instance.result == 4);
    CHECK(output.i64 == 4);
    CHECK(resume.action == LEIR_AOT_RESUME_RETURN);
    CHECK(resume.error_code == 0);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.direct_consumes == 1U);
    CHECK(metrics.queued_consumes == 0U);
    CHECK(metrics.normalizations == 1U);
    CHECK(metrics.site_lookups == 1U);

    CHECK(leir_aot_completion_consume(
              completion, 7U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == EBUSY);
    CHECK(instance.resume_calls == 1U);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

static int test_queue_transfer_and_module_defer(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_record_t record = record_for(11U, 5);
    leir_aot_completion_metrics_t metrics;
    leir_aot_resume_result_v1_t resume;
    const uint64_t queued_guard =
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    CHECK(leir_aot_completion_arm(completion, 11U) == 0);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 11U, LEIR_AOT_COMPLETION_DIRECT,
              queued_guard, &output, 1U, &resume) == EAGAIN);
    CHECK(instance.resume_calls == 0U);
    CHECK(leir_aot_completion_set_module_available(completion, false) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 11U, LEIR_AOT_COMPLETION_QUEUE,
              queued_guard, &output, 1U, &resume) == EAGAIN);
    CHECK(instance.resume_calls == 0U);
    CHECK(leir_aot_completion_set_module_available(completion, true) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 11U, LEIR_AOT_COMPLETION_QUEUE,
              queued_guard, &output, 1U, &resume) == 0);
    CHECK(instance.resume_calls == 1U);
    CHECK(instance.copy_calls == 1U);
    CHECK(output.i64 == 5);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.publications == 1U);
    CHECK(metrics.normalizations == 1U);
    CHECK(metrics.site_lookups == 3U);
    CHECK(metrics.direct_consumes == 0U);
    CHECK(metrics.queued_consumes == 1U);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

static int test_duplicate_and_stale_deliveries(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_record_t record = record_for(20U, 6);
    leir_aot_completion_record_t stale;
    leir_aot_completion_metrics_t metrics;
    leir_aot_resume_result_v1_t resume;
    const uint64_t direct_guard =
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    CHECK(leir_aot_completion_arm(completion, 20U) == 0);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    CHECK(leir_aot_completion_publish(completion, &record) == EBUSY);
    CHECK(leir_aot_completion_consume(
              completion, 19U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == ESTALE);
    CHECK(instance.resume_calls == 0U);
    CHECK(leir_aot_completion_consume(
              completion, 20U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(leir_aot_completion_arm(completion, 21U) == 0);
    record = record_for(21U, 9);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    stale = record_for(20U, 99);
    CHECK(leir_aot_completion_publish(completion, &stale) == ESTALE);
    CHECK(leir_aot_completion_consume(
              completion, 21U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(instance.resume_calls == 2U);
    CHECK(instance.result == 9);
    CHECK(output.i64 == 9);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.publications == 2U);
    CHECK(metrics.duplicate_rejections == 1U);
    CHECK(metrics.stale_rejections == 2U);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

static int test_cancel_and_malformed_failure(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_record_t malformed;
    leir_aot_resume_result_v1_t resume;
    const uint64_t direct_guard =
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    CHECK(leir_aot_completion_arm(completion, 30U) == 0);
    CHECK(leir_aot_completion_cancel(completion, 1U) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 30U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(instance.cancel_calls == 1U);
    CHECK(instance.resume_calls == 1U);
    CHECK(instance.result == -(int64_t)ECANCELED);
    CHECK(output.i64 == -(int64_t)ECANCELED);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == ECANCELED);

    instance.cancelled = false;
    CHECK(leir_aot_completion_arm(completion, 31U) == 0);
    malformed = record_for(31U, INT64_MIN);
    malformed.source_kind = LCCF_FACT_SOURCE_LINUX_CQE;
    CHECK(leir_aot_completion_publish(completion, &malformed) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 31U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(instance.resume_calls == 2U);
    CHECK(instance.result == -(int64_t)EPROTO);
    CHECK(output.i64 == -(int64_t)EPROTO);
    CHECK(resume.action == LEIR_AOT_RESUME_FAIL);
    CHECK(resume.error_code == EPROTO);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

static int test_callback_failure_aborts_and_rearms(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_record_t record = record_for(40U, 7);
    leir_aot_completion_metrics_t metrics;
    leir_aot_resume_result_v1_t resume;
    const uint64_t direct_guard =
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    CHECK(leir_aot_completion_arm(completion, 40U) == 0);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    instance.fail_resume = true;
    CHECK(leir_aot_completion_consume(
              completion, 40U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == EIO);
    CHECK(instance.resume_calls == 1U);
    CHECK(instance.copy_calls == 0U);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.aborts == 1U);

    instance.fail_resume = false;
    CHECK(leir_aot_completion_arm(completion, 41U) == 0);
    record = record_for(41U, 8);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    instance.fail_copy = true;
    CHECK(leir_aot_completion_consume(
              completion, 41U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == ENOSPC);
    CHECK(instance.resume_calls == 2U);
    CHECK(instance.copy_calls == 1U);
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.aborts == 2U);

    instance.fail_copy = false;
    CHECK(leir_aot_completion_arm(completion, 42U) == 0);
    record = record_for(42U, 9);
    CHECK(leir_aot_completion_publish(completion, &record) == 0);
    CHECK(leir_aot_completion_consume(
              completion, 42U, LEIR_AOT_COMPLETION_DIRECT,
              direct_guard, &output, 1U, &resume) == 0);
    CHECK(instance.resume_calls == 3U);
    CHECK(instance.copy_calls == 2U);
    CHECK(output.i64 == 9);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

static int test_repeated_publish_consume_rearm(void) {
    completion_storage_t storage;
    spy_instance_t instance;
    leir_phase0_value_t output;
    leir_aot_completion_t *completion;
    leir_aot_completion_metrics_t metrics;
    leir_aot_resume_result_v1_t resume;
    uint64_t generation;
    const uint64_t direct_guard =
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
        LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE;

    memset(&output, 0, sizeof(output));
    CHECK(initialize_completion(&storage, &instance, &completion) == 0);
    for (generation = 1U;
         generation <= COMPLETION_REARM_GENERATIONS;
         generation += 1U) {
        leir_aot_completion_record_t record =
            record_for(generation, (int64_t)generation);

        CHECK(leir_aot_completion_arm(completion, generation) == 0);
        CHECK(leir_aot_completion_publish(completion, &record) == 0);
        CHECK(leir_aot_completion_consume(
                  completion, generation, LEIR_AOT_COMPLETION_DIRECT,
                  direct_guard, &output, 1U, &resume) == 0);
        CHECK(output.i64 == (int64_t)generation);
        CHECK(resume.action == LEIR_AOT_RESUME_RETURN);
        CHECK(resume.error_code == 0);
    }
    leir_aot_completion_metrics(completion, &metrics);
    CHECK(metrics.publications == COMPLETION_REARM_GENERATIONS);
    CHECK(metrics.normalizations == COMPLETION_REARM_GENERATIONS);
    CHECK(metrics.site_lookups == COMPLETION_REARM_GENERATIONS);
    CHECK(metrics.direct_consumes == COMPLETION_REARM_GENERATIONS);
    CHECK(metrics.queued_consumes == 0U);
    CHECK(metrics.duplicate_rejections == 0U);
    CHECK(metrics.stale_rejections == 0U);
    CHECK(metrics.aborts == 0U);
    CHECK(instance.resume_calls == COMPLETION_REARM_GENERATIONS);
    CHECK(instance.copy_calls == COMPLETION_REARM_GENERATIONS);
    CHECK(leir_aot_completion_destroy(completion) == 0);
    return 0;
}

int main(void) {
    CHECK(test_shared_event_publication_and_direct_consume() == 0);
    CHECK(test_queue_transfer_and_module_defer() == 0);
    CHECK(test_duplicate_and_stale_deliveries() == 0);
    CHECK(test_cancel_and_malformed_failure() == 0);
    CHECK(test_callback_failure_aborts_and_rearms() == 0);
    CHECK(test_repeated_publish_consume_rearm() == 0);
    puts("LEIR common AOT completion tests passed");
    return 0;
}
