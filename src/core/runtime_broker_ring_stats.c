/**
 * @file src/core/runtime_broker_ring_stats.c
 * @brief Broker ring diagnostic counters.
 *
 * @details
 * These counters are intentionally diagnostic: the ring is client-visible, so
 * authority decisions must never depend on them. They exist to expose queue
 * pressure, cursor publication rate, batch drain size, and broker serve
 * latency when diagnosing shared-ring performance regressions.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"
#include "runtime_broker_ring.h"

#include <string.h>

void llam_broker_ring_client_stat_add(llam_broker_ring_t *ring,
                                      llam_broker_ring_client_stat_t stat,
                                      uint64_t delta) {
    atomic_fetch_add_explicit(&ring->client_stats.values[(size_t)stat], delta, memory_order_relaxed);
}

void llam_broker_ring_client_stat_max(llam_broker_ring_t *ring,
                                      llam_broker_ring_client_stat_t stat,
                                      uint64_t value) {
    _Atomic uint64_t *slot = &ring->client_stats.values[(size_t)stat];
    uint64_t current = atomic_load_explicit(slot, memory_order_relaxed);

    while (current < value &&
           !atomic_compare_exchange_weak_explicit(slot,
                                                  &current,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

void llam_broker_ring_broker_stat_add(llam_broker_ring_t *ring,
                                      llam_broker_ring_broker_stat_t stat,
                                      uint64_t delta) {
    atomic_fetch_add_explicit(&ring->broker_stats.values[(size_t)stat], delta, memory_order_relaxed);
}

void llam_broker_ring_broker_stat_max(llam_broker_ring_t *ring,
                                      llam_broker_ring_broker_stat_t stat,
                                      uint64_t value) {
    _Atomic uint64_t *slot = &ring->broker_stats.values[(size_t)stat];
    uint64_t current = atomic_load_explicit(slot, memory_order_relaxed);

    while (current < value &&
           !atomic_compare_exchange_weak_explicit(slot,
                                                  &current,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

int llam_broker_ring_collect_stats(const llam_broker_ring_t *ring, llam_broker_ring_stats_t *out_stats) {
    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_stats == NULL)) {
        errno = EINVAL;
        return -1;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->client_submit_pushes =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_PUSHES],
                             memory_order_relaxed);
    out_stats->client_submit_full =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_FULL],
                             memory_order_relaxed);
    out_stats->client_submit_tail_publishes =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_TAIL_PUBLISHES],
                             memory_order_relaxed);
    out_stats->client_complete_drain_calls =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_CALLS],
                             memory_order_relaxed);
    out_stats->client_complete_drain_entries =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_ENTRIES],
                             memory_order_relaxed);
    out_stats->client_complete_empty =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_EMPTY],
                             memory_order_relaxed);
    out_stats->client_complete_head_publishes =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_HEAD_PUBLISHES],
                             memory_order_relaxed);
    out_stats->client_complete_batch_max =
        atomic_load_explicit(&ring->client_stats.values[LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_BATCH_MAX],
                             memory_order_relaxed);
    out_stats->broker_serve_calls =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SERVE_CALLS],
                             memory_order_relaxed);
    out_stats->broker_serve_success =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SERVE_SUCCESS],
                             memory_order_relaxed);
    out_stats->broker_submit_empty =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SUBMIT_EMPTY],
                             memory_order_relaxed);
    out_stats->broker_complete_full =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_COMPLETE_FULL],
                             memory_order_relaxed);
    out_stats->broker_submit_head_publishes =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SUBMIT_HEAD_PUBLISHES],
                             memory_order_relaxed);
    out_stats->broker_complete_tail_publishes =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_COMPLETE_TAIL_PUBLISHES],
                             memory_order_relaxed);
    out_stats->broker_serve_latency_ns_total =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_TOTAL],
                             memory_order_relaxed);
    out_stats->broker_serve_latency_ns_max =
        atomic_load_explicit(&ring->broker_stats.values[LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_MAX],
                             memory_order_relaxed);
    out_stats->cursor_write_estimate =
        out_stats->client_submit_tail_publishes +
        out_stats->client_complete_head_publishes +
        out_stats->broker_submit_head_publishes +
        out_stats->broker_complete_tail_publishes;
    return 0;
}
