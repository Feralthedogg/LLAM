/**
 * @file src/core/runtime_broker_transport_ring.c
 * @brief Broker transport helper for creating shared-memory ring grants.
 *
 * @details
 * The wire dispatcher decides which operation to run. This file owns the
 * platform-specific work needed by CREATE_RING: create a broker-owned mapping,
 * duplicate the client-visible descriptor/HANDLE, register the ring session,
 * and clean up every authority on partial failure.
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

#if !LLAM_PLATFORM_WINDOWS
#include <unistd.h>
#endif

int llam_broker_transport_create_ring(llam_broker_t *broker,
                                      llam_handle_t *out_descriptor,
                                      uint64_t *out_session_id) {
    llam_broker_ring_mapping_t mapping;
    llam_handle_t descriptor = LLAM_INVALID_HANDLE;
    int saved_errno;

    if (out_descriptor != NULL) {
        *out_descriptor = LLAM_INVALID_HANDLE;
    }
    if (out_session_id != NULL) {
        *out_session_id = 0U;
    }
    if (LLAM_UNLIKELY(broker == NULL || out_descriptor == NULL || out_session_id == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(&mapping, 0, sizeof(mapping));
    mapping.fd = -1;
    mapping.mapping_handle = LLAM_INVALID_HANDLE;

#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE duplicate = NULL;

        if (llam_broker_ring_create_private_handle(&mapping) != 0) {
            return -1;
        }
        if (!DuplicateHandle(GetCurrentProcess(),
                             (HANDLE)mapping.mapping_handle,
                             GetCurrentProcess(),
                             &duplicate,
                             0U,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            saved_errno = llam_windows_system_error_to_errno(GetLastError());
            llam_broker_ring_unmap(&mapping);
            errno = saved_errno;
            return -1;
        }
        descriptor = (llam_handle_t)duplicate;
    }
#else
    if (llam_broker_ring_create_private_fd(&mapping) != 0) {
        return -1;
    }
    descriptor = (llam_handle_t)llam_broker_dup_cloexec_fd(mapping.fd);
    if (descriptor < 0) {
        saved_errno = errno;
        llam_broker_ring_unmap(&mapping);
        errno = saved_errno;
        return -1;
    }
#endif

    if (llam_broker_ring_register_mapping(broker,
                                          &mapping,
                                          llam_broker_current_subject(broker),
                                          out_session_id) != 0) {
        saved_errno = errno;
        llam_broker_close_handle(descriptor);
        llam_broker_ring_unmap(&mapping);
        errno = saved_errno;
        return -1;
    }
    *out_descriptor = descriptor;
    return 0;
}
