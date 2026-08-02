// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_portable.h"

#include "leir_aot_completion.h"

#include "lccf_fact.h"
#include "llam/io.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define LEIR_AOT_PORTABLE_TICKET_MAGIC UINT64_C(0x4c45495250525431)

enum {
    LEIR_AOT_PORTABLE_INITIALIZED = 1U,
    LEIR_AOT_PORTABLE_BINDING = 2U,
    LEIR_AOT_PORTABLE_BOUND = 3U,
    LEIR_AOT_PORTABLE_BOUND_CANCELLED = 4U,
    LEIR_AOT_PORTABLE_RUNNING = 5U,
    LEIR_AOT_PORTABLE_RUNNING_CANCELLED = 6U,
    LEIR_AOT_PORTABLE_COMPLETING = 7U,
    LEIR_AOT_PORTABLE_CONSUMED = 8U,
    LEIR_AOT_PORTABLE_DESTROYED = 9U,
};

struct leir_aot_portable_ticket {
    uint64_t magic;
    const leir_aot_module_v1_t *module;
    void *module_instance;
    size_t module_instance_size;
    leir_aot_portable_effects_t effects;
    llam_fd_t fd;
    const struct sockaddr *address;
    socklen_t address_length;
    const void *payload;
    size_t payload_length;
    uint64_t generation;
    uint32_t connect_error_continuation;
    uint32_t write_continuation;
    bool prepared;
    atomic_uint state;
    max_align_t completion_alignment;
    unsigned char completion_storage[];
};

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

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

static leir_aot_completion_t *ticket_completion(
    leir_aot_portable_ticket_t *ticket) {
    return (leir_aot_completion_t *)(void *)ticket->completion_storage;
}

static const leir_aot_completion_t *ticket_completion_const(
    const leir_aot_portable_ticket_t *ticket) {
    return (const leir_aot_completion_t *)(const void *)
        ticket->completion_storage;
}

static bool ticket_is_valid(const leir_aot_portable_ticket_t *ticket) {
    return ticket != NULL &&
           ticket->magic == LEIR_AOT_PORTABLE_TICKET_MAGIC &&
           module_is_valid(ticket->module) &&
           ticket->module_instance != NULL &&
           ticket->module_instance_size >= ticket->module->instance_size &&
           ticket->effects.connect != NULL &&
           ticket->effects.write != NULL;
}

static int default_connect(void *context, llam_fd_t fd,
                           const struct sockaddr *address,
                           socklen_t address_length) {
    (void)context;
    return llam_connect(fd, address, address_length);
}

static ssize_t default_write(void *context, llam_fd_t fd,
                             const void *payload,
                             size_t payload_length) {
    (void)context;
    return llam_write(fd, payload, payload_length);
}

static bool range_is_inside_module_instance(
    const leir_aot_portable_ticket_t *ticket,
    const void *pointer,
    size_t length) {
    const uintptr_t base = (uintptr_t)ticket->module_instance;
    const uintptr_t candidate = (uintptr_t)pointer;
    size_t offset;

    if (pointer == NULL || candidate < base) {
        return false;
    }
    offset = (size_t)(candidate - base);
    return offset <= ticket->module_instance_size &&
           length <= ticket->module_instance_size - offset;
}

static int prepare_connect_write(
    void *context,
    llam_fd_t fd,
    const struct sockaddr *address,
    socklen_t address_length,
    const void *payload,
    size_t payload_length,
    uint64_t generation,
    uint32_t connect_error_continuation,
    uint32_t write_continuation) {
    leir_aot_portable_ticket_t *ticket = context;
    unsigned state;

    if (!ticket_is_valid(ticket)) {
        return fail_with_errno(EINVAL);
    }
    state = atomic_load_explicit(&ticket->state, memory_order_acquire);
    if ((state != LEIR_AOT_PORTABLE_RUNNING &&
         state != LEIR_AOT_PORTABLE_RUNNING_CANCELLED) ||
        ticket->prepared || LLAM_FD_IS_INVALID(fd) ||
        address_length == 0U ||
        address_length > (socklen_t)sizeof(struct sockaddr_storage) ||
        !range_is_inside_module_instance(
            ticket, address, (size_t)address_length) ||
        (payload_length != 0U && payload == NULL) ||
        generation == 0U ||
        connect_error_continuation == write_continuation ||
        connect_error_continuation >=
            LEIR_AOT_COMPLETION_SITE_CAPACITY ||
        write_continuation >= LEIR_AOT_COMPLETION_SITE_CAPACITY) {
        return fail_with_errno(EINVAL);
    }
    ticket->fd = fd;
    ticket->address = address;
    ticket->address_length = address_length;
    ticket->payload = payload;
    ticket->payload_length = payload_length;
    ticket->generation = generation;
    ticket->connect_error_continuation =
        connect_error_continuation;
    ticket->write_continuation = write_continuation;
    ticket->prepared = true;
    return 0;
}

