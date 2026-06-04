/**
 * @file src/core/broker/ring/broker_ring_queue.c
 * @brief Raw SPSC submission/completion queue primitives for broker rings.
 *
 * @details
 * These helpers intentionally stay separate from broker session ownership and
 * operation dispatch. They only validate the shared ring window, copy queue
 * entries, and publish cursor updates with the SPSC memory ordering contract.
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

int llam_broker_ring_init(llam_broker_ring_t *ring) {
    if (LLAM_UNLIKELY(ring == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(ring, 0, sizeof(*ring));
    ring->magic = LLAM_BROKER_RING_MAGIC;
    ring->version = LLAM_BROKER_RING_VERSION;
    ring->capacity = LLAM_BROKER_RING_CAP;
    atomic_init(&ring->submit_head.value, 0U);
    atomic_init(&ring->submit_tail.value, 0U);
    atomic_init(&ring->complete_head.value, 0U);
    atomic_init(&ring->complete_tail.value, 0U);
    return 0;
}

int llam_broker_ring_submit_push(llam_broker_ring_t *ring, const llam_broker_ring_submission_t *entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_relaxed);
    head = atomic_load_explicit(&ring->submit_head.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(tail - head >= LLAM_BROKER_RING_CAP)) {
        llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_FULL, 1U);
        errno = EAGAIN;
        return -1;
    }
    ring->submissions[tail & LLAM_BROKER_RING_MASK] = *entry;
    atomic_store_explicit(&ring->submit_tail.value, tail + 1U, memory_order_release);
    llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_PUSHES, 1U);
    llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_TAIL_PUBLISHES, 1U);
    return 0;
}

int llam_broker_ring_submit_pop(llam_broker_ring_t *ring, llam_broker_ring_submission_t *out_entry) {
    uint64_t head;
    uint64_t tail;

    if (out_entry != NULL) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    head = atomic_load_explicit(&ring->submit_head.value, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(head == tail)) {
        errno = EAGAIN;
        return -1;
    }
    *out_entry = ring->submissions[head & LLAM_BROKER_RING_MASK];
    atomic_store_explicit(&ring->submit_head.value, head + 1U, memory_order_release);
    return 0;
}

int llam_broker_ring_complete_push(llam_broker_ring_t *ring, const llam_broker_ring_completion_t *entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_relaxed);
    head = atomic_load_explicit(&ring->complete_head.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(tail - head >= LLAM_BROKER_RING_CAP)) {
        llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_FULL, 1U);
        errno = EAGAIN;
        return -1;
    }
    ring->completions[tail & LLAM_BROKER_RING_MASK] = *entry;
    atomic_store_explicit(&ring->complete_tail.value, tail + 1U, memory_order_release);
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_TAIL_PUBLISHES, 1U);
    return 0;
}

int llam_broker_ring_complete_pop(llam_broker_ring_t *ring, llam_broker_ring_completion_t *out_entry) {
    size_t count;

    if (out_entry != NULL) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_ring_complete_drain(ring, out_entry, 1U, &count) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(count == 0U)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int llam_broker_ring_complete_drain(llam_broker_ring_t *ring,
                                    llam_broker_ring_completion_t *out_entries,
                                    size_t max_entries,
                                    size_t *out_count) {
    uint64_t available;
    uint64_t head;
    uint64_t tail;
    size_t count;
    size_t i;

    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (out_entries != NULL && max_entries > 0U) {
        if (LLAM_UNLIKELY(max_entries > SIZE_MAX / sizeof(*out_entries))) {
            /*
             * The caller's element count is not trustworthy. Clear one
             * completion as a stale-authority sentinel, then fail before the
             * byte-count multiplication can wrap.
             */
            memset(out_entries, 0, sizeof(*out_entries));
            errno = EOVERFLOW;
            return -1;
        }
        memset(out_entries, 0, sizeof(*out_entries) * max_entries);
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) ||
                      out_count == NULL ||
                      (max_entries > 0U && out_entries == NULL))) {
        errno = EINVAL;
        return -1;
    }
    if (max_entries == 0U) {
        return 0;
    }
    llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_CALLS, 1U);
    head = atomic_load_explicit(&ring->complete_head.value, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    available = tail - head;
    if (available == 0U) {
        llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_EMPTY, 1U);
        return 0;
    }
    count = available < (uint64_t)max_entries ? (size_t)available : max_entries;
    for (i = 0U; i < count; ++i) {
        out_entries[i] = ring->completions[(head + (uint64_t)i) & LLAM_BROKER_RING_MASK];
    }
    /*
     * Publish the client-side drain cursor once after copying the batch. This
     * preserves SPSC ordering while avoiding one shared-cache-line write per
     * completion when transport clients harvest multiple responses.
     */
    atomic_store_explicit(&ring->complete_head.value, head + (uint64_t)count, memory_order_release);
    llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_ENTRIES, (uint64_t)count);
    llam_broker_ring_client_stat_add(ring, LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_HEAD_PUBLISHES, 1U);
    llam_broker_ring_client_stat_max(ring, LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_BATCH_MAX, (uint64_t)count);
    *out_count = count;
    return 0;
}
