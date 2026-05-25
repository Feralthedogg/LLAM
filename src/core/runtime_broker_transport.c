/**
 * @file src/core/runtime_broker_transport.c
 * @brief Broker transport subject tracking.
 *
 * @details
 * Transport subjects bind serialized broker capabilities to one local control
 * connection. Platform-specific control transports live in separate source
 * files so subject lifetime rules stay independent of fd/HANDLE mechanics.
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
#include "runtime_broker.h"

#include <string.h>

static atomic_bool g_llam_broker_force_subject_entropy_failure;

void llam_broker_test_force_subject_entropy_failure(bool enabled) {
    atomic_store_explicit(&g_llam_broker_force_subject_entropy_failure, enabled, memory_order_relaxed);
}

static int llam_broker_new_transport_subject(llam_broker_t *broker,
                                             uintptr_t transport_id,
                                             uint64_t nonce,
                                             uint64_t *out_subject) {
    uint64_t subject;

    if (LLAM_UNLIKELY(out_subject == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_subject = 0U;
    if (LLAM_UNLIKELY(atomic_load_explicit(&g_llam_broker_force_subject_entropy_failure,
                                           memory_order_relaxed) ||
                      !llam_public_slot_entropy_from_os(&subject))) {
        /*
         * Transport subjects are the audience binding for serialized broker
         * tokens. If the broker cannot obtain OS entropy for a new session,
         * fail closed rather than falling back to predictable fd/path state.
         */
        errno = EIO;
        return -1;
    }

    subject = llam_public_slot_mix64(subject ^ (uint64_t)transport_id ^ nonce);
    if (subject == 0U) {
        subject = llam_public_slot_mix64(UINT64_C(0x9e3779b97f4a7c15) ^ nonce);
        if (subject == 0U) {
            subject = UINT64_C(0x9e3779b97f4a7c15);
        }
    }
    *out_subject = subject;
    (void)broker;
    return 0;
}

int llam_broker_transport_subject(llam_broker_t *broker,
                                  uintptr_t transport_id,
                                  uint64_t *out_subject_id) {
    llam_broker_transport_session_t *free_session = NULL;
    uint64_t subject;
    size_t i;

    /*
     * A broker transport session is the security audience for tokens minted
     * over that connection. Keep it stable for the lifetime of a local control
     * connection so helpers that drive one request at a time do not accidentally
     * invalidate tokens between adjacent requests.
     */
    if (LLAM_UNLIKELY(broker == NULL || out_subject_id == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_subject_id = 0U;
    if (llam_broker_lock(broker) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL || broker->destroying)) {
        llam_broker_unlock(broker);
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        llam_broker_transport_session_t *session = &broker->transport_sessions[i];

        if (session->active && session->transport_id == transport_id) {
            *out_subject_id = session->subject_id;
            llam_broker_unlock(broker);
            return 0;
        }
        if (!session->active && free_session == NULL) {
            free_session = session;
        }
    }
    if (free_session == NULL) {
        llam_broker_unlock(broker);
        errno = ENOSPC;
        return -1;
    }
    broker->next_transport_subject_nonce++;
    if (broker->next_transport_subject_nonce == 0U) {
        broker->next_transport_subject_nonce = 1U;
    }
    if (llam_broker_new_transport_subject(broker,
                                          transport_id,
                                          broker->next_transport_subject_nonce,
                                          &subject) != 0) {
        llam_broker_unlock(broker);
        return -1;
    }
    free_session->transport_id = transport_id;
    free_session->subject_id = subject;
    free_session->active = true;
    *out_subject_id = subject;
    llam_broker_unlock(broker);
    return 0;
}

void llam_broker_forget_transport_subject(llam_broker_t *broker, uintptr_t transport_id) {
    size_t i;

    if (broker == NULL || llam_broker_lock(broker) != 0) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        llam_broker_transport_session_t *session = &broker->transport_sessions[i];

        if (session->active && session->transport_id == transport_id) {
            memset(session, 0, sizeof(*session));
            break;
        }
    }
    llam_broker_unlock(broker);
}
