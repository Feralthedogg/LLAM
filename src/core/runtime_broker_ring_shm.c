/**
 * @file src/core/runtime_broker_ring_shm.c
 * @brief Platform-neutral broker ring shared-memory helpers.
 *
 * @details
 * This unit keeps the validation and high-entropy private-name generation used
 * by both POSIX and Windows mapping backends. Platform-specific mapping,
 * descriptor/HANDLE ownership, and cleanup live in separate source files.
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

#include <stdio.h>
#include <string.h>

bool llam_broker_ring_mapping_ring_valid(const llam_broker_ring_t *ring) {
    return ring != NULL &&
           ring->magic == LLAM_BROKER_RING_MAGIC &&
           ring->version == LLAM_BROKER_RING_VERSION &&
           ring->capacity == LLAM_BROKER_RING_CAP;
}

bool llam_broker_ring_name_valid(const char *name) {
    size_t len;

    if (name == NULL) {
        return false;
    }
    len = strlen(name);
    if (len == 0U || len >= sizeof(((llam_broker_ring_mapping_t *)0)->name)) {
        return false;
    }
#if LLAM_PLATFORM_WINDOWS
    return true;
#else
    return name[0] == '/' && len > 1U;
#endif
}

int llam_broker_ring_private_name(char *out_name, size_t out_name_len) {
    uint64_t a;
    uint64_t b;
    int written;

    if (LLAM_UNLIKELY(out_name == NULL || out_name_len == 0U)) {
        errno = EINVAL;
        return -1;
    }
    memset(out_name, 0, out_name_len);
    if (LLAM_UNLIKELY(!llam_public_slot_entropy_from_os(&a) ||
                      !llam_public_slot_entropy_from_os(&b) ||
                      (a | b) == 0U)) {
        errno = EIO;
        return -1;
    }
    a = llam_public_slot_mix64(a);
    b = llam_public_slot_mix64(b ^ UINT64_C(0x9e3779b97f4a7c15));
#if LLAM_PLATFORM_WINDOWS
    written = snprintf(out_name,
                       out_name_len,
                       "Local\\llam-broker-ring-%016llx%016llx",
                       (unsigned long long)a,
                       (unsigned long long)b);
#else
    a ^= b;
    written = snprintf(out_name,
                       out_name_len,
                       "/llambr-%016llx",
                       (unsigned long long)a);
#endif
    if (LLAM_UNLIKELY(written < 0 || (size_t)written >= out_name_len)) {
        memset(out_name, 0, out_name_len);
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

void llam_broker_ring_mapping_reset(llam_broker_ring_mapping_t *mapping) {
    if (mapping == NULL) {
        return;
    }
    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    mapping->mapping_handle = LLAM_INVALID_HANDLE;
}

int llam_broker_ring_create_private_shm(llam_broker_ring_mapping_t *out_mapping) {
    char name[sizeof(((llam_broker_ring_mapping_t *)0)->name)];
    size_t attempt;

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (LLAM_UNLIKELY(out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Named mappings are still bearer rendezvous objects, but tests and broker
     * setup should not rely on predictable pid-based names. Generate enough
     * entropy that accidental or same-UID opportunistic guessing is not the
     * default path. Full hostile-client isolation still requires passing the
     * mapping authority over a subject-bound broker transport.
     */
    for (attempt = 0U; attempt < 8U; ++attempt) {
        if (llam_broker_ring_private_name(name, sizeof(name)) != 0) {
            return -1;
        }
        if (llam_broker_ring_create_shm(name, out_mapping) == 0) {
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}
