// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_completion.h"

#include "lccf_fact.h"
#include "lccf_representation.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define LEIR_AOT_COMPLETION_MAGIC UINT64_C(0x4c454952434f4d50)
#define LEIR_AOT_COMPLETION_CELL_EMPTY 0U
#define LEIR_AOT_COMPLETION_CELL_INITIALIZING 1U
#define LEIR_AOT_COMPLETION_CELL_READY 2U

typedef struct leir_aot_completion_atomic_metrics {
    _Atomic uint64_t publications;
    _Atomic uint64_t normalizations;
    _Atomic uint64_t site_lookups;
    _Atomic uint64_t direct_consumes;
    _Atomic uint64_t queued_consumes;
    _Atomic uint64_t duplicate_rejections;
    _Atomic uint64_t stale_rejections;
    _Atomic uint64_t aborts;
} leir_aot_completion_atomic_metrics_t;

struct leir_aot_completion {
    uint64_t magic;
    lccf_fact_cell_t cell;
    lccf_event_core_t event;
    lccf_fact_site_descriptor_t
        sites[LEIR_AOT_COMPLETION_SITE_CAPACITY];
    const lccf_fact_site_descriptor_t
        *site_table[LEIR_AOT_COMPLETION_SITE_CAPACITY];
    const leir_aot_module_v1_t *module;
    void *module_instance;
    size_t module_instance_size;
    _Atomic uint64_t generation;
    _Atomic uint32_t continuation;
    _Atomic uint32_t cell_state;
    _Atomic bool module_available;
    _Atomic bool cancelled;
    _Atomic bool destroyed;
    leir_aot_completion_atomic_metrics_t metrics;
};

typedef struct leir_aot_invocation {
    leir_aot_completion_t *completion;
    leir_phase0_value_t *values_out;
    size_t value_count;
    leir_aot_resume_result_v1_t *resume_out;
} leir_aot_invocation_t;

static bool alignment_is_valid(size_t alignment) {
    return alignment != 0U &&
           (alignment & (alignment - 1U)) == 0U;
}

static bool module_is_valid(const leir_aot_module_v1_t *module) {
    return module != NULL &&
           module->abi_version == LEIR_AOT_MODULE_ABI_V1 &&
           module->struct_size >= sizeof(*module) &&
           module->backend_kind == LEIR_AOT_MODULE_BACKEND_AGNOSTIC &&
           module->semantic_digest != 0U &&
           module->instance_size != 0U &&
           alignment_is_valid(module->instance_alignment) &&
           module->slot_count != 0U &&
           module->bind != NULL &&
           module->prepare != NULL &&
           module->resume != NULL &&
           module->copy_outputs != NULL &&
           module->cancel != NULL;
}

static bool completion_is_live(const leir_aot_completion_t *completion) {
    return completion != NULL &&
           completion->magic == LEIR_AOT_COMPLETION_MAGIC &&
           !atomic_load_explicit(&completion->destroyed,
                                 memory_order_acquire);
}

static int module_status(int rc) {
    if (rc == 0) {
        return 0;
    }
    if (rc > 0) {
        return rc;
    }
    return errno > 0 ? errno : EIO;
}

static void metric_add(_Atomic uint64_t *metric, uint64_t amount) {
    if (amount != 0U) {
        (void)atomic_fetch_add_explicit(
            metric, amount, memory_order_relaxed);
    }
}

static void accumulate_fact_metrics(
    leir_aot_completion_t *completion,
    const lccf_fact_counters_t *counters) {
    metric_add(&completion->metrics.normalizations,
               counters->normalization_calls);
    metric_add(&completion->metrics.site_lookups,
               counters->site_lookups);
}

static int invoke_module(
    const lccf_fact_site_descriptor_t *descriptor,
    const lccf_fact_core_t *fact,
    void *context) {
    leir_aot_invocation_t *invocation = context;
    leir_aot_completion_t *completion;
    int64_t result;
    int rc;

    if (descriptor == NULL || fact == NULL || invocation == NULL ||
        descriptor->logical_index != fact->site_index) {
        return EPROTO;
    }
    completion = invocation->completion;
    if (!completion_is_live(completion) ||
        descriptor->logical_index >= LEIR_AOT_COMPLETION_SITE_CAPACITY) {
        return EPROTO;
    }
    result = fact->error_code == 0
                 ? fact->result
                 : -(int64_t)fact->error_code;
    errno = 0;
    rc = module_status(completion->module->resume(
        completion->module_instance, descriptor->logical_index,
        result, invocation->resume_out));
    if (rc != 0) {
        return rc;
    }
    errno = 0;
    return module_status(completion->module->copy_outputs(
        completion->module_instance, invocation->values_out,
        invocation->value_count));
}

