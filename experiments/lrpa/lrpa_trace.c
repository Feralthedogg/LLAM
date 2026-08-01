/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

#include <string.h>

void
lrpa_trace_bind(lrpa_trace_t *trace, lrpa_trace_entry_t *entries,
                size_t capacity)
{
    if (trace == NULL) {
        return;
    }
    trace->entries = entries;
    trace->capacity = capacity;
    atomic_init(&trace->next, 0U);
    atomic_init(&trace->truncated, false);
}

bool
lrpa_trace_record(lrpa_trace_t *trace, const lrpa_trace_entry_t *entry)
{
    size_t index;

    if (trace == NULL || entry == NULL) {
        return false;
    }
    index = atomic_fetch_add_explicit(&trace->next, 1U,
                                      memory_order_relaxed);
    if (index >= trace->capacity || trace->entries == NULL) {
        atomic_store_explicit(&trace->truncated, true, memory_order_relaxed);
        return false;
    }
    trace->entries[index] = *entry;
    if (trace->entries[index].sequence == 0U) {
        trace->entries[index].sequence = (uint64_t)index + 1U;
    }
    return true;
}

void
lrpa_trace_event(lrpa_context_t *context, uint32_t lane, uint32_t actor,
                 uint32_t event_kind, uint32_t object_id,
                 uint64_t generation, uint32_t state_before,
                 uint32_t state_after, int32_t result,
                 uintptr_t debug_address)
{
    lrpa_trace_entry_t entry;

    if (context == NULL) {
        return;
    }
    memset(&entry, 0, sizeof(entry));
    entry.timestamp_ns = lrpa_platform_monotonic_ns();
    entry.lane = lane;
    entry.actor = actor;
    entry.event_kind = event_kind;
    entry.object_id = object_id;
    entry.generation = generation;
    entry.state_before = state_before;
    entry.state_after = state_after;
    entry.result = result;
    entry.debug_address = debug_address;
    (void)lrpa_trace_record(&context->trace, &entry);
}
