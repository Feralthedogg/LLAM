/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"
#include "lccf_representation.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool representation_valid(lccf_representation_t representation) {
    return representation >= LCCF_REP_CANONICAL_HELPER &&
           representation < LCCF_REP_COUNT;
}

static bool source_valid(uint32_t source) {
    return source < (uint32_t)LCCF_FACT_SOURCE_COUNT;
}

static bool event_valid(uint32_t event) {
    return event >= (uint32_t)LCCF_FACT_EVENT_IO &&
           event < (uint32_t)LCCF_FACT_EVENT_COUNT;
}

static int64_t bits_to_i64(uint64_t value) {
    int64_t result;

    memcpy(&result, &value, sizeof(result));
    return result;
}

static void failure_event(const lccf_fact_ticket_t *ticket,
                          lccf_event_core_t *event) {
    memset(event, 0, sizeof(*event));
    event->generation = ticket->generation;
    event->stable_flags = ticket->stable_flags;
    event->result = -1;
    event->error_code = EPROTO;
    event->event_kind = LCCF_FACT_EVENT_FAIL;
    event->source_kind = (uint8_t)ticket->source_kind;
    event->captured_home_shard = ticket->captured_home_shard;
    event->source_node = ticket->source_node;
}

static int normalize_event(const lccf_fact_ticket_t *ticket,
                           lccf_event_core_t *event) {
    int64_t result;
    int32_t error_code;
    bool malformed;

    if (ticket == NULL || event == NULL ||
        ticket->generation > LCCF_FACT_MAX_GENERATION ||
        !source_valid(ticket->source_kind) ||
        !event_valid(ticket->event_kind)) {
        return EINVAL;
    }
    malformed = (ticket->raw_flags & LCCF_FACT_TICKET_MALFORMED) != 0U ||
                (ticket->raw_flags &
                 LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN) != 0U;
    result = ticket->raw_result;
    error_code = ticket->raw_error;

    switch ((lccf_fact_source_t)ticket->source_kind) {
    case LCCF_FACT_SOURCE_LINUX_CQE:
        if (ticket->raw_result < 0) {
            int64_t decoded_error;

            if (ticket->raw_result == INT64_MIN) {
                malformed = true;
            } else {
                decoded_error = -ticket->raw_result;
                if (decoded_error > INT32_MAX) {
                    malformed = true;
                } else {
                    error_code = (int32_t)decoded_error;
                    result = bits_to_i64(ticket->raw_aux);
                }
            }
        } else if (ticket->raw_error != 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_KQUEUE:
        if ((ticket->raw_flags & LCCF_FACT_RAW_KQUEUE_ERROR) != 0U) {
            if (ticket->raw_error <= 0) {
                malformed = true;
            }
        } else if (ticket->raw_error != 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_IOCP:
        if ((ticket->raw_flags & LCCF_FACT_RAW_IOCP_SUCCESS) != 0U) {
            if (ticket->raw_error != 0) {
                malformed = true;
            }
        } else if (ticket->raw_error <= 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_TIMER:
    case LCCF_FACT_SOURCE_CANCEL:
    case LCCF_FACT_SOURCE_EXTERNAL:
    case LCCF_FACT_SOURCE_STOP:
        if (ticket->raw_error < 0) {
            malformed = true;
        }
        break;
    case LCCF_FACT_SOURCE_COUNT:
    default:
        return EINVAL;
    }

    if (malformed) {
        failure_event(ticket, event);
        return 0;
    }
    memset(event, 0, sizeof(*event));
    event->generation = ticket->generation;
    event->stable_flags = ticket->stable_flags;
    event->result = result;
    event->payload_word = ticket->payload_word;
    event->error_code = error_code;
    event->captured_home_shard = ticket->captured_home_shard;
    event->source_node = ticket->source_node;
    event->event_kind = (uint8_t)ticket->event_kind;
    event->source_kind = (uint8_t)ticket->source_kind;
    return 0;
}

static void fact_from_event(const lccf_event_core_t *event,
                            uint32_t site_index,
                            lccf_fact_core_t *fact) {
    memset(fact, 0, sizeof(*fact));
    fact->generation = event->generation;
    fact->stable_flags = event->stable_flags;
    fact->result = event->result;
    fact->payload_word = event->payload_word;
    fact->error_code = event->error_code;
    fact->captured_home_shard = event->captured_home_shard;
    fact->source_node = event->source_node;
    fact->site_index = (uint16_t)site_index;
    fact->event_kind = event->event_kind;
    fact->source_kind = event->source_kind;
}

static void fact_site_failure(const lccf_fact_ticket_t *ticket,
                              const lccf_event_core_t *event,
                              uint32_t site_index,
                              lccf_fact_core_t *fact) {
    fact_from_event(event, site_index, fact);
    fact->result = -1;
    fact->payload_word = 0U;
    fact->resolved_site = NULL;
    fact->error_code = EPROTO;
    fact->event_kind = LCCF_FACT_EVENT_FAIL;
    fact->source_kind = (uint8_t)ticket->source_kind;
    fact->fact_id = lccf_fact_compute_id(fact);
}

static int resolve_event_fact(const lccf_fact_ticket_t *ticket,
                              const lccf_event_core_t *event,
                              uint32_t site_index,
                              lccf_fact_core_t *fact) {
    const lccf_fact_site_descriptor_t *site;

    if (ticket == NULL || event == NULL || fact == NULL ||
        event->generation != ticket->generation ||
        site_index > UINT16_MAX) {
        return EINVAL;
    }
    if (event->event_kind == LCCF_FACT_EVENT_FAIL ||
        (ticket->raw_flags & LCCF_FACT_TICKET_INVALID_SITE) != 0U ||
        ticket->site_table == NULL || site_index >= ticket->site_count) {
        fact_site_failure(ticket, event, site_index, fact);
        return 0;
    }
    site = ticket->site_table[site_index];
    if (site == NULL || site->invoke == NULL ||
        site->logical_index != site_index) {
        fact_site_failure(ticket, event, site_index, fact);
        return 0;
    }
    fact_from_event(event, site_index, fact);
    fact->resolved_site = site;
    fact->fact_id = lccf_fact_compute_id(fact);
    return 0;
}

int lccf_fact_normalize(const lccf_fact_ticket_t *ticket,
                        uint64_t fact_id,
                        lccf_fact_core_t *out_fact) {
    lccf_event_core_t event;
    int rc;

    if (ticket == NULL || out_fact == NULL ||
        ticket->site_count > UINT16_MAX) {
        return EINVAL;
    }
    rc = normalize_event(ticket, &event);
    if (rc != 0) {
        return rc;
    }
    if (ticket->site_index > UINT16_MAX) {
        fact_site_failure(ticket, &event, ticket->site_index, out_fact);
        out_fact->fact_id = fact_id;
        return 0;
    }
    rc = resolve_event_fact(
        ticket, &event, ticket->site_index, out_fact);
    if (rc == 0) {
        out_fact->fact_id = fact_id;
    }
    return rc;
}

size_t lccf_representation_sidecar_bytes(
    lccf_representation_t representation) {
    switch (representation) {
    case LCCF_REP_CANONICAL_HELPER:
        return 0U;
    case LCCF_REP_SHARED_EVENT:
        return sizeof(lccf_event_core_t);
    case LCCF_REP_FULL_FACT:
        return sizeof(lccf_fact_core_t);
    case LCCF_REP_COUNT:
    default:
        return 0U;
    }
}

int lccf_representation_publish(
    lccf_representation_t representation,
    const lccf_fact_ticket_t *ticket,
    void *storage,
    lccf_fact_counters_t *counters) {
    int rc;

    if (!representation_valid(representation) || ticket == NULL ||
        counters == NULL ||
        (representation != LCCF_REP_CANONICAL_HELPER && storage == NULL)) {
        return EINVAL;
    }
    if (representation == LCCF_REP_CANONICAL_HELPER) {
        return 0;
    }
    counters->normalization_calls += UINT64_C(1);
    if (representation == LCCF_REP_SHARED_EVENT) {
        return normalize_event(ticket, storage);
    }
    counters->site_lookups += UINT64_C(1);
    rc = lccf_fact_normalize(ticket, 0U, storage);
    if (rc == 0) {
        lccf_fact_core_t *fact = storage;

        fact->fact_id = lccf_fact_compute_id(fact);
    }
    return rc;
}

int lccf_representation_materialize(
    lccf_representation_t representation,
    const lccf_fact_ticket_t *ticket,
    const void *storage,
    uint32_t site_index,
    lccf_fact_counters_t *counters,
    lccf_fact_core_t *out_fact) {
    int rc;

    if (!representation_valid(representation) || ticket == NULL ||
        counters == NULL || out_fact == NULL || site_index > UINT16_MAX ||
        (representation != LCCF_REP_CANONICAL_HELPER && storage == NULL)) {
        return EINVAL;
    }
    if (representation == LCCF_REP_CANONICAL_HELPER) {
        lccf_fact_ticket_t materialized_ticket = *ticket;

        materialized_ticket.site_index = site_index;
        counters->normalization_calls += UINT64_C(1);
        counters->site_lookups += UINT64_C(1);
        rc = lccf_fact_normalize(&materialized_ticket, 0U, out_fact);
        if (rc == 0) {
            out_fact->fact_id = lccf_fact_compute_id(out_fact);
        }
        return rc;
    }
    if (representation == LCCF_REP_SHARED_EVENT) {
        counters->site_lookups += UINT64_C(1);
        return resolve_event_fact(ticket, storage, site_index, out_fact);
    }

    *out_fact = *(const lccf_fact_core_t *)storage;
    if (out_fact->generation != ticket->generation) {
        return LCCF_FACT_ESTALE;
    }
    if (out_fact->site_index == site_index) {
        return 0;
    }
    counters->site_lookups += UINT64_C(1);
    {
        lccf_event_core_t event;

        event.generation = out_fact->generation;
        event.stable_flags = out_fact->stable_flags;
        event.result = out_fact->result;
        event.payload_word = out_fact->payload_word;
        event.captured_home_shard = out_fact->captured_home_shard;
        event.source_node = out_fact->source_node;
        event.error_code = out_fact->error_code;
        event.event_kind = out_fact->event_kind;
        event.source_kind = out_fact->source_kind;
        event.reserved = 0U;
        return resolve_event_fact(ticket, &event, site_index, out_fact);
    }
}
