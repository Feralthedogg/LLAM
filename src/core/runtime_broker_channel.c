/**
 * @file src/core/runtime_broker_channel.c
 * @brief Broker-owned bounded channel capability data plane.
 *
 * @details
 * Broker channels are intentionally small and synchronous: they provide a
 * trusted-process message queue for capability tests and broker commands, not a
 * replacement for LLAM's in-process high-throughput channel implementation.
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

void llam_broker_clear_channels(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL || broker->channels == NULL) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
        memset(&broker->channels[i], 0, sizeof(broker->channels[i]));
    }
}

llam_broker_channel_slot_t *llam_broker_find_channel_unlocked(llam_broker_t *broker,
                                                              const llam_capability_token_t *token,
                                                              uint64_t required_rights) {
    size_t i;

    if (LLAM_UNLIKELY(broker == NULL || broker->channels == NULL)) {
        errno = EINVAL;
        return NULL;
    }
    if (llam_broker_validate_token_family_unlocked(broker,
                                                   token,
                                                   LLAM_BROKER_CAP_FAMILY_CHANNEL,
                                                   required_rights) != 0) {
        return NULL;
    }
    for (i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
        llam_broker_channel_slot_t *slot = &broker->channels[i];

        if (slot->active &&
            slot->id == token->slot &&
            slot->generation == token->generation) {
            if (LLAM_UNLIKELY((slot->rights & required_rights) != required_rights)) {
                errno = EACCES;
                return NULL;
            }
            return slot;
        }
    }
    errno = EACCES;
    return NULL;
}

int llam_broker_create_channel(llam_broker_t *broker,
                               size_t capacity,
                               uint64_t rights,
                               llam_capability_token_t *out_token) {
    llam_broker_channel_slot_t *slot = NULL;
    size_t i;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      broker->channels == NULL ||
                      capacity == 0U ||
                      capacity > LLAM_BROKER_CHANNEL_CAPACITY ||
                      rights == 0U ||
                      out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_CHANNEL, rights) != 0) {
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
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
    for (i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
        if (!broker->channels[i].active) {
            slot = &broker->channels[i];
            break;
        }
    }
    if (slot == NULL || broker->next_channel_id == 0U) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = ENOSPC;
        return -1;
    }

    memset(slot, 0, sizeof(*slot));
    slot->capacity = capacity;
    slot->id = broker->next_channel_id++;
    slot->generation = 1U;
    slot->rights = rights;
    slot->active = true;
    if (llam_broker_issue_object_cap_unlocked(broker,
                                              LLAM_BROKER_CAP_FAMILY_CHANNEL,
                                              slot->id,
                                              slot->generation,
                                              rights,
                                              out_token) != 0) {
        memset(slot, 0, sizeof(*slot));
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_channel_send(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             const void *data,
                             size_t length) {
    llam_broker_channel_slot_t *slot;
    llam_broker_channel_message_t *message;

    if (LLAM_UNLIKELY(data == NULL ||
                      length == 0U ||
                      length > LLAM_BROKER_CHANNEL_MESSAGE_BYTES)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    slot = llam_broker_find_channel_unlocked(broker, token, LLAM_CAP_RIGHT_SEND);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    if (slot->closed) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EPIPE;
        return -1;
    }
    if (slot->count >= slot->capacity) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EAGAIN;
        return -1;
    }

    message = &slot->messages[slot->tail];
    memcpy(message->data, data, length);
    message->length = length;
    slot->tail = (slot->tail + 1U) % slot->capacity;
    slot->count += 1U;
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

ssize_t llam_broker_channel_recv(llam_broker_t *broker,
                                 const llam_capability_token_t *token,
                                 void *out_data,
                                 size_t capacity) {
    llam_broker_channel_slot_t *slot;
    llam_broker_channel_message_t *message;
    size_t length;

    if (LLAM_UNLIKELY(out_data == NULL || capacity == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return llam_broker_fail_clear_output_ssize(out_data, capacity, errno);
    }
    if (llam_broker_lock(broker) != 0) {
        int saved_errno = errno;

        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, capacity, saved_errno);
    }
    slot = llam_broker_find_channel_unlocked(broker, token, LLAM_CAP_RIGHT_RECV);
    if (slot == NULL) {
        int saved_errno = errno;

        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, capacity, saved_errno);
    }
    if (slot->count == 0U) {
        int saved_errno = slot->closed ? EPIPE : EAGAIN;

        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, capacity, saved_errno);
    }

    message = &slot->messages[slot->head];
    length = message->length;
    if (LLAM_UNLIKELY(length > capacity)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, capacity, EMSGSIZE);
    }
    memcpy(out_data, message->data, length);
    (void)llam_broker_finish_read_clear_tail(out_data, capacity, (ssize_t)length);
    memset(message, 0, sizeof(*message));
    slot->head = (slot->head + 1U) % slot->capacity;
    slot->count -= 1U;
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return (ssize_t)length;
}

int llam_broker_channel_close(llam_broker_t *broker, const llam_capability_token_t *token) {
    llam_broker_channel_slot_t *slot;

    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    slot = llam_broker_find_channel_unlocked(broker, token, LLAM_CAP_RIGHT_CLOSE);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    slot->closed = true;
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}
