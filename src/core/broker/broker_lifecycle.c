/**
 * @file src/core/broker/broker_lifecycle.c
 * @brief Broker teardown and destroy sequencing.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_internal.h"
#include "runtime_broker.h"
#include "runtime_broker_ring.h"

#include <stdlib.h>
#include <string.h>

static void llam_broker_clear_session_state(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }

    /* Erase broker-local audience bindings before caller-owned storage reuse. */
    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        llam_broker_ring_session_t *session = &broker->ring_sessions[i];

        if (session->owns_mapping) {
            llam_broker_ring_mapping_t mapping;

            if (llam_broker_ring_session_take_mapping(session, &mapping)) {
                llam_broker_ring_unmap(&mapping);
            }
        }
    }
    memset(broker->ring_sessions, 0, sizeof(broker->ring_sessions));
    memset(broker->transport_sessions, 0, sizeof(broker->transport_sessions));
    broker->next_transport_subject_nonce = 0U;
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
    if (LLAM_UNLIKELY(llam_broker_current_thread_has_op(broker))) {
        llam_broker_unlock(broker);
        errno = EBUSY;
        return;
    }
    if (broker->destroying) {
        if (!broker->idle_cond_initialized) {
            llam_broker_unlock(broker);
            return;
        }
        broker->destroy_waiters += 1U;
        while (broker->initialized && broker->idle_cond_initialized) {
            (void)pthread_cond_wait(&broker->idle_cond, &broker->lock);
        }
        broker->destroy_waiters -= 1U;
        if (broker->destroy_waiters == 0U && broker->idle_cond_initialized) {
            (void)pthread_cond_broadcast(&broker->idle_cond);
        }
        llam_broker_unlock(broker);
        return;
    }
    if (LLAM_UNLIKELY(broker->active_ops >= LLAM_BROKER_ACTIVE_OP_BUSY_SENTINEL)) {
        llam_broker_unlock(broker);
        errno = EBUSY;
        return;
    }
    broker->destroying = true;
    while (broker->active_ops > 0U && broker->idle_cond_initialized) {
        if (LLAM_UNLIKELY(broker->active_ops >= LLAM_BROKER_ACTIVE_OP_BUSY_SENTINEL)) {
            broker->destroying = false;
            (void)pthread_cond_broadcast(&broker->idle_cond);
            llam_broker_unlock(broker);
            errno = EBUSY;
            return;
        }
        (void)pthread_cond_wait(&broker->idle_cond, &broker->lock);
    }
    runtime = broker->runtime;
    llam_broker_unlock(broker);

    /*
     * Broker task slots are trampoline arguments. Request cooperative stop
     * before draining them so long broker task sleeps cannot pin teardown.
     */
    if (runtime != NULL) {
        llam_request_stop(runtime);
    }

    /* Keep task slot storage valid until broker-owned trampoline work drains. */
    llam_broker_clear_tasks(broker);
    if (llam_broker_lock(broker) != 0) {
        return;
    }
    broker->initialized = false;
    broker->runtime = NULL;
    if (broker->idle_cond_initialized) {
        (void)pthread_cond_broadcast(&broker->idle_cond);
    }
    while (broker->destroy_waiters > 0U && broker->idle_cond_initialized) {
        (void)pthread_cond_wait(&broker->idle_cond, &broker->lock);
    }
    broker->destroying = false;
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
