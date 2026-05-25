/**
 * @file src/core/runtime_broker.c
 * @brief Minimal LLAM secure-broker control-plane state.
 *
 * @details
 * This module owns broker authority that must not be mapped into an untrusted
 * client process: runtime state, MAC keys, revocation epoch, descriptor
 * authority, and lifecycle drain state. Data-plane logic lives in neighboring
 * runtime_broker_* modules.
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
#include "runtime_broker_ring.h"

#include <string.h>

static uint64_t llam_broker_revocation_epoch_unlocked(const llam_broker_t *broker) {
    if (broker == NULL || !broker->initialized) {
        return 0U;
    }
    return atomic_load_explicit(&broker->revocation_epoch, memory_order_acquire);
}

static void llam_broker_clear_session_state(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }

    /* Broker-local subject metadata is not exported, but destroy still erases
     * it so caller-owned broker storage cannot retain stale audience bindings. */
    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        llam_broker_ring_session_t *session = &broker->ring_sessions[i];

        if (session->owns_mapping) {
            llam_broker_ring_mapping_t mapping;

            memset(&mapping, 0, sizeof(mapping));
            mapping.ring = (llam_broker_ring_t *)session->ring;
            mapping.bytes = session->mapping_bytes;
            mapping.fd = session->mapping_fd;
            mapping.mapping_handle = session->mapping_handle;
            mapping.owner = true;
            llam_broker_ring_unmap(&mapping);
        }
    }
    memset(broker->ring_sessions, 0, sizeof(broker->ring_sessions));
    memset(broker->transport_sessions, 0, sizeof(broker->transport_sessions));
    broker->next_transport_subject_nonce = 0U;
}