static int publish_record(leir_aot_portable_ticket_t *ticket,
                          uint32_t continuation,
                          int64_t result,
                          int error_code) {
    leir_aot_completion_record_t record;

    memset(&record, 0, sizeof(record));
    record.generation = ticket->generation;
    record.result = result;
    record.continuation = continuation;
    record.source_kind = LCCF_FACT_SOURCE_EXTERNAL;
    record.event_kind = LCCF_FACT_EVENT_IO;
    record.error_code = error_code;
    return leir_aot_completion_publish(
        ticket_completion(ticket), &record);
}

static void copy_completion_metrics(
    const leir_aot_completion_metrics_t *before,
    const leir_aot_completion_metrics_t *after,
    leir_aot_portable_metrics_t *out) {
    out->terminal_publications =
        after->publications - before->publications;
    out->normalizations =
        after->normalizations - before->normalizations;
    out->site_lookups = after->site_lookups - before->site_lookups;
}

size_t leir_aot_portable_ticket_size(void) {
    return offsetof(leir_aot_portable_ticket_t, completion_storage) +
           leir_aot_completion_size();
}

size_t leir_aot_portable_ticket_alignment(void) {
    return _Alignof(leir_aot_portable_ticket_t);
}

int leir_aot_portable_ticket_init(
    void *ticket_storage,
    size_t ticket_storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance,
    size_t module_instance_size,
    const leir_aot_portable_effects_t *effects) {
    leir_aot_portable_ticket_t *ticket = ticket_storage;
    int rc;

    if (ticket_storage == NULL ||
        (uintptr_t)ticket_storage %
                _Alignof(leir_aot_portable_ticket_t) !=
            0U ||
        ticket_storage_size < leir_aot_portable_ticket_size() ||
        !module_is_valid(module) || module_instance == NULL ||
        (uintptr_t)module_instance % module->instance_alignment != 0U ||
        module_instance_size < module->instance_size ||
        (effects != NULL &&
         (effects->connect == NULL || effects->write == NULL))) {
        return fail_with_errno(EINVAL);
    }
    memset(ticket, 0, leir_aot_portable_ticket_size());
    ticket->magic = LEIR_AOT_PORTABLE_TICKET_MAGIC;
    ticket->module = module;
    ticket->module_instance = module_instance;
    ticket->module_instance_size = module_instance_size;
    if (effects == NULL) {
        ticket->effects.connect = default_connect;
        ticket->effects.write = default_write;
    } else {
        ticket->effects = *effects;
    }
    atomic_init(&ticket->state, LEIR_AOT_PORTABLE_INITIALIZED);
    rc = leir_aot_completion_init(
        ticket->completion_storage, leir_aot_completion_size(),
        module, module_instance, module_instance_size);
    if (rc != 0) {
        ticket->magic = 0U;
        return fail_with_errno(rc);
    }
    errno = 0;
    return 0;
}

