/**
 * @file src/core/broker/transport/broker_transport.c
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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "runtime_internal.h"
#include "runtime_broker.h"
#include "runtime_broker_ring.h"

#include <string.h>

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_broker_force_subject_entropy_failure;
static atomic_bool g_llam_broker_force_subject_value_enabled;
static atomic_uint_fast64_t g_llam_broker_forced_subject_value;
static atomic_bool g_llam_broker_pause_subject_reclaim;
static atomic_bool g_llam_broker_subject_reclaim_entered;
static atomic_bool g_llam_broker_release_subject_reclaim;

static bool llam_broker_subject_entropy_forced_failure(void) {
    return atomic_load_explicit(&g_llam_broker_force_subject_entropy_failure, memory_order_relaxed);
}

void llam_broker_test_force_subject_entropy_failure(bool enabled) {
    atomic_store_explicit(&g_llam_broker_force_subject_entropy_failure, enabled, memory_order_relaxed);
}

static bool llam_broker_subject_forced_value(uint64_t *out_subject) {
    if (!atomic_load_explicit(&g_llam_broker_force_subject_value_enabled, memory_order_relaxed)) {
        return false;
    }
    *out_subject = atomic_load_explicit(&g_llam_broker_forced_subject_value, memory_order_relaxed);
    return true;
}

void llam_broker_test_force_subject_value(bool enabled, uint64_t subject_id) {
    atomic_store_explicit(&g_llam_broker_forced_subject_value, subject_id, memory_order_relaxed);
    atomic_store_explicit(&g_llam_broker_force_subject_value_enabled, enabled, memory_order_relaxed);
}

void llam_broker_test_pause_subject_reclaim(bool enabled) {
    if (enabled) {
        atomic_store_explicit(&g_llam_broker_release_subject_reclaim, false, memory_order_relaxed);
        atomic_store_explicit(&g_llam_broker_subject_reclaim_entered, false, memory_order_relaxed);
        atomic_store_explicit(&g_llam_broker_pause_subject_reclaim, true, memory_order_release);
        return;
    }
    atomic_store_explicit(&g_llam_broker_pause_subject_reclaim, false, memory_order_relaxed);
    atomic_store_explicit(&g_llam_broker_release_subject_reclaim, true, memory_order_release);
}

bool llam_broker_test_subject_reclaim_paused(void) {
    return atomic_load_explicit(&g_llam_broker_subject_reclaim_entered, memory_order_acquire);
}

static void llam_broker_test_pause_before_subject_reclaim(void) {
    if (!atomic_load_explicit(&g_llam_broker_pause_subject_reclaim, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&g_llam_broker_subject_reclaim_entered, true, memory_order_release);
    while (!atomic_load_explicit(&g_llam_broker_release_subject_reclaim, memory_order_acquire)) {
        llam_pause_cpu();
    }
    atomic_store_explicit(&g_llam_broker_subject_reclaim_entered, false, memory_order_release);
}
#else
static bool llam_broker_subject_entropy_forced_failure(void) {
    return false;
}

static bool llam_broker_subject_forced_value(uint64_t *out_subject) {
    (void)out_subject;
    return false;
}

static void llam_broker_test_pause_before_subject_reclaim(void) {
}
#endif

#define LLAM_BROKER_SUBJECT_MINT_ATTEMPTS 8U

static bool llam_broker_transport_subject_in_use_unlocked(const llam_broker_t *broker, uint64_t subject) {
    size_t i;

    if (broker == NULL || subject == 0U) {
        return false;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        const llam_broker_transport_session_t *session = &broker->transport_sessions[i];

        if (session->active && session->subject_id == subject) {
            return true;
        }
    }
    return false;
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
    if (llam_broker_subject_forced_value(&subject)) {
        if (LLAM_UNLIKELY(subject == 0U)) {
            errno = EIO;
            return -1;
        }
        *out_subject = subject;
        (void)broker;
        (void)transport_id;
        (void)nonce;
        return 0;
    }
    if (LLAM_UNLIKELY(llam_broker_subject_entropy_forced_failure() ||
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
    unsigned attempts;
    size_t i;

    /*
     * A broker transport session is the security audience for tokens minted
     * over that connection. Keep it stable for the lifetime of a local control
     * connection so helpers that drive one request at a time do not accidentally
     * invalidate tokens between adjacent requests.
     */
    if (out_subject_id != NULL) {
        *out_subject_id = 0U;
    }
    if (LLAM_UNLIKELY(broker == NULL || out_subject_id == NULL)) {
        errno = EINVAL;
        return -1;
    }
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
    for (attempts = 0U; attempts < LLAM_BROKER_SUBJECT_MINT_ATTEMPTS; ++attempts) {
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
        /*
         * Subject ids are MAC audiences. Random collision is vanishingly
         * unlikely, but accepting one would make two live local-control
         * sessions mutually replayable. Keep allocation fail-closed instead.
         */
        if (!llam_broker_transport_subject_in_use_unlocked(broker, subject)) {
            free_session->transport_id = transport_id;
            free_session->subject_id = subject;
            free_session->active = true;
            *out_subject_id = subject;
            llam_broker_unlock(broker);
            return 0;
        }
    }
    llam_broker_unlock(broker);
    errno = EIO;
    return -1;
}

void llam_broker_forget_transport_subject(llam_broker_t *broker, uintptr_t transport_id) {
    size_t i;
    uint64_t subject_id = 0U;

    if (broker == NULL || llam_broker_begin_op(broker) != 0) {
        return;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        llam_broker_transport_session_t *session = &broker->transport_sessions[i];

        if (session->active && session->transport_id == transport_id) {
            subject_id = session->subject_id;
            memset(session, 0, sizeof(*session));
            break;
        }
    }
    llam_broker_unlock(broker);
    if (subject_id != 0U) {
        llam_broker_test_pause_before_subject_reclaim();
        llam_broker_reclaim_subject_objects(broker, subject_id);
    }
    llam_broker_end_op(broker);
}

void llam_broker_reclaim_subject_objects(llam_broker_t *broker, uint64_t subject_id) {
    if (broker == NULL || subject_id == 0U) {
        return;
    }
    /*
     * A busy ring session has already accepted immutable work for this
     * subject and may still validate a descriptor/channel/buffer token after
     * dropping the broker table lock.  Mark every owned ring for deferred
     * teardown first; the last busy completion retries this whole reclaim once
     * no in-flight session can consume the subject's grants.
     */
    if (llam_broker_reclaim_subject_rings(broker, subject_id)) {
        return;
    }
    llam_broker_reclaim_subject_buffers(broker, subject_id);
    llam_broker_reclaim_subject_descriptors(broker, subject_id);
    llam_broker_reclaim_subject_channels(broker, subject_id);
    llam_broker_reclaim_subject_tasks(broker, subject_id);
}
