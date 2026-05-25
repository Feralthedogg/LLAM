/**
 * @file src/core/runtime_broker_ring.c
 * @brief Shared-memory-ready broker ring and buffer grant helpers.
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

static llam_broker_ring_session_t *llam_broker_ring_session_get(llam_broker_t *broker,
                                                                llam_broker_ring_t *ring,
                                                                uint64_t subject_id) {
    llam_broker_ring_session_t *free_session = NULL;
    size_t i;

    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        llam_broker_ring_session_t *session = &broker->ring_sessions[i];

        if (session->active && session->ring == ring) {
            if (LLAM_UNLIKELY(session->subject_id != subject_id)) {
                errno = EACCES;
                return NULL;
            }
            return session;
        }
        if (!session->active && free_session == NULL) {
            free_session = session;
        }
    }
    if (free_session == NULL) {
        errno = ENOSPC;
        return NULL;
    }

    /*
     * Broker progress cursors live outside the shared mapping. A client can
     * corrupt the visible counters, but it cannot rewind these private cursors
     * to replay a stale submission as fresh broker work.
     */
    memset(free_session, 0, sizeof(*free_session));
    free_session->ring = ring;
    free_session->subject_id = subject_id;
    free_session->mapping_fd = -1;
    free_session->mapping_handle = LLAM_INVALID_HANDLE;
    free_session->active = true;
    return free_session;
}

static int llam_broker_ring_begin_locked_owned_session(llam_broker_t *broker,
                                                       uint64_t session_id,
                                                       uint64_t subject_id,
                                                       llam_broker_ring_session_t **out_session) {
    llam_broker_ring_session_t *session;

    if (LLAM_UNLIKELY(session_id == 0U || session_id > LLAM_BROKER_RING_SESSIONS || out_session == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op_subject(broker, subject_id) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }

    session = &broker->ring_sessions[(size_t)session_id - 1U];
    if (LLAM_UNLIKELY(!session->active || !session->owns_mapping || session->ring == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(session->subject_id != subject_id)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EACCES;
        return -1;
    }

    /*
     * Success intentionally returns with both the broker op and table lock held.
     * Forget/serve must claim or clear the session before another broker command
     * can race the private mapping authority.
     */
    *out_session = session;
    return 0;
}

int llam_broker_ring_register_mapping(llam_broker_t *broker,
                                      llam_broker_ring_mapping_t *mapping,
                                      uint64_t subject_id,
                                      uint64_t *out_session_id) {
    llam_broker_ring_session_t *session;
    size_t session_index;

    if (LLAM_UNLIKELY(broker == NULL ||
                      mapping == NULL ||
                      out_session_id == NULL ||
                      !llam_broker_ring_valid(mapping->ring))) {
        errno = EINVAL;
        return -1;
    }
    *out_session_id = 0U;
    if (llam_broker_lock(broker) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL || broker->destroying)) {
        llam_broker_unlock(broker);
        errno = EINVAL;
        return -1;
    }
    session = llam_broker_ring_session_get(broker, mapping->ring, subject_id);
    if (session == NULL) {
        llam_broker_unlock(broker);
        return -1;
    }
    if (LLAM_UNLIKELY(session->owns_mapping)) {
        llam_broker_unlock(broker);
        errno = EBUSY;
        return -1;
    }

    /*
     * Transport-created rings are broker-owned. The shared mapping is the data
     * plane, while the session keeps private progress cursors and the authority
     * needed to unmap/close the mapping during broker destruction.
     */
    session->mapping_bytes = mapping->bytes;
    session->mapping_fd = mapping->fd;
    session->mapping_handle = mapping->mapping_handle;
    session->owns_mapping = true;
    session_index = (size_t)(session - broker->ring_sessions);
    *out_session_id = (uint64_t)session_index + 1U;

    mapping->ring = NULL;
    mapping->bytes = 0U;
    mapping->fd = -1;
    mapping->mapping_handle = LLAM_INVALID_HANDLE;
    mapping->owner = false;
    mapping->name[0] = '\0';

    llam_broker_unlock(broker);
    return 0;
}

int llam_broker_ring_forget_session(llam_broker_t *broker, uint64_t session_id, uint64_t subject_id) {
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_session_t *session;

    if (llam_broker_ring_begin_locked_owned_session(broker, session_id, subject_id, &session) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(session->busy)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EBUSY;
        return -1;
    }

    /*
     * A transport can create the broker-owned mapping before discovering that
     * the response fd/HANDLE cannot be delivered. Clear the session first so
     * the slot cannot be served, then unmap the private authority outside the
     * table state.
     */
    memset(&mapping, 0, sizeof(mapping));
    mapping.ring = (llam_broker_ring_t *)session->ring;
    mapping.bytes = session->mapping_bytes;
    mapping.fd = session->mapping_fd;
    mapping.mapping_handle = session->mapping_handle;
    mapping.owner = true;
    memset(session, 0, sizeof(*session));
    session->mapping_fd = -1;
    session->mapping_handle = LLAM_INVALID_HANDLE;
    llam_broker_unlock(broker);
    llam_broker_ring_unmap(&mapping);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_ring_serve_session(llam_broker_t *broker, uint64_t session_id, uint64_t subject_id) {
    llam_broker_ring_t *ring;
    llam_broker_ring_session_t *session;

    if (llam_broker_ring_begin_locked_owned_session(broker, session_id, subject_id, &session) != 0) {
        return -1;
    }
    /*
     * Claim the session while still holding the table lock. Splitting
     * validation from busy-claim would allow a failed response cleanup path to
     * forget and unmap the broker-owned ring between lookup and execution.
     */
    ring = (llam_broker_ring_t *)session->ring;
    if (session->busy) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EBUSY;
        return -1;
    }
    return llam_broker_ring_serve_locked_session(broker, ring, session);
}

int llam_broker_ring_init(llam_broker_ring_t *ring) {
    if (LLAM_UNLIKELY(ring == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(ring, 0, sizeof(*ring));
    ring->magic = LLAM_BROKER_RING_MAGIC;
    ring->version = LLAM_BROKER_RING_VERSION;
    ring->capacity = LLAM_BROKER_RING_CAP;
    atomic_init(&ring->submit_head, 0U);
    atomic_init(&ring->submit_tail, 0U);
    atomic_init(&ring->complete_head, 0U);
    atomic_init(&ring->complete_tail, 0U);
    return 0;
}

int llam_broker_ring_submit_push(llam_broker_ring_t *ring, const llam_broker_ring_submission_t *entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    tail = atomic_load_explicit(&ring->submit_tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->submit_head, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(tail - head >= LLAM_BROKER_RING_CAP)) {
        errno = EAGAIN;
        return -1;
    }
    ring->submissions[tail & LLAM_BROKER_RING_MASK] = *entry;
    atomic_store_explicit(&ring->submit_tail, tail + 1U, memory_order_release);
    return 0;
}

int llam_broker_ring_submit_pop(llam_broker_ring_t *ring, llam_broker_ring_submission_t *out_entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    head = atomic_load_explicit(&ring->submit_head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->submit_tail, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(head == tail)) {
        errno = EAGAIN;
        return -1;
    }
    *out_entry = ring->submissions[head & LLAM_BROKER_RING_MASK];
    atomic_store_explicit(&ring->submit_head, head + 1U, memory_order_release);
    return 0;
}

int llam_broker_ring_complete_push(llam_broker_ring_t *ring, const llam_broker_ring_completion_t *entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    tail = atomic_load_explicit(&ring->complete_tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->complete_head, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(tail - head >= LLAM_BROKER_RING_CAP)) {
        errno = EAGAIN;
        return -1;
    }
    ring->completions[tail & LLAM_BROKER_RING_MASK] = *entry;
    atomic_store_explicit(&ring->complete_tail, tail + 1U, memory_order_release);
    return 0;
}

int llam_broker_ring_complete_pop(llam_broker_ring_t *ring, llam_broker_ring_completion_t *out_entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    head = atomic_load_explicit(&ring->complete_head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->complete_tail, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(head == tail)) {
        errno = EAGAIN;
        return -1;
    }
    *out_entry = ring->completions[head & LLAM_BROKER_RING_MASK];
    atomic_store_explicit(&ring->complete_head, head + 1U, memory_order_release);
    return 0;
}

int llam_broker_ring_serve_one_subject(llam_broker_t *broker,
                                       llam_broker_ring_t *ring,
                                       uint64_t subject_id) {
    llam_broker_ring_session_t *session;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring))) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op_subject(broker, subject_id) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    session = llam_broker_ring_session_get(broker, ring, subject_id);
    if (session == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    if (session->busy) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EBUSY;
        return -1;
    }
    return llam_broker_ring_serve_locked_session(broker, ring, session);
}