static int abort_generation(leir_aot_completion_t *completion,
                            uint64_t generation, int result) {
    const int abort_rc = lccf_fact_abort(&completion->cell, generation);

    if (abort_rc == 0) {
        metric_add(&completion->metrics.aborts, 1U);
        return result;
    }
    return abort_rc;
}

size_t leir_aot_completion_size(void) {
    return sizeof(leir_aot_completion_t);
}

size_t leir_aot_completion_alignment(void) {
    return _Alignof(leir_aot_completion_t);
}

int leir_aot_completion_init(
    void *storage,
    size_t storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size) {
    leir_aot_completion_t *completion = storage;
    size_t index;

    if (storage == NULL ||
        (uintptr_t)storage % _Alignof(leir_aot_completion_t) != 0U ||
        storage_size < sizeof(*completion) || !module_is_valid(module) ||
        module_instance == NULL ||
        (uintptr_t)module_instance % module->instance_alignment != 0U ||
        module_instance_size < module->instance_size) {
        return EINVAL;
    }
    memset(completion, 0, sizeof(*completion));
    completion->module = module;
    completion->module_instance = module_instance;
    completion->module_instance_size = module_instance_size;
    for (index = 0U;
         index < LEIR_AOT_COMPLETION_SITE_CAPACITY;
         ++index) {
        completion->sites[index].invoke = invoke_module;
        completion->sites[index].logical_index = (uint32_t)index;
        completion->site_table[index] = &completion->sites[index];
    }
    atomic_init(&completion->generation, 0U);
    atomic_init(&completion->continuation, 0U);
    atomic_init(&completion->cell_state, LEIR_AOT_COMPLETION_CELL_EMPTY);
    atomic_init(&completion->module_available, true);
    atomic_init(&completion->cancelled, false);
    atomic_init(&completion->destroyed, false);
    atomic_init(&completion->metrics.publications, 0U);
    atomic_init(&completion->metrics.normalizations, 0U);
    atomic_init(&completion->metrics.site_lookups, 0U);
    atomic_init(&completion->metrics.direct_consumes, 0U);
    atomic_init(&completion->metrics.queued_consumes, 0U);
    atomic_init(&completion->metrics.duplicate_rejections, 0U);
    atomic_init(&completion->metrics.stale_rejections, 0U);
    atomic_init(&completion->metrics.aborts, 0U);
    completion->magic = LEIR_AOT_COMPLETION_MAGIC;
    return 0;
}

int leir_aot_completion_arm(leir_aot_completion_t *completion,
                            uint64_t generation) {
    uint32_t expected = LEIR_AOT_COMPLETION_CELL_EMPTY;
    int rc;

    if (!completion_is_live(completion) ||
        generation > LCCF_FACT_MAX_GENERATION) {
        return EINVAL;
    }
    if (atomic_compare_exchange_strong_explicit(
            &completion->cell_state, &expected,
            LEIR_AOT_COMPLETION_CELL_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire)) {
        rc = lccf_fact_cell_init_representation(
            &completion->cell, generation,
            LCCF_FACT_LAYOUT_SPLIT64_64, 1U,
            LCCF_REP_SHARED_EVENT, &completion->event);
        if (rc != 0) {
            atomic_store_explicit(
                &completion->cell_state,
                LEIR_AOT_COMPLETION_CELL_EMPTY,
                memory_order_release);
            return rc;
        }
        atomic_store_explicit(&completion->cell_state,
                              LEIR_AOT_COMPLETION_CELL_READY,
                              memory_order_release);
    } else if (expected == LEIR_AOT_COMPLETION_CELL_INITIALIZING) {
        return EBUSY;
    } else {
        rc = lccf_fact_cell_arm(&completion->cell, generation, 1U);
        if (rc != 0) {
            return rc;
        }
    }
    atomic_store_explicit(&completion->continuation, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&completion->cancelled, false,
                          memory_order_relaxed);
    atomic_store_explicit(&completion->generation, generation,
                          memory_order_release);
    return 0;
}

