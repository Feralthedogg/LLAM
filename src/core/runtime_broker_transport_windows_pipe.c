/**
 * @file src/core/runtime_broker_transport_windows_pipe.c
 * @brief Windows named-pipe lifecycle and security descriptor helpers.
 *
 * @details
 * Named-pipe objects are the Windows broker process boundary. Keeping DACL
 * setup and pipe connection retry logic here makes the request-serving unit
 * focus only on wire-message handling and HANDLE authority transfer.
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
#include "runtime_broker_windows_security.h"

#if LLAM_PLATFORM_WINDOWS

#include <string.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008U
#endif

static int llam_broker_pipe_name(const char *path, char *out, size_t out_size) {
    int written;

    if (path == NULL || out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (strncmp(path, "\\\\.\\pipe\\", 9U) == 0) {
        written = snprintf(out, out_size, "%s", path);
    } else {
        written = snprintf(out, out_size, "\\\\.\\pipe\\%s", path);
    }
    if (written < 0 || (size_t)written >= out_size) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_broker_listen_pipe(const char *name, llam_handle_t *out_handle) {
    char pipe_name[256];
    llam_broker_windows_security_t security;
    HANDLE handle;

    if (LLAM_UNLIKELY(name == NULL || out_handle == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_pipe_name(name, pipe_name, sizeof(pipe_name)) != 0) {
        return -1;
    }
    if (llam_broker_windows_security_init(&security, GENERIC_ALL) != 0) {
        return -1;
    }
    handle = CreateNamedPipeA(pipe_name,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              1U,
                              (DWORD)(sizeof(llam_broker_wire_response_t) * 4U),
                              (DWORD)(sizeof(llam_broker_wire_request_t) * 4U),
                              5000U,
                              &security.attrs);
    llam_broker_windows_security_cleanup(&security);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    *out_handle = (llam_handle_t)handle;
    return 0;
}

int llam_broker_connect_pipe(const char *name, llam_handle_t *out_handle) {
    char pipe_name[256];
    ULONGLONG deadline;
    DWORD mode = PIPE_READMODE_BYTE;

    if (LLAM_UNLIKELY(name == NULL || out_handle == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_pipe_name(name, pipe_name, sizeof(pipe_name)) != 0) {
        return -1;
    }
    *out_handle = LLAM_INVALID_HANDLE;
    deadline = GetTickCount64() + 5000U;
    for (;;) {
        HANDLE handle = CreateFileA(pipe_name,
                                    GENERIC_READ | GENERIC_WRITE,
                                    0U,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            if (!SetNamedPipeHandleState(handle, &mode, NULL, NULL)) {
                DWORD error_code = GetLastError();

                CloseHandle(handle);
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
            *out_handle = (llam_handle_t)handle;
            return 0;
        }
        {
            DWORD error_code = GetLastError();
            ULONGLONG now = GetTickCount64();

            if (error_code != ERROR_FILE_NOT_FOUND && error_code != ERROR_PIPE_BUSY) {
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
            if (now >= deadline) {
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
        }
        /*
         * WaitNamedPipe reports ERROR_FILE_NOT_FOUND before the server has
         * created the pipe object. Retry both not-found and busy states so a
         * client self-test can start immediately after the server process.
         */
        if (!WaitNamedPipeA(pipe_name, 100U)) {
            DWORD error_code = GetLastError();

            if (error_code != ERROR_FILE_NOT_FOUND && error_code != ERROR_SEM_TIMEOUT) {
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
            Sleep(10U);
        }
    }
}

void llam_broker_close_handle(llam_handle_t handle) {
    if (!llam_handle_is_invalid(handle)) {
        (void)CloseHandle((HANDLE)handle);
    }
}

#endif