int llam_broker_ring_serve_one(llam_broker_t *broker, llam_broker_ring_t *ring) {
    return llam_broker_ring_serve_one_subject(broker, ring, 0U);
}

int llam_broker_buffer_grant_init(llam_broker_buffer_grant_t *grant,
                                  uint64_t grant_id,
                                  uint64_t generation,
                                  uint64_t offset,
                                  uint64_t length,
                                  uint64_t rights,
                                  uint64_t revocation_epoch) {
    if (LLAM_UNLIKELY(grant == NULL || grant_id == 0U || generation == 0U || length == 0U || rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    grant->grant_id = grant_id;
    grant->generation = generation;
    grant->offset = offset;
    grant->length = length;
    grant->rights = rights;
    grant->revocation_epoch = revocation_epoch;
    return 0;
}

int llam_broker_buffer_grant_validate(const llam_broker_buffer_grant_t *grant,
                                      uint64_t required_rights,
                                      uint64_t relative_offset,
                                      uint64_t length,
                                      uint64_t current_revocation_epoch) {
    uint64_t end;

    if (LLAM_UNLIKELY(grant == NULL || required_rights == 0U || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(grant->revocation_epoch != current_revocation_epoch ||
                      (grant->rights & required_rights) != required_rights)) {
        errno = EACCES;
        return -1;
    }
    end = relative_offset + length;
    if (LLAM_UNLIKELY(end < relative_offset || end > grant->length)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