int leir_aot_completion_publish(
    leir_aot_completion_t *completion,
    const leir_aot_completion_record_t *record) {
    lccf_fact_ticket_t ticket;
    lccf_fact_counters_t counters = {0};
    const uint64_t current_generation = completion_is_live(completion)
                                            ? atomic_load_explicit(
                                                  &completion->generation,
                                                  memory_order_acquire)
                                            : 0U;
    bool won = false;
    int rc;

    if (!completion_is_live(completion) || record == NULL ||
        atomic_load_explicit(&completion->cell_state,
                             memory_order_acquire) !=
            LEIR_AOT_COMPLETION_CELL_READY ||
        record->continuation >= LEIR_AOT_COMPLETION_SITE_CAPACITY) {
        return EINVAL;
    }
    rc = lccf_fact_ticket_from_logical(
        (lccf_fact_source_t)record->source_kind,
        (lccf_fact_event_kind_t)record->event_kind,
        record->generation, record->result, record->error_code,
        record->payload_word, record->continuation,
        LEIR_AOT_COMPLETION_SITE_CAPACITY,
        record->captured_home_shard, record->source_node, 0U,
        completion->site_table, &ticket);
    if (rc != 0) {
        return rc;
    }
    ticket.stable_flags = record->stable_flags;
    rc = lccf_fact_try_publish_configured(
        &completion->cell, &ticket, &counters, &won);
    accumulate_fact_metrics(completion, &counters);
    if (rc == LCCF_FACT_ESTALE) {
        if (record->generation == current_generation) {
            metric_add(&completion->metrics.duplicate_rejections, 1U);
            return EBUSY;
        }
        metric_add(&completion->metrics.stale_rejections, 1U);
        return LCCF_FACT_ESTALE;
    }
    if (rc != 0) {
        return rc;
    }
    if (!won) {
        metric_add(&completion->metrics.duplicate_rejections, 1U);
        return EBUSY;
    }
    atomic_store_explicit(&completion->continuation,
                          record->continuation, memory_order_release);
    metric_add(&completion->metrics.publications, 1U);
    return 0;
}

int leir_aot_completion_consume(
    leir_aot_completion_t *completion,
    uint64_t generation,
    leir_aot_completion_route_t route,
    uint64_t guard_flags,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out) {
    lccf_fact_guard_t guard;
    lccf_fact_counters_t counters = {0};
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    leir_aot_invocation_t invocation;
    uint64_t state_word;
    uint32_t continuation;
    int rc;

    if (!completion_is_live(completion) || values_out == NULL ||
        value_count != completion->module->slot_count ||
        resume_out == NULL ||
        atomic_load_explicit(&completion->cell_state,
                             memory_order_acquire) !=
            LEIR_AOT_COMPLETION_CELL_READY ||
        (route != LEIR_AOT_COMPLETION_DIRECT &&
         route != LEIR_AOT_COMPLETION_QUEUE)) {
        return EINVAL;
    }
    if (generation != atomic_load_explicit(
                          &completion->generation,
                          memory_order_acquire)) {
        metric_add(&completion->metrics.stale_rejections, 1U);
        return LCCF_FACT_ESTALE;
    }
    state_word = atomic_load_explicit(&completion->cell.state_generation,
                                      memory_order_acquire);
    if (lccf_fact_unpack_generation(state_word) != generation) {
        metric_add(&completion->metrics.stale_rejections, 1U);
        return LCCF_FACT_ESTALE;
    }
    if (lccf_fact_unpack_state(state_word) == LCCF_FACT_STATE_TERMINAL) {
        metric_add(&completion->metrics.duplicate_rejections, 1U);
        return EBUSY;
    }
    continuation = atomic_load_explicit(
        &completion->continuation, memory_order_acquire);
    if (continuation >= LEIR_AOT_COMPLETION_SITE_CAPACITY) {
        return EPROTO;
    }
    memset(resume_out, 0, sizeof(*resume_out));
    memset(&guard, 0, sizeof(guard));
    guard.flags = guard_flags & ~LCCF_FACT_GUARD_MODULE_ENABLED;
    if (atomic_load_explicit(&completion->module_available,
                             memory_order_acquire)) {
        guard.flags |= LCCF_FACT_GUARD_MODULE_ENABLED;
    }
    guard.budget_remaining = UINT64_MAX;
    guard.current_home_shard = 0U;
    guard.consuming_shard = 0U;
    rc = lccf_fact_consume(
        &completion->cell, generation, continuation,
        route == LEIR_AOT_COMPLETION_DIRECT
            ? LCCF_FACT_CONSUMER_DIRECT
            : LCCF_FACT_CONSUMER_QUEUE,
        &guard, &counters, &decision, &fact);
    accumulate_fact_metrics(completion, &counters);
    if (rc == LCCF_FACT_ESTALE) {
        metric_add(&completion->metrics.stale_rejections, 1U);
        return rc;
    }
    if (rc == EPROTO) {
        metric_add(&completion->metrics.duplicate_rejections, 1U);
        return EBUSY;
    }
    if (rc != 0) {
        return rc;
    }
    if ((route == LEIR_AOT_COMPLETION_DIRECT &&
         decision.route == LCCF_FACT_ROUTE_QUEUE) ||
        decision.route == LCCF_FACT_ROUTE_FORWARD ||
        decision.route == LCCF_FACT_ROUTE_DEFER) {
        return EAGAIN;
    }
    if (fact.event_kind == LCCF_FACT_EVENT_FAIL &&
        fact.resolved_site == NULL) {
        fact.resolved_site = completion->site_table[continuation];
    }
    invocation.completion = completion;
    invocation.values_out = values_out;
    invocation.value_count = value_count;
    invocation.resume_out = resume_out;
    rc = lccf_fact_invoke(&fact, &invocation);
    if (rc != 0) {
        return abort_generation(completion, generation, rc);
    }
    memset(&counters, 0, sizeof(counters));
    rc = lccf_fact_finish(&completion->cell, generation, &counters);
    accumulate_fact_metrics(completion, &counters);
    if (rc != 0) {
        return abort_generation(completion, generation, rc);
    }
    if (route == LEIR_AOT_COMPLETION_DIRECT) {
        metric_add(&completion->metrics.direct_consumes, 1U);
    } else {
        metric_add(&completion->metrics.queued_consumes, 1U);
    }
    return 0;
}