int leir_aot_portable_ticket_bind(
    leir_aot_portable_ticket_t *ticket,
    const leir_phase0_value_t *values,
    size_t value_count) {
    unsigned expected;
    unsigned prior;
    int rc;

    if (!ticket_is_valid(ticket) || values == NULL ||
        value_count != ticket->module->slot_count) {
        return fail_with_errno(EINVAL);
    }
    expected = atomic_load_explicit(&ticket->state,
                                    memory_order_acquire);
    for (;;) {
        if (expected != LEIR_AOT_PORTABLE_INITIALIZED &&
            expected != LEIR_AOT_PORTABLE_CONSUMED) {
            return fail_with_errno(
                expected == LEIR_AOT_PORTABLE_DESTROYED
                    ? EINVAL
                    : EBUSY);
        }
        prior = expected;
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state, &expected,
                LEIR_AOT_PORTABLE_BINDING,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    ticket->prepared = false;
    errno = 0;
    rc = ticket->module->bind(
        ticket->module_instance, ticket->module_instance_size,
        values, value_count);
    atomic_store_explicit(
        &ticket->state,
        rc == 0 ? LEIR_AOT_PORTABLE_BOUND : prior,
        memory_order_release);
    return rc;
}

int leir_aot_portable_ticket_run(
    leir_aot_portable_ticket_t *ticket,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out,
    leir_aot_portable_metrics_t *metrics_out) {
    leir_aot_completion_metrics_t metrics_before;
    leir_aot_completion_metrics_t metrics_after;
    leir_aot_backend_v1_t backend;
    unsigned expected;
    unsigned desired;
    bool cancelled;
    uint32_t continuation = 0U;
    int64_t semantic_result = 0;
    ssize_t write_result;
    int error_code = 0;
    int saved_errno = 0;
    int rc;

    if (!ticket_is_valid(ticket) || values_out == NULL ||
        value_count != ticket->module->slot_count ||
        resume_out == NULL || metrics_out == NULL) {
        return fail_with_errno(EINVAL);
    }
    expected = atomic_load_explicit(&ticket->state,
                                    memory_order_acquire);
    for (;;) {
        if (expected != LEIR_AOT_PORTABLE_BOUND &&
            expected != LEIR_AOT_PORTABLE_BOUND_CANCELLED) {
            return fail_with_errno(
                expected == LEIR_AOT_PORTABLE_DESTROYED
                    ? EINVAL
                    : EBUSY);
        }
        desired = expected == LEIR_AOT_PORTABLE_BOUND_CANCELLED
                      ? LEIR_AOT_PORTABLE_RUNNING_CANCELLED
                      : LEIR_AOT_PORTABLE_RUNNING;
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state, &expected, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    memset(resume_out, 0, sizeof(*resume_out));
    memset(metrics_out, 0, sizeof(*metrics_out));
    memset(&backend, 0, sizeof(backend));
    backend.abi_version = LEIR_AOT_MODULE_ABI_V1;
    backend.struct_size = sizeof(backend);
    backend.backend_kind = LEIR_AOT_BACKEND_PORTABLE;
    backend.context = ticket;
    backend.prepare_connect_write = prepare_connect_write;
    ticket->prepared = false;
    errno = 0;
    rc = ticket->module->prepare(ticket->module_instance, &backend);
    saved_errno = errno;
    if (rc != 0 || !ticket->prepared) {
        saved_errno = saved_errno != 0 ? saved_errno : EPROTO;
        goto fail;
    }
    metrics_out->activations = 1U;
    metrics_out->logical_operations = 2U;
    metrics_out->generation = ticket->generation;
    rc = leir_aot_completion_arm(
        ticket_completion(ticket), ticket->generation);
    if (rc != 0) {
        saved_errno = rc;
        goto fail;
    }
    leir_aot_completion_metrics(
        ticket_completion_const(ticket), &metrics_before);

    if (atomic_load_explicit(&ticket->state, memory_order_acquire) ==
        LEIR_AOT_PORTABLE_RUNNING) {
        errno = 0;
        rc = ticket->effects.connect(
            ticket->effects.context, ticket->fd,
            ticket->address, ticket->address_length);
        saved_errno = errno;
        metrics_out->effect_calls += 1U;
        if (atomic_load_explicit(&ticket->state,
                                 memory_order_acquire) ==
            LEIR_AOT_PORTABLE_RUNNING_CANCELLED) {
            continuation = ticket->connect_error_continuation;
        } else if (rc != 0) {
            continuation = ticket->connect_error_continuation;
            error_code = saved_errno != 0 ? saved_errno : EIO;
            semantic_result = -(int64_t)error_code;
        } else {
            errno = 0;
            write_result = ticket->effects.write(
                ticket->effects.context, ticket->fd,
                ticket->payload, ticket->payload_length);
            saved_errno = errno;
            metrics_out->effect_calls += 1U;
            continuation = ticket->write_continuation;
            if (write_result < 0) {
                error_code = saved_errno != 0 ? saved_errno : EIO;
                semantic_result = -(int64_t)error_code;
            } else if (write_result == 0 &&
                       ticket->payload_length != 0U) {
                error_code = EIO;
                semantic_result = -(int64_t)error_code;
            } else if ((uint64_t)write_result >
                       (uint64_t)ticket->payload_length) {
                error_code = EPROTO;
                semantic_result = -(int64_t)error_code;
            } else {
                semantic_result = (int64_t)write_result;
            }
        }
    }

    expected = atomic_load_explicit(&ticket->state,
                                    memory_order_acquire);
    for (;;) {
        if (expected != LEIR_AOT_PORTABLE_RUNNING &&
            expected != LEIR_AOT_PORTABLE_RUNNING_CANCELLED) {
            saved_errno = EBUSY;
            goto fail;
        }
        cancelled = expected == LEIR_AOT_PORTABLE_RUNNING_CANCELLED;
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state, &expected,
                LEIR_AOT_PORTABLE_COMPLETING,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    if (cancelled) {
        continuation = ticket->connect_error_continuation;
        rc = leir_aot_completion_cancel(
            ticket_completion(ticket), continuation);
    } else {
        rc = publish_record(ticket, continuation,
                            semantic_result, error_code);
    }
    if (rc != 0) {
        saved_errno = rc;
        goto fail;
    }

    metrics_out->resumed_continuation = continuation;
    rc = leir_aot_completion_consume(
        ticket_completion(ticket), ticket->generation,
        LEIR_AOT_COMPLETION_DIRECT,
        LEIR_AOT_COMPLETION_GUARD_DIRECT_ENABLED |
            LEIR_AOT_COMPLETION_GUARD_BACKEND_CAPABLE,
        values_out, value_count, resume_out);
    if (rc != 0) {
        saved_errno = rc;
        goto fail;
    }
    leir_aot_completion_metrics(
        ticket_completion_const(ticket), &metrics_after);
    copy_completion_metrics(&metrics_before, &metrics_after,
                            metrics_out);
    atomic_store_explicit(&ticket->state, LEIR_AOT_PORTABLE_CONSUMED,
                          memory_order_release);
    errno = 0;
    return 0;

fail:
    atomic_store_explicit(&ticket->state, LEIR_AOT_PORTABLE_CONSUMED,
                          memory_order_release);
    return fail_with_errno(saved_errno != 0 ? saved_errno : EPROTO);
}

int leir_aot_portable_ticket_cancel(
    leir_aot_portable_ticket_t *ticket) {
    unsigned state;
    unsigned desired;

    if (!ticket_is_valid(ticket)) {
        return fail_with_errno(EINVAL);
    }
    state = atomic_load_explicit(&ticket->state, memory_order_acquire);
    for (;;) {
        if (state == LEIR_AOT_PORTABLE_BOUND_CANCELLED ||
            state == LEIR_AOT_PORTABLE_RUNNING_CANCELLED) {
            errno = 0;
            return 0;
        }
        if (state == LEIR_AOT_PORTABLE_BOUND) {
            desired = LEIR_AOT_PORTABLE_BOUND_CANCELLED;
        } else if (state == LEIR_AOT_PORTABLE_RUNNING) {
            desired = LEIR_AOT_PORTABLE_RUNNING_CANCELLED;
        } else {
            return fail_with_errno(
                state == LEIR_AOT_PORTABLE_DESTROYED
                    ? EINVAL
                    : EBUSY);
        }
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state, &state, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            errno = 0;
            return 0;
        }
    }
}

int leir_aot_portable_ticket_destroy(
    leir_aot_portable_ticket_t *ticket) {
    unsigned state;
    int rc;

    if (!ticket_is_valid(ticket)) {
        return fail_with_errno(EINVAL);
    }
    state = atomic_load_explicit(&ticket->state, memory_order_acquire);
    for (;;) {
        if (state == LEIR_AOT_PORTABLE_RUNNING ||
            state == LEIR_AOT_PORTABLE_RUNNING_CANCELLED ||
            state == LEIR_AOT_PORTABLE_COMPLETING ||
            state == LEIR_AOT_PORTABLE_BINDING) {
            return fail_with_errno(EBUSY);
        }
        if (state == LEIR_AOT_PORTABLE_DESTROYED) {
            return fail_with_errno(EINVAL);
        }
        if (atomic_compare_exchange_weak_explicit(
                &ticket->state, &state,
                LEIR_AOT_PORTABLE_DESTROYED,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    if (state == LEIR_AOT_PORTABLE_BOUND ||
        state == LEIR_AOT_PORTABLE_BOUND_CANCELLED) {
        ticket->module->cancel(ticket->module_instance);
    }
    rc = leir_aot_completion_destroy(ticket_completion(ticket));
    if (rc != 0) {
        atomic_store_explicit(&ticket->state, state,
                              memory_order_release);
        return fail_with_errno(rc);
    }
    ticket->prepared = false;
    ticket->module = NULL;
    ticket->module_instance = NULL;
    ticket->module_instance_size = 0U;
    ticket->magic = 0U;
    errno = 0;
    return 0;
}
