/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LCCF_REPRESENTATION_H
#define LLAM_EXPERIMENTS_LCCF_REPRESENTATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lccf_fact_core;
struct lccf_fact_counters;
struct lccf_fact_ticket;

typedef enum lccf_representation {
    LCCF_REP_CANONICAL_HELPER = 0,
    LCCF_REP_SHARED_EVENT,
    LCCF_REP_FULL_FACT,
    LCCF_REP_COUNT
} lccf_representation_t;

typedef struct lccf_event_core {
    uint64_t generation;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    uint32_t captured_home_shard;
    uint32_t source_node;
    int32_t error_code;
    uint8_t event_kind;
    uint8_t source_kind;
    uint16_t reserved;
} lccf_event_core_t;

_Static_assert(sizeof(lccf_event_core_t) == 48U,
               "LCCF event core must occupy 48 bytes");

size_t lccf_representation_sidecar_bytes(
    lccf_representation_t representation);
int lccf_representation_publish(
    lccf_representation_t representation,
    const struct lccf_fact_ticket *ticket,
    void *storage,
    struct lccf_fact_counters *counters);
int lccf_representation_materialize(
    lccf_representation_t representation,
    const struct lccf_fact_ticket *ticket,
    const void *storage,
    uint32_t site_index,
    struct lccf_fact_counters *counters,
    struct lccf_fact_core *out_fact);

#ifdef __cplusplus
}
#endif

#endif