int leir_aot_completion_cancel(leir_aot_completion_t *completion,
                               uint32_t continuation) {
    leir_aot_completion_record_t record;
    uint64_t state_word;
    uint64_t generation;
    bool already_cancelled;
    int rc;

    if (!completion_is_live(completion) ||
        continuation >= LEIR_AOT_COMPLETION_SITE_CAPACITY) {
        return EINVAL;
    }
    already_cancelled = atomic_exchange_explicit(
        &completion->cancelled, true, memory_order_acq_rel);
    if (!already_cancelled) {
        completion->module->cancel(completion->module_instance);
    }
    if (atomic_load_explicit(&completion->cell_state,
                             memory_order_acquire) !=
        LEIR_AOT_COMPLETION_CELL_READY) {
        return 0;
    }
    state_word = atomic_load_explicit(&completion->cell.state_generation,
                                      memory_order_acquire);
    generation = lccf_fact_unpack_generation(state_word);
    if (lccf_fact_unpack_state(state_word) != LCCF_FACT_STATE_ARMED) {
        return lccf_fact_unpack_state(state_word) ==
                       LCCF_FACT_STATE_TERMINAL
                   ? EBUSY
                   : 0;
    }
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.result = -(int64_t)ECANCELED;
    record.continuation = continuation;
    record.source_kind = LCCF_FACT_SOURCE_CANCEL;
    record.event_kind = LCCF_FACT_EVENT_CANCEL;
    record.error_code = ECANCELED;
    rc = leir_aot_completion_publish(completion, &record);
    return rc == EBUSY ? 0 : rc;
}

int leir_aot_completion_set_module_available(
    leir_aot_completion_t *completion,
    bool available) {
    if (!completion_is_live(completion)) {
        return EINVAL;
    }
    atomic_store_explicit(&completion->module_available, available,
                          memory_order_release);
    return 0;
}

int leir_aot_completion_destroy(leir_aot_completion_t *completion) {
    bool expected = false;
    uint32_t cell_state;

    if (!completion_is_live(completion)) {
        return EINVAL;
    }
    cell_state = atomic_load_explicit(&completion->cell_state,
                                      memory_order_acquire);
    if (cell_state == LEIR_AOT_COMPLETION_CELL_INITIALIZING) {
        return EBUSY;
    }
    if (cell_state == LEIR_AOT_COMPLETION_CELL_READY &&
        !lccf_fact_can_reuse(&completion->cell)) {
        return EBUSY;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &completion->destroyed, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return EBUSY;
    }
    return 0;
}

void leir_aot_completion_metrics(
    const leir_aot_completion_t *completion,
    leir_aot_completion_metrics_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (completion == NULL ||
        completion->magic != LEIR_AOT_COMPLETION_MAGIC) {
        return;
    }
    out->publications = atomic_load_explicit(
        &completion->metrics.publications, memory_order_relaxed);
    out->normalizations = atomic_load_explicit(
        &completion->metrics.normalizations, memory_order_relaxed);
    out->site_lookups = atomic_load_explicit(
        &completion->metrics.site_lookups, memory_order_relaxed);
    out->direct_consumes = atomic_load_explicit(
        &completion->metrics.direct_consumes, memory_order_relaxed);
    out->queued_consumes = atomic_load_explicit(
        &completion->metrics.queued_consumes, memory_order_relaxed);
    out->duplicate_rejections = atomic_load_explicit(
        &completion->metrics.duplicate_rejections, memory_order_relaxed);
    out->stale_rejections = atomic_load_explicit(
        &completion->metrics.stale_rejections, memory_order_relaxed);
    out->aborts = atomic_load_explicit(
        &completion->metrics.aborts, memory_order_relaxed);
}
