/**
 * @file src/core/runtime_broker_ring.c
 * @brief Broker ring session ownership and mapping lifecycle helpers.
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

#include <stdint.h>
#include <string.h>

static llam_broker_ring_session_t *llam_broker_ring_session_get(llam_broker_t *broker,
                                                                llam_broker_ring_t *ring,
                                                                uint64_t subject_id,
                                                                llam_broker_ring_mapping_t *out_reclaimed_mapping) {
    llam_broker_ring_session_t *free_session = NULL;
    size_t i;

    if (LLAM_UNLIKELY(out_reclaimed_mapping == NULL)) {
        errno = EINVAL;
        return NULL;
    }
    llam_broker_ring_mapping_reset(out_reclaimed_mapping);
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
     *
     * Reuse also has to reclaim by ownership, not by the active bit. Recovery
     * paths can leave a session inactive while it still owns a named mapping;
     * blindly zeroing the slot would strand the fd/name authority until process
     * exit and keep the shared-memory object reachable.
     */
    (void)llam_broker_ring_session_take_mapping(free_session, out_reclaimed_mapping);
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

    if (out_session != NULL) {
        *out_session = NULL;
    }
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

bool llam_broker_ring_session_take_mapping(llam_broker_ring_session_t *session,
                                           llam_broker_ring_mapping_t *out_mapping) {
    bool has_mapping = false;

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (session == NULL) {
        return false;
    }

    if (session->owns_mapping) {
        if (LLAM_UNLIKELY(out_mapping == NULL)) {
            return false;
        }
        out_mapping->ring = (llam_broker_ring_t *)session->ring;
        out_mapping->bytes = session->mapping_bytes;
        out_mapping->fd = session->mapping_fd;
        out_mapping->mapping_handle = session->mapping_handle;
        memcpy(out_mapping->name, session->mapping_name, sizeof(out_mapping->name));
        out_mapping->name[sizeof(out_mapping->name) - 1U] = '\0';
        out_mapping->owner = true;
        has_mapping = true;
    }

    /*
     * Reclaim mapping authority from owns_mapping, not from the active bit:
     * cleanup paths should still close/unlink broker-owned resources if an
     * interrupted lifecycle leaves the session inactive. Clear the private
     * session before releasing broker state so stale session ids or direct-ring
     * entries cannot reuse the authority being reclaimed.
     */
    memset(session, 0, sizeof(*session));
    session->mapping_fd = -1;
    session->mapping_handle = LLAM_INVALID_HANDLE;
    return has_mapping;
}

int llam_broker_ring_register_mapping(llam_broker_t *broker,
                                      llam_broker_ring_mapping_t *mapping,
                                      uint64_t subject_id,
                                      uint64_t *out_session_id) {
    llam_broker_ring_mapping_t reclaimed_mapping;
    llam_broker_ring_session_t *session;
    size_t session_index;
    int saved_errno;

    if (out_session_id != NULL) {
        *out_session_id = 0U;
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      mapping == NULL ||
                      out_session_id == NULL ||
                      !llam_broker_ring_valid(mapping->ring))) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Registration transfers mapping ownership into the broker session table.
     * Pin it like other broker operations so destroy cannot clear session state
     * while an accepted CREATE_RING request is finishing.
     */
    if (llam_broker_begin_op_subject(broker, subject_id) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    session = llam_broker_ring_session_get(broker, mapping->ring, subject_id, &reclaimed_mapping);
    if (session == NULL) {
        saved_errno = errno;
        llam_broker_unlock(broker);
        if (reclaimed_mapping.owner) {
            llam_broker_ring_unmap(&reclaimed_mapping);
        }
        llam_broker_end_op(broker);
        errno = saved_errno;
        return -1;
    }
    if (LLAM_UNLIKELY(session->owns_mapping)) {
        saved_errno = EBUSY;
        llam_broker_unlock(broker);
        if (reclaimed_mapping.owner) {
            llam_broker_ring_unmap(&reclaimed_mapping);
        }
        llam_broker_end_op(broker);
        errno = saved_errno;
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
    memcpy(session->mapping_name, mapping->name, sizeof(session->mapping_name));
    session->mapping_name[sizeof(session->mapping_name) - 1U] = '\0';
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
    if (reclaimed_mapping.owner) {
        llam_broker_ring_unmap(&reclaimed_mapping);
    }
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_ring_forget_session(llam_broker_t *broker, uint64_t session_id, uint64_t subject_id) {
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_session_t *session;
    bool unmap_mapping;

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
    unmap_mapping = llam_broker_ring_session_take_mapping(session, &mapping);
    llam_broker_unlock(broker);
    if (unmap_mapping) {
        llam_broker_ring_unmap(&mapping);
    }
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_ring_serve_session_batch(llam_broker_t *broker,
                                         uint64_t session_id,
                                         uint64_t subject_id,
                                         size_t max_requests,
                                         size_t *out_served) {
    llam_broker_ring_t *ring;
    llam_broker_ring_session_t *session;

    if (LLAM_UNLIKELY(out_served == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_served = 0U;
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
    return llam_broker_ring_serve_locked_session_batch(broker, ring, session, max_requests, out_served);
}

int llam_broker_ring_serve_session(llam_broker_t *broker, uint64_t session_id, uint64_t subject_id) {
    size_t served;

    return llam_broker_ring_serve_session_batch(broker, session_id, subject_id, 1U, &served);
}

int llam_broker_ring_serve_one_subject(llam_broker_t *broker,
                                       llam_broker_ring_t *ring,
                                       uint64_t subject_id) {
    size_t served;

    if (llam_broker_ring_serve_batch_subject(broker, ring, subject_id, 1U, &served) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(served != 1U)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int llam_broker_ring_serve_one(llam_broker_t *broker, llam_broker_ring_t *ring) {
    return llam_broker_ring_serve_one_subject(broker, ring, 0U);
}

int llam_broker_ring_serve_batch_subject(llam_broker_t *broker,
                                         llam_broker_ring_t *ring,
                                         uint64_t subject_id,
                                         size_t max_requests,
                                         size_t *out_served) {
    llam_broker_ring_mapping_t reclaimed_mapping;
    llam_broker_ring_session_t *session;

    if (out_served != NULL) {
        *out_served = 0U;
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || out_served == NULL)) {
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
    session = llam_broker_ring_session_get(broker, ring, subject_id, &reclaimed_mapping);
    if (session == NULL) {
        int saved_errno = errno;

        llam_broker_unlock(broker);
        if (reclaimed_mapping.owner) {
            llam_broker_ring_unmap(&reclaimed_mapping);
        }
        llam_broker_end_op(broker);
        errno = saved_errno;
        return -1;
    }
    /*
     * Direct ring serving is a hot path, but reclaiming an inactive owned
     * mapping is an exceptional repair path. Keep the initialized session under
     * the table lock while the old shm object is unlinked so another serve
     * cannot observe the stale authority between reuse and cleanup.
     */
    if (reclaimed_mapping.owner) {
        llam_broker_ring_unmap(&reclaimed_mapping);
    }
    if (session->busy) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EBUSY;
        return -1;
    }
    return llam_broker_ring_serve_locked_session_batch(broker, ring, session, max_requests, out_served);
}

int llam_broker_ring_serve_batch(llam_broker_t *broker,
                                 llam_broker_ring_t *ring,
                                 size_t max_requests,
                                 size_t *out_served) {
    return llam_broker_ring_serve_batch_subject(broker, ring, 0U, max_requests, out_served);
}
