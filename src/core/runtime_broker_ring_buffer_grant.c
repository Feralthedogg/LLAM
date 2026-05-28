/**
 * @file src/core/runtime_broker_ring_buffer_grant.c
 * @brief Broker ring buffer-grant initialization and bounds validation.
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

int llam_broker_buffer_grant_init(llam_broker_buffer_grant_t *grant,
                                  uint64_t grant_id,
                                  uint64_t generation,
                                  uint64_t offset,
                                  uint64_t length,
                                  uint64_t rights,
                                  uint64_t revocation_epoch) {
    uint64_t end;

    if (LLAM_UNLIKELY(grant == NULL || grant_id == 0U || generation == 0U || length == 0U || rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    end = offset + length;
    if (LLAM_UNLIKELY(end < offset)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Grants are internal broker authority. Reject non-buffer rights at the
     * mint boundary so future users cannot accidentally validate unknown bits
     * just because a caller stored them in the grant.
     */
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_BUFFER, rights) != 0) {
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
