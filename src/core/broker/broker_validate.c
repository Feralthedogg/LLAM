/**
 * @file src/core/broker/broker_validate.c
 * @brief Broker live-object capability validation.
 *
 * @details
 * Token MAC validation proves that a broker issued a token at some point. This
 * module adds the live object-table check that makes per-object generation
 * rotation observable to validation, attenuation, transport, and ring paths.
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

int llam_broker_validate_token_family_unlocked(const llam_broker_t *broker,
                                               const llam_capability_token_t *token,
                                               uint32_t expected_family,
                                               uint64_t required_rights) {
    if (LLAM_UNLIKELY(broker == NULL || token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_cap_unlocked(broker, token, required_rights) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(token->family != expected_family)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int llam_broker_validate_live_object_unlocked(const llam_broker_t *broker,
                                             const llam_capability_token_t *token,
                                             uint64_t required_rights) {
    size_t i;

    if (LLAM_UNLIKELY(broker == NULL || token == NULL)) {
        errno = EINVAL;
        return -1;
    }

    switch (token->family) {
    case LLAM_BROKER_CAP_FAMILY_BUFFER:
        for (i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
            const llam_broker_buffer_slot_t *slot = &broker->buffers[i];

            if (slot->active &&
                slot->id == token->slot &&
                slot->generation == token->generation &&
                (slot->rights & required_rights) == required_rights) {
                return 0;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_DESCRIPTOR:
        for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
            const llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

            if (slot->active &&
                slot->id == token->slot &&
                slot->generation == token->generation &&
                (slot->rights & required_rights) == required_rights) {
                return 0;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_CHANNEL:
        for (i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
            const llam_broker_channel_slot_t *slot = &broker->channels[i];

            if (slot->active &&
                slot->id == token->slot &&
                slot->generation == token->generation &&
                (slot->rights & required_rights) == required_rights) {
                return 0;
            }
        }
        break;
    case LLAM_BROKER_CAP_FAMILY_TASK:
        for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
            const llam_broker_task_slot_t *slot = &broker->tasks[i];

            if (slot->active &&
                slot->id == token->slot &&
                slot->generation == token->generation &&
                (slot->rights & required_rights) == required_rights) {
                return 0;
            }
        }
        break;
    default:
        errno = EACCES;
        return -1;
    }

    errno = EACCES;
    return -1;
}

int llam_broker_fail_clear_output(void *out_data, size_t length, int error_code) {
    if (out_data != NULL && length > 0U) {
        memset(out_data, 0, length);
    }
    errno = error_code != 0 ? error_code : EINVAL;
    return -1;
}

ssize_t llam_broker_fail_clear_output_ssize(void *out_data, size_t length, int error_code) {
    (void)llam_broker_fail_clear_output(out_data, length, error_code);
    return -1;
}

ssize_t llam_broker_finish_read_clear_tail(void *out_data, size_t length, ssize_t result) {
    if (result < 0) {
        return llam_broker_fail_clear_output_ssize(out_data, length, errno);
    }
    if (LLAM_UNLIKELY(out_data == NULL && length > 0U)) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)result < length) {
        memset((unsigned char *)out_data + (size_t)result, 0, length - (size_t)result);
    }
    return result;
}
