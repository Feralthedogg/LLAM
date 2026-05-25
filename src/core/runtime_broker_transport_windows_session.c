/**
 * @file src/core/runtime_broker_transport_windows_session.c
 * @brief Windows named-pipe broker local session helpers.
 *
 * @details
 * This layer owns named-pipe accept/session orchestration. Keeping it separate
 * from HANDLE duplication and exact pipe I/O keeps the Windows transport split
 * aligned with the POSIX Unix-domain transport layout.
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

#if LLAM_PLATFORM_WINDOWS

#include <windows.h>

int llam_broker_serve_unix_once(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_local_once(broker, path);
}

int llam_broker_client_self_test_unix(const char *path) {
    return llam_broker_client_self_test_local(path);
}

int llam_broker_serve_local_n(llam_broker_t *broker, const char *path, size_t max_connections) {
    llam_handle_t pipe = LLAM_INVALID_HANDLE;
    int rc = -1;
    int last_session_errno = 0;
    size_t served = 0U;
    size_t successful = 0U;

    if (LLAM_UNLIKELY(broker == NULL || max_connections == 0U)) {
        errno = EINVAL;
        return -1;
    }
    while (served < max_connections) {
        pipe = LLAM_INVALID_HANDLE;
        if (llam_broker_listen_pipe(path, &pipe) != 0) {
            if (served > 0U && successful > 0U) {
                return 0;
            }
            if (served > 0U && last_session_errno != 0) {
                errno = last_session_errno;
            }
            return -1;
        }
        if (!ConnectNamedPipe((HANDLE)pipe, NULL)) {
            DWORD error_code = GetLastError();

            if (error_code != ERROR_PIPE_CONNECTED) {
                errno = llam_broker_windows_pipe_errno(error_code);
                llam_broker_close_handle(pipe);
                return -1;
            }
        }
        /*
         * Named pipes create one kernel pipe instance per accepted session.
         * STOP closes only that session; the broker can create a fresh instance
         * for the next client without sharing subject ids or HANDLE authority.
         * Malformed clients therefore fail their own session without terminating
         * the long-running broker process.
         */
        rc = llam_broker_serve_handle(broker, pipe);
        if (rc == 0) {
            successful++;
        } else {
            last_session_errno = errno != 0 ? errno : EIO;
        }
        (void)DisconnectNamedPipe((HANDLE)pipe);
        llam_broker_close_handle(pipe);
        served++;
    }
    if (successful == 0U) {
        errno = last_session_errno != 0 ? last_session_errno : EPIPE;
        return -1;
    }
    return 0;
}

int llam_broker_serve_local_once(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_local_n(broker, path, 1U);
}

int llam_broker_serve_local(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_local_n(broker, path, (size_t)-1);
}

typedef struct llam_broker_handle_transport {
    llam_handle_t handle;
} llam_broker_handle_transport_t;

static int llam_broker_request_handle_adapter(void *transport,
                                              const llam_broker_wire_request_t *request,
                                              llam_broker_wire_response_t *response) {
    llam_broker_handle_transport_t *state = (llam_broker_handle_transport_t *)transport;

    return llam_broker_request_handle(state->handle, request, response);
}

int llam_broker_client_self_test_local(const char *path) {
    llam_broker_handle_transport_t transport;
    int rc;

    transport.handle = LLAM_INVALID_HANDLE;
    if (llam_broker_connect_pipe(path, &transport.handle) != 0) {
        return -1;
    }
    rc = llam_broker_client_self_test_exchange(llam_broker_request_handle_adapter, &transport);
    llam_broker_close_handle(transport.handle);
    return rc;
}

#endif
