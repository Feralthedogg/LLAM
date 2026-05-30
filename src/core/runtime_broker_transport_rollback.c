/**
 * @file src/core/runtime_broker_transport_rollback.c
 * @brief Roll back broker authority created for failed transport responses.
 *
 * @details
 * Broker transport handlers create some broker-owned authority before the
 * response bytes are written to a client. If the write fails, the client never
 * receives the token or private-ring session id. This file owns the cleanup
 * path that reclaims those unreachable grants without mixing rollback details
 * into the request dispatcher.
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

#include <stdlib.h>
#include <string.h>

static void llam_broker_rollback_response_token_locked(llam_broker_t *broker,
                                                       const llam_capability_token_t *token,
                                                       llam_task_t **out_task_to_detach) {
    size_t i;

    if (broker == NULL || token == NULL) {
        return;
    }
    switch (token->family) {
    case LLAM_BROKER_CAP_FAMILY_BUFFER:
        for (i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
            llam_broker_buffer_slot_t *slot = &broker->buffers[i];

            if (slot->active && slot->id == token->slot && slot->generation == token->generation) {
                llam_broker_buffer_slot_reset(slot);
                return;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_CHANNEL:
        if (broker->channels == NULL) {
            return;
        }
        for (i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
            llam_broker_channel_slot_t *slot = &broker->channels[i];

            if (slot->active && slot->id == token->slot && slot->generation == token->generation) {
                memset(slot, 0, sizeof(*slot));
                return;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_DESCRIPTOR:
        for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
            llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

            if (slot->active && slot->id == token->slot && slot->generation == token->generation) {
                if (slot->close_on_destroy) {
#if LLAM_PLATFORM_WINDOWS
                    if (!LLAM_HANDLE_IS_INVALID(slot->handle)) {
                        llam_broker_close_handle(slot->handle);
                    }
#else
                    if (slot->fd >= 0) {
                        llam_broker_close_handle((llam_handle_t)slot->fd);
                    }
#endif
                }
                memset(slot, 0, sizeof(*slot));
#if LLAM_PLATFORM_WINDOWS
                slot->handle = LLAM_INVALID_HANDLE;
#else
                slot->fd = -1;
#endif
                return;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_TASK:
        for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
            llam_broker_task_slot_t *slot = &broker->tasks[i];

            if (slot->active && slot->id == token->slot && slot->generation == token->generation) {
                for (;;) {
                    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);

                    if (slot->task == NULL) {
                        return;
                    }
                    if (state == LLAM_BROKER_TASK_STATE_COMPLETED) {
                        if (out_task_to_detach != NULL) {
                            *out_task_to_detach = slot->task;
                        }
                        memset(slot, 0, sizeof(*slot));
                        atomic_init(&slot->state, LLAM_BROKER_TASK_STATE_EMPTY);
                        return;
                    }
                    if (state == LLAM_BROKER_TASK_STATE_SPAWNED) {
                        uint32_t expected = LLAM_BROKER_TASK_STATE_SPAWNED;

                        /*
                         * Task completion writes COMPLETED without taking the
                         * broker lock. Claim the live spawned state with a CAS
                         * so rollback cannot overwrite a concurrent completion
                         * with DETACHED and leave the slot permanently active.
                         * The running task observes DETACHED in its trampoline
                         * and detaches itself before slot reset; doing that from
                         * the transport thread races task exit/reclaim on some
                         * scheduler interleavings.
                         */
                        if (!atomic_compare_exchange_weak_explicit(&slot->state,
                                                                   &expected,
                                                                   LLAM_BROKER_TASK_STATE_DETACHED,
                                                                   memory_order_acq_rel,
                                                                   memory_order_acquire)) {
                            continue;
                        }
                        slot->rights = 0U;
                        return;
                    }
                    return;
                }
            }
        }
        break;
    default:
        break;
    }
}

void llam_broker_rollback_created_response(llam_broker_t *broker,
                                           const llam_broker_wire_request_t *request,
                                           const llam_broker_wire_response_t *response,
                                           uint64_t subject_id) {
    llam_task_t *task_to_detach = NULL;

    if (broker == NULL || request == NULL || response == NULL || response->status != 0) {
        return;
    }
    if (request->op == (uint32_t)LLAM_BROKER_WIRE_OP_CREATE_RING && response->result2 != 0U) {
        (void)llam_broker_ring_forget_session(broker, response->result2, subject_id);
        return;
    }
    switch ((llam_broker_wire_op_t)request->op) {
    case LLAM_BROKER_WIRE_OP_CREATE_BUFFER:
    case LLAM_BROKER_WIRE_OP_CREATE_CHANNEL:
    case LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR:
    case LLAM_BROKER_WIRE_OP_TASK_SPAWN:
        /*
         * The broker creates these grants before writing the response. If the
         * transport write fails, the client never receives the token, so keeping
         * the slot live would leak broker-owned authority until destroy.
         */
        if (llam_broker_lock(broker) == 0) {
            llam_broker_rollback_response_token_locked(broker, &response->token, &task_to_detach);
            llam_broker_unlock(broker);
        }
        if (task_to_detach != NULL) {
            (void)llam_detach(task_to_detach);
        }
        break;
    default:
        break;
    }
}