int llam_broker_issue_object_cap_unlocked(llam_broker_t *broker,
                                          uint32_t family,
                                          uint64_t slot,
                                          uint64_t generation,
                                          uint64_t rights,
                                          llam_capability_token_t *out_token) {
    llam_capability_object_t object;

    if (LLAM_UNLIKELY(broker == NULL || !broker->initialized || broker->runtime == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(&object, 0, sizeof(object));
    object.runtime_id = broker->runtime->runtime_id;
    object.family = family;
    object.slot = slot;
    object.generation = generation;
    object.revocation_epoch = atomic_load_explicit(&broker->revocation_epoch, memory_order_acquire);
    object.subject_id = llam_broker_current_subject(broker);
    return llam_capability_issue(&broker->capability_key, &object, rights, out_token);
}

int llam_broker_init(llam_broker_t *broker, const llam_runtime_opts_t *opts, size_t opts_size) {
    llam_runtime_t *runtime = NULL;

    if (LLAM_UNLIKELY(broker == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(broker, 0, sizeof(*broker));
    {
        int rc = pthread_mutex_init(&broker->lock, NULL);

        if (rc != 0) {
            errno = rc;
            return -1;
        }
    }
    broker->lock_initialized = true;
    {
        int rc = pthread_cond_init(&broker->idle_cond, NULL);

        if (rc != 0) {
            (void)pthread_mutex_destroy(&broker->lock);
            broker->lock_initialized = false;
            errno = rc;
            return -1;
        }
    }
    broker->idle_cond_initialized = true;
    if (llam_runtime_create(opts, opts_size, &runtime) != 0) {
        (void)pthread_cond_destroy(&broker->idle_cond);
        broker->idle_cond_initialized = false;
        (void)pthread_mutex_destroy(&broker->lock);
        broker->lock_initialized = false;
        return -1;
    }
    /*
     * Capability MAC authority is broker-local. Ordinary in-process runtimes
     * must not need broker entropy or carry broker signing keys; otherwise the
     * fast in-process API would look like a capability boundary it cannot be.
     */
    if (llam_capability_key_init(&broker->capability_key, broker, runtime->runtime_id) != 0) {
        int saved_errno = errno != 0 ? errno : EIO;

        llam_runtime_destroy(runtime);
        (void)pthread_cond_destroy(&broker->idle_cond);
        broker->idle_cond_initialized = false;
        (void)pthread_mutex_destroy(&broker->lock);
        broker->lock_initialized = false;
        errno = saved_errno;
        return -1;
    }
    /*
     * Broker channels carry their fixed-size message ring in each slot. Keeping
     * that table inline made llam_broker_t too large for the default Windows
     * thread stack when examples/tests declared the broker as a local variable.
     * The broker still owns the table, but its storage now comes from the heap.
     */
    broker->channels = (llam_broker_channel_slot_t *)calloc(LLAM_BROKER_CHANNEL_SLOTS,
                                                            sizeof(*broker->channels));
    if (broker->channels == NULL) {
        llam_capability_key_clear(&broker->capability_key);
        llam_runtime_destroy(runtime);
        (void)pthread_cond_destroy(&broker->idle_cond);
        broker->idle_cond_initialized = false;
        (void)pthread_mutex_destroy(&broker->lock);
        broker->lock_initialized = false;
        errno = ENOMEM;
        return -1;
    }
    broker->runtime = runtime;
    atomic_init(&broker->revocation_epoch, 1U);
    broker->next_buffer_id = 1U;
    broker->next_descriptor_id = 1U;
    broker->next_channel_id = 1U;
    broker->next_task_id = 1U;
    broker->next_transport_subject_nonce = 1U;
    broker->initialized = true;
    return 0;
}

void llam_broker_destroy(llam_broker_t *broker) {
    llam_runtime_t *runtime;

    if (broker == NULL || !broker->lock_initialized) {
        return;
    }
    if (llam_broker_lock(broker) != 0) {
        return;
    }
    if (!broker->initialized) {
        llam_broker_unlock(broker);
        return;
    }
    broker->destroying = true;
    while (broker->active_ops > 0U && broker->idle_cond_initialized) {
        (void)pthread_cond_wait(&broker->idle_cond, &broker->lock);
    }
    runtime = broker->runtime;
    llam_broker_unlock(broker);

    /* Keep task slot storage valid until broker-owned trampoline work drains. */
    llam_broker_clear_tasks(broker);
    if (llam_broker_lock(broker) != 0) {
        return;
    }
    broker->initialized = false;
    broker->runtime = NULL;
    llam_broker_unlock(broker);

    llam_broker_clear_buffers(broker);
    llam_broker_clear_descriptors(broker);
    llam_broker_clear_channels(broker);
    free(broker->channels);
    broker->channels = NULL;
    llam_broker_clear_session_state(broker);
    llam_capability_key_clear(&broker->capability_key);
    atomic_store_explicit(&broker->revocation_epoch, 0U, memory_order_release);
    broker->next_buffer_id = 0U;
    broker->next_descriptor_id = 0U;
    broker->next_channel_id = 0U;
    broker->next_task_id = 0U;
    broker->destroying = false;
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    if (broker->idle_cond_initialized) {
        (void)pthread_cond_destroy(&broker->idle_cond);
        broker->idle_cond_initialized = false;
    }
    if (broker->lock_initialized) {
        (void)pthread_mutex_destroy(&broker->lock);
        broker->lock_initialized = false;
    }
}

uint64_t llam_broker_revocation_epoch(const llam_broker_t *broker) {
    llam_broker_t *mutable_broker = (llam_broker_t *)broker;
    uint64_t epoch;

    if (llam_broker_begin_op(mutable_broker) != 0) {
        return 0U;
    }
    epoch = llam_broker_revocation_epoch_unlocked(broker);
    llam_broker_end_op(mutable_broker);
    return epoch;
}

uint64_t llam_broker_revoke_all(llam_broker_t *broker) {
    uint64_t next_epoch;

    if (llam_broker_begin_op(broker) != 0) {
        return 0U;
    }
    next_epoch = atomic_fetch_add_explicit(&broker->revocation_epoch, 1U, memory_order_acq_rel) + 1U;
    if (next_epoch == 0U) {
        atomic_store_explicit(&broker->revocation_epoch, 1U, memory_order_release);
        llam_broker_end_op(broker);
        return 1U;
    }
    llam_broker_end_op(broker);
    return next_epoch;
}

int llam_broker_issue_object_cap(llam_broker_t *broker,
                                 uint32_t family,
                                 uint64_t slot,
                                 uint64_t generation,
                                 uint64_t rights,
                                 llam_capability_token_t *out_token) {
    int rc;

    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    rc = llam_broker_issue_object_cap_unlocked(broker, family, slot, generation, rights, out_token);
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return rc;
}

int llam_broker_validate_cap_unlocked(const llam_broker_t *broker,
                                      const llam_capability_token_t *token,
                                      uint64_t required_rights) {
    if (LLAM_UNLIKELY(broker == NULL || !broker->initialized || broker->runtime == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(token == NULL || required_rights == 0U)) {
        /*
         * Internal token attenuation can validate a MAC before narrowing rights,
         * but broker-visible validation must always prove a concrete authority.
         * Otherwise VALIDATE_CAP becomes a rightless live-token oracle.
         */
        errno = EINVAL;
        return -1;
    }
    if (llam_capability_validate_subject(&broker->capability_key,
                                         token,
                                         required_rights,
                                         llam_broker_revocation_epoch_unlocked(broker),
                                         llam_broker_current_subject(broker)) != 0) {
        return -1;
    }
    /*
     * The MAC already covers runtime_id, but the broker must still bind every
     * token to its live runtime instance. This keeps validation fail-closed if
     * key material is accidentally reused or an internal caller fabricates a
     * structurally valid token for a different broker runtime.
     */
    if (LLAM_UNLIKELY(token->runtime_id != broker->runtime->runtime_id)) {
        errno = EACCES;
        return -1;
    }
    return llam_broker_validate_live_object_unlocked(broker, token, required_rights);
}

int llam_broker_validate_cap(const llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             uint64_t required_rights) {
    llam_broker_t *mutable_broker = (llam_broker_t *)broker;
    int rc;

    if (llam_broker_begin_op(mutable_broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(mutable_broker) != 0) {
        llam_broker_end_op(mutable_broker);
        return -1;
    }
    /*
     * Public validation consults the live object table, so it must serialize
     * with object generation rotation and slot teardown. Callers that already
     * hold the broker lock use llam_broker_validate_cap_unlocked().
     */
    rc = llam_broker_validate_cap_unlocked(broker, token, required_rights);
    llam_broker_unlock(mutable_broker);
    llam_broker_end_op(mutable_broker);
    return rc;
}

int llam_broker_attenuate_cap(const llam_broker_t *broker,
                              const llam_capability_token_t *token,
                              uint64_t subset_rights,
                              llam_capability_token_t *out_token) {
    llam_broker_t *mutable_broker = (llam_broker_t *)broker;
    llam_capability_token_t attenuated;
    uint64_t epoch;
    bool aliases_input;
    int rc;

    if (LLAM_UNLIKELY(out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    aliases_input = token == out_token;
    if (!aliases_input) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(subset_rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(mutable_broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(mutable_broker) != 0) {
        llam_broker_end_op(mutable_broker);
        return -1;
    }
    /*
     * Attenuation is a broker-owned operation: callers may request fewer
     * rights, but only the broker process can MAC the reduced token. The live
     * object check is performed under the broker lock so revoke cannot race
     * the source token validation.
     */
    rc = llam_broker_validate_cap_unlocked(broker, token, subset_rights);
    if (rc == 0) {
        epoch = llam_broker_revocation_epoch_unlocked(broker);
        memset(&attenuated, 0, sizeof(attenuated));
        rc = llam_capability_attenuate(&broker->capability_key, token, subset_rights, epoch, &attenuated);
        if (rc == 0) {
            *out_token = attenuated;
        }
        memset(&attenuated, 0, sizeof(attenuated));
    }
    if (rc != 0 && !aliases_input) {
        memset(out_token, 0, sizeof(*out_token));
    }
    llam_broker_unlock(mutable_broker);
    llam_broker_end_op(mutable_broker);
    return rc;
}
