/**
 * @file src/core/runtime_broker_revoke.c
 * @brief Broker-owned object-specific capability revocation.
 *
 * @details
 * Object revocation rotates the broker slot generation instead of exposing MAC
 * keys to clients. Existing tokens remain structurally valid but stop matching
 * the broker-owned object slot, while the broker can return a replacement token
 * with a reduced rights set in the same operation.
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

static int llam_broker_reissue_rotated_cap_unlocked(llam_broker_t *broker,
                                                    uint32_t family,
                                                    uint64_t id,
                                                    uint64_t generation,
                                                    uint64_t slot_rights,
                                                    uint64_t replacement_rights,
                                                    llam_capability_token_t *out_token) {
    if (LLAM_UNLIKELY(out_token == NULL || replacement_rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY((slot_rights & replacement_rights) != replacement_rights)) {
        errno = EACCES;
        return -1;
    }
    return llam_broker_issue_object_cap_unlocked(broker,
                                                family,
                                                id,
                                                generation,
                                                replacement_rights,
                                                out_token);
}

static int llam_broker_prepare_rotated_cap_unlocked(llam_broker_t *broker,
                                                    uint32_t family,
                                                    uint64_t id,
                                                    uint64_t current_generation,
                                                    uint64_t slot_rights,
                                                    uint64_t replacement_rights,
                                                    uint64_t *out_generation,
                                                    llam_capability_token_t *out_token) {
    uint64_t next_generation;

    if (LLAM_UNLIKELY(out_generation == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_generation = 0U;
    if (LLAM_UNLIKELY(current_generation == UINT64_MAX)) {
        errno = EOVERFLOW;
        return -1;
    }
    next_generation = current_generation + 1U;
    /*
     * Token issuance can fail if the broker cannot obtain OS entropy for the
     * replacement nonce. Issue into a temporary token before mutating the live
     * slot so a failed revoke is atomic and leaves the old authority usable.
     */
    if (llam_broker_reissue_rotated_cap_unlocked(broker,
                                                family,
                                                id,
                                                next_generation,
                                                slot_rights,
                                                replacement_rights,
                                                out_token) != 0) {
        return -1;
    }
    *out_generation = next_generation;
    return 0;
}

static int llam_broker_revoke_buffer_unlocked(llam_broker_t *broker,
                                              const llam_capability_token_t *token,
                                              uint64_t replacement_rights,
                                              llam_capability_token_t *out_token) {
    llam_broker_buffer_slot_t *slot = llam_broker_find_buffer_unlocked(broker, token, LLAM_CAP_RIGHT_DESTROY);
    uint64_t next_generation;

    if (slot == NULL) {
        return -1;
    }
    if (LLAM_UNLIKELY((slot->rights & replacement_rights) != replacement_rights)) {
        errno = EACCES;
        return -1;
    }
    if (llam_broker_prepare_rotated_cap_unlocked(broker,
                                                token->family,
                                                slot->id,
                                                slot->generation,
                                                slot->rights,
                                                replacement_rights,
                                                &next_generation,
                                                out_token) != 0) {
        return -1;
    }
    slot->generation = next_generation;
    return 0;
}

static int llam_broker_revoke_descriptor_unlocked(llam_broker_t *broker,
                                                  const llam_capability_token_t *token,
                                                  uint64_t replacement_rights,
                                                  llam_capability_token_t *out_token) {
    size_t i;

    if (LLAM_UNLIKELY(token->family != LLAM_BROKER_CAP_FAMILY_DESCRIPTOR)) {
        errno = EACCES;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        if (slot->active &&
            slot->id == token->slot &&
            slot->generation == token->generation) {
            uint64_t next_generation;

            if (LLAM_UNLIKELY((slot->rights & LLAM_CAP_RIGHT_DESTROY) == 0U ||
                              (slot->rights & replacement_rights) != replacement_rights)) {
                errno = EACCES;
                return -1;
            }
            if (llam_broker_prepare_rotated_cap_unlocked(broker,
                                                        token->family,
                                                        slot->id,
                                                        slot->generation,
                                                        slot->rights,
                                                        replacement_rights,
                                                        &next_generation,
                                                        out_token) != 0) {
                return -1;
            }
            slot->generation = next_generation;
            return 0;
        }
    }
    errno = EACCES;
    return -1;
}

static int llam_broker_revoke_channel_unlocked(llam_broker_t *broker,
                                               const llam_capability_token_t *token,
                                               uint64_t replacement_rights,
                                               llam_capability_token_t *out_token) {
    llam_broker_channel_slot_t *slot = llam_broker_find_channel_unlocked(broker, token, LLAM_CAP_RIGHT_DESTROY);
    uint64_t next_generation;

    if (slot == NULL) {
        return -1;
    }
    if (LLAM_UNLIKELY((slot->rights & replacement_rights) != replacement_rights)) {
        errno = EACCES;
        return -1;
    }
    if (llam_broker_prepare_rotated_cap_unlocked(broker,
                                                token->family,
                                                slot->id,
                                                slot->generation,
                                                slot->rights,
                                                replacement_rights,
                                                &next_generation,
                                                out_token) != 0) {
        return -1;
    }
    slot->generation = next_generation;
    return 0;
}

int llam_broker_revoke_object_cap(llam_broker_t *broker,
                                  const llam_capability_token_t *token,
                                  uint64_t replacement_rights,
                                  llam_capability_token_t *out_token) {
    llam_capability_token_t source;
    llam_capability_token_t replacement;
    int rc = -1;

    if (LLAM_UNLIKELY(out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Revoke rotates object generation and returns replacement authority.
     * Copy first and clear output unconditionally so in-place invalid revoke
     * attempts do not leave the caller holding the old token.
     */
    memset(&source, 0, sizeof(source));
    if (token != NULL) {
        source = *token;
    }
    memset(out_token, 0, sizeof(*out_token));
    if (LLAM_UNLIKELY(token == NULL || replacement_rights == 0U)) {
        errno = EINVAL;
        memset(&source, 0, sizeof(source));
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        memset(&source, 0, sizeof(source));
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        memset(&source, 0, sizeof(source));
        return -1;
    }
    if (llam_broker_validate_cap_unlocked(broker, &source, LLAM_CAP_RIGHT_DESTROY) != 0) {
        goto done;
    }
    memset(&replacement, 0, sizeof(replacement));
    switch (source.family) {
    case LLAM_BROKER_CAP_FAMILY_BUFFER:
        rc = llam_broker_revoke_buffer_unlocked(broker, &source, replacement_rights, &replacement);
        break;
    case LLAM_BROKER_CAP_FAMILY_DESCRIPTOR:
        rc = llam_broker_revoke_descriptor_unlocked(broker, &source, replacement_rights, &replacement);
        break;
    case LLAM_BROKER_CAP_FAMILY_CHANNEL:
        rc = llam_broker_revoke_channel_unlocked(broker, &source, replacement_rights, &replacement);
        break;
    default:
        errno = EINVAL;
        break;
    }
    if (rc == 0) {
        *out_token = replacement;
    }
    memset(&replacement, 0, sizeof(replacement));

done:
    memset(&source, 0, sizeof(source));
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return rc;
}
