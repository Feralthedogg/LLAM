/**
 * @file src/core/broker/transport/broker_transport_windows.c
 * @brief Windows named-pipe broker transport helpers.
 *
 * @details
 * Windows cannot attach HANDLEs to a byte stream like POSIX SCM_RIGHTS. The
 * broker authenticates the connected named-pipe peer process and duplicates
 * HANDLE authority across that process boundary.
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

#if LLAM_PLATFORM_WINDOWS

#include <windows.h>

#include <string.h>

#define LLAM_BROKER_WINDOWS_IO_TIMEOUT_MS 250U

int llam_broker_windows_pipe_errno(unsigned long error_code) {
    switch (error_code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_PIPE_BUSY:
        return EAGAIN;
    default:
        return llam_windows_system_error_to_errno(error_code);
    }
}

static int llam_broker_read_exact_handle(HANDLE handle, void *data, size_t len) {
    unsigned char *cursor = (unsigned char *)data;
    size_t done = 0U;
    HANDLE event;
    ULONGLONG deadline;

    if (LLAM_UNLIKELY(handle == NULL ||
                      handle == INVALID_HANDLE_VALUE ||
                      data == NULL ||
                      len == 0U)) {
        return llam_broker_fail_clear_output(data, len, EINVAL);
    }
    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        return llam_broker_fail_clear_output(data, len, llam_broker_windows_pipe_errno(GetLastError()));
    }
    deadline = GetTickCount64() + LLAM_BROKER_WINDOWS_IO_TIMEOUT_MS;
    while (done < len) {
        DWORD chunk = (DWORD)((len - done) > UINT32_MAX ? UINT32_MAX : (len - done));
        DWORD nread = 0U;
        DWORD wait_ms;
        OVERLAPPED overlapped;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline) {
            CloseHandle(event);
            return llam_broker_fail_clear_output(data, len, ETIMEDOUT);
        }
        wait_ms = (DWORD)(deadline - now);

        memset(&overlapped, 0, sizeof(overlapped));
        ResetEvent(event);
        overlapped.hEvent = event;
        if (!ReadFile(handle, cursor + done, chunk, &nread, &overlapped)) {
            DWORD error_code = GetLastError();

            if (error_code != ERROR_IO_PENDING) {
                CloseHandle(event);
                return llam_broker_fail_clear_output(data, len, llam_broker_windows_pipe_errno(error_code));
            }
            error_code = WaitForSingleObject(event, wait_ms);
            if (error_code != WAIT_OBJECT_0) {
                DWORD wait_error = error_code == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;

                (void)CancelIoEx(handle, &overlapped);
                (void)WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
                return llam_broker_fail_clear_output(data,
                                                     len,
                                                     error_code == WAIT_TIMEOUT
                                                         ? ETIMEDOUT
                                                         : llam_broker_windows_pipe_errno(wait_error));
            }
            if (!GetOverlappedResult(handle, &overlapped, &nread, FALSE)) {
                error_code = GetLastError();

                CloseHandle(event);
                return llam_broker_fail_clear_output(data, len, llam_broker_windows_pipe_errno(error_code));
            }
        }
        if (nread == 0U) {
            CloseHandle(event);
            return llam_broker_fail_clear_output(data, len, EPIPE);
        }
        done += (size_t)nread;
    }
    CloseHandle(event);
    return 0;
}

static int llam_broker_write_exact_handle(HANDLE handle, const void *data, size_t len) {
    const unsigned char *cursor = (const unsigned char *)data;
    size_t done = 0U;
    HANDLE event;
    ULONGLONG deadline;

    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    deadline = GetTickCount64() + LLAM_BROKER_WINDOWS_IO_TIMEOUT_MS;
    while (done < len) {
        DWORD chunk = (DWORD)((len - done) > UINT32_MAX ? UINT32_MAX : (len - done));
        DWORD nwritten = 0U;
        DWORD wait_ms;
        OVERLAPPED overlapped;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline) {
            CloseHandle(event);
            errno = ETIMEDOUT;
            return -1;
        }
        wait_ms = (DWORD)(deadline - now);

        memset(&overlapped, 0, sizeof(overlapped));
        ResetEvent(event);
        overlapped.hEvent = event;
        if (!WriteFile(handle, cursor + done, chunk, &nwritten, &overlapped)) {
            DWORD error_code = GetLastError();

            if (error_code != ERROR_IO_PENDING) {
                CloseHandle(event);
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
            error_code = WaitForSingleObject(event, wait_ms);
            if (error_code != WAIT_OBJECT_0) {
                DWORD wait_error = error_code == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;

                (void)CancelIoEx(handle, &overlapped);
                (void)WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
                errno = error_code == WAIT_TIMEOUT
                    ? ETIMEDOUT
                    : llam_broker_windows_pipe_errno(wait_error);
                return -1;
            }
            if (!GetOverlappedResult(handle, &overlapped, &nwritten, FALSE)) {
                error_code = GetLastError();

                CloseHandle(event);
                errno = llam_broker_windows_pipe_errno(error_code);
                return -1;
            }
        }
        if (nwritten == 0U) {
            CloseHandle(event);
            errno = EPIPE;
            return -1;
        }
        done += (size_t)nwritten;
    }
    CloseHandle(event);
    return 0;
}

static void llam_broker_wait_for_pipe_client_finish(HANDLE handle) {
    unsigned char extra_byte = 0U;
    OVERLAPPED overlapped;
    HANDLE event;
    DWORD transferred = 0U;
    DWORD saved_last_error = GetLastError();
    int saved_errno = errno;

    /*
     * DisconnectNamedPipe discards a response that the client has not read
     * yet.  After a graceful STOP, wait for the client to consume the response
     * and close its end.  An extra byte also ends the wait and is discarded:
     * requests after STOP are outside the protocol contract.
     *
     * FlushFileBuffers would provide the same delivery ordering but can block
     * forever when a hostile client stops reading.  Keep this close handshake
     * within the transport's existing I/O deadline instead.
     */
    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        goto done;
    }
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = event;
    if (!ReadFile(handle, &extra_byte, 1U, &transferred, &overlapped)) {
        DWORD error_code = GetLastError();

        if (error_code == ERROR_IO_PENDING) {
            DWORD wait_result =
                WaitForSingleObject(event, LLAM_BROKER_WINDOWS_IO_TIMEOUT_MS);

            if (wait_result == WAIT_OBJECT_0) {
                (void)GetOverlappedResult(handle, &overlapped, &transferred, FALSE);
            } else {
                (void)CancelIoEx(handle, &overlapped);
                (void)WaitForSingleObject(event, INFINITE);
            }
        }
    }
    CloseHandle(event);

done:
    errno = saved_errno;
    SetLastError(saved_last_error);
}

static int llam_broker_duplicate_pipe_client_handle(HANDLE pipe,
                                                    uint64_t client_handle_value,
                                                    llam_handle_t *out_handle) {
    HANDLE client_process;
    HANDLE duplicate = NULL;
    DWORD client_pid = 0U;
    uintptr_t raw_handle;

    if (LLAM_UNLIKELY(out_handle == NULL || client_handle_value > (uint64_t)UINTPTR_MAX)) {
        errno = EINVAL;
        return -1;
    }
    *out_handle = LLAM_INVALID_HANDLE;
    raw_handle = (uintptr_t)client_handle_value;
    if (LLAM_UNLIKELY(raw_handle == 0U ||
                      raw_handle == (uintptr_t)INVALID_HANDLE_VALUE)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Windows named pipes cannot attach HANDLEs like SCM_RIGHTS. The broker
     * therefore derives authority from the connected pipe peer process and
     * duplicates that peer's handle into broker ownership. The wire value only
     * selects a handle in the already-authenticated peer process.
     */
    if (!GetNamedPipeClientProcessId(pipe, &client_pid)) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    client_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
    if (client_process == NULL) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    if (!DuplicateHandle(client_process,
                         (HANDLE)raw_handle,
                         GetCurrentProcess(),
                         &duplicate,
                         0U,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        int saved_errno = llam_broker_windows_pipe_errno(GetLastError());

        CloseHandle(client_process);
        errno = saved_errno;
        return -1;
    }
    CloseHandle(client_process);
    *out_handle = (llam_handle_t)duplicate;
    return 0;
}

static void llam_broker_close_pipe_client_duplicate(HANDLE client_process, uint64_t client_handle_value) {
    HANDLE local_duplicate = NULL;

    if (client_process == NULL ||
        client_handle_value == 0U ||
        client_handle_value > (uint64_t)UINTPTR_MAX ||
        (uintptr_t)client_handle_value == (uintptr_t)INVALID_HANDLE_VALUE) {
        return;
    }
    /*
     * CREATE_RING duplicates a mapping HANDLE into the pipe peer before the
     * response bytes are written. If the pipe write then fails, the peer never
     * learns the numeric HANDLE value, so the broker must close the source in
     * the peer handle table to avoid leaking authority into a hostile client.
     */
    if (DuplicateHandle(client_process,
                        (HANDLE)(uintptr_t)client_handle_value,
                        GetCurrentProcess(),
                        &local_duplicate,
                        0U,
                        FALSE,
                        DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
        if (local_duplicate != NULL) {
            CloseHandle(local_duplicate);
        }
    }
}

static int llam_broker_duplicate_handle_to_pipe_client(HANDLE pipe,
                                                       llam_handle_t broker_handle,
                                                       HANDLE *out_client_process,
                                                       uint64_t *out_client_handle_value) {
    HANDLE client_process, client_handle = NULL;
    DWORD client_pid = 0U;

    if (out_client_process != NULL) {
        *out_client_process = NULL;
    }
    if (out_client_handle_value != NULL) {
        *out_client_handle_value = 0U;
    }
    if (LLAM_UNLIKELY(llam_handle_is_invalid(broker_handle) ||
                      out_client_process == NULL ||
                      out_client_handle_value == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (!GetNamedPipeClientProcessId(pipe, &client_pid)) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    client_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
    if (client_process == NULL) {
        errno = llam_broker_windows_pipe_errno(GetLastError());
        return -1;
    }
    if (!DuplicateHandle(GetCurrentProcess(),
                         (HANDLE)broker_handle,
                         client_process,
                         &client_handle,
                         0U,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        int saved_errno = llam_broker_windows_pipe_errno(GetLastError());

        CloseHandle(client_process);
        errno = saved_errno;
        return -1;
    }
    *out_client_process = client_process;
    *out_client_handle_value = (uint64_t)(uintptr_t)client_handle;
    return 0;
}

static int llam_broker_serve_one_handle_subject(llam_broker_t *broker,
                                                llam_handle_t handle,
                                                uint64_t subject_id,
                                                bool *out_should_close) {
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_handle_t descriptor_handle = LLAM_INVALID_HANDLE;
    llam_handle_t response_descriptor = LLAM_INVALID_HANDLE;
    HANDLE response_client_process = NULL;
    uint64_t response_client_handle_value = 0U;

    if (out_should_close != NULL) {
        *out_should_close = false;
    }
    if (LLAM_UNLIKELY(llam_handle_is_invalid(handle))) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_read_exact_handle((HANDLE)handle, &request, sizeof(request)) != 0) {
        return -1;
    }
    if (request.magic == LLAM_BROKER_WIRE_MAGIC &&
        request.version == LLAM_BROKER_WIRE_VERSION &&
        request.op == (uint32_t)LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR &&
        llam_broker_duplicate_pipe_client_handle((HANDLE)handle, request.slot, &descriptor_handle) != 0) {
        descriptor_handle = LLAM_INVALID_HANDLE;
    }
    if (llam_broker_begin_op_subject(broker, subject_id) != 0) {
        if (!llam_handle_is_invalid(descriptor_handle)) {
            llam_broker_close_handle(descriptor_handle);
        }
        return -1;
    }
    llam_broker_process_request_with_descriptors(broker,
                                                 &request,
                                                 &response,
                                                 out_should_close,
                                                 descriptor_handle,
                                                 &response_descriptor);
    if (response.status != 0) {
        llam_broker_normalize_response_failure_outputs(&response);
        if (!llam_handle_is_invalid(response_descriptor)) {
            llam_broker_close_handle(response_descriptor);
            response_descriptor = LLAM_INVALID_HANDLE;
        }
    }
    if (!llam_handle_is_invalid(response_descriptor)) {
        uint64_t client_handle_value = 0U;

        if (llam_broker_duplicate_handle_to_pipe_client((HANDLE)handle,
                                                        response_descriptor,
                                                        &response_client_process,
                                                        &client_handle_value) == 0) {
            response.result0 = client_handle_value;
            response_client_handle_value = client_handle_value;
        } else {
            int saved_errno = errno;

            llam_broker_rollback_created_response(broker, &request, &response, subject_id);
            llam_broker_mark_response_failure_clear_outputs(&response, saved_errno);
        }
        llam_broker_close_handle(response_descriptor);
    }
    if (llam_broker_write_exact_handle((HANDLE)handle, &response, sizeof(response)) != 0) {
        int saved_errno = errno;

        llam_broker_close_pipe_client_duplicate(response_client_process, response_client_handle_value);
        if (response_client_process != NULL) {
            CloseHandle(response_client_process);
        }
        llam_broker_rollback_created_response(broker, &request, &response, subject_id);
        llam_broker_end_op(broker);
        errno = saved_errno;
        return -1;
    }
    if (response_client_process != NULL) {
        CloseHandle(response_client_process);
    }
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_serve_one_handle(llam_broker_t *broker, llam_handle_t handle, bool *out_should_close) {
    uintptr_t transport_id = (uintptr_t)handle;
    uint64_t subject_id;
    int rc;

    if (llam_broker_transport_subject(broker, transport_id, &subject_id) != 0) {
        return -1;
    }
    rc = llam_broker_serve_one_handle_subject(broker, handle, subject_id, out_should_close);

    if (rc != 0 || (out_should_close != NULL && *out_should_close)) {
        llam_broker_forget_transport_subject(broker, transport_id);
    }
    return rc;
}

int llam_broker_serve_handle(llam_broker_t *broker, llam_handle_t handle) {
    bool should_close = false;
    uintptr_t transport_id = (uintptr_t)handle;
    uint64_t subject_id;
    uint64_t session_start_ns;
    size_t request_count = 0U;
    int rc = 0;

    if (llam_broker_transport_subject(broker, transport_id, &subject_id) != 0) {
        return -1;
    }
    session_start_ns = llam_now_ns();
    while (!should_close) {
        uint64_t now_ns = llam_now_ns();

        if (request_count >= LLAM_BROKER_SESSION_REQUEST_MAX ||
            (session_start_ns != 0U && now_ns != 0U &&
             now_ns >= session_start_ns &&
             now_ns - session_start_ns >= LLAM_BROKER_SESSION_LIFETIME_NS)) {
            break;
        }
        if (llam_broker_serve_one_handle_subject(broker, handle, subject_id, &should_close) != 0) {
            rc = -1;
            break;
        }
        ++request_count;
    }
    if (rc == 0 && should_close) {
        llam_broker_wait_for_pipe_client_finish((HANDLE)handle);
    }
    llam_broker_forget_transport_subject(broker, transport_id);
    return rc;
}

int llam_broker_request_handle(llam_handle_t handle,
                               const llam_broker_wire_request_t *request,
                               llam_broker_wire_response_t *response) {
    int rc;

    if (LLAM_UNLIKELY(llam_handle_is_invalid(handle) || request == NULL || response == NULL)) {
        return llam_broker_fail_clear_output(response,
                                             response != NULL ? sizeof(*response) : 0U,
                                             EINVAL);
    }
    memset(response, 0, sizeof(*response));
    if (llam_broker_write_exact_handle((HANDLE)handle, request, sizeof(*request)) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    rc = llam_broker_read_exact_handle((HANDLE)handle, response, sizeof(*response));
    if (rc == 0) {
        rc = llam_broker_validate_response_frame_or_clear(response);
    }
    return rc;
}

int llam_broker_request_handle_with_descriptor(llam_handle_t handle,
                                               const llam_broker_wire_request_t *request,
                                               llam_handle_t descriptor_handle,
                                               llam_broker_wire_response_t *response) {
    llam_broker_wire_request_t local_request;
    int rc;

    if (LLAM_UNLIKELY(llam_handle_is_invalid(handle) ||
                      request == NULL ||
                      llam_handle_is_invalid(descriptor_handle) ||
                      response == NULL)) {
        return llam_broker_fail_clear_output(response,
                                             response != NULL ? sizeof(*response) : 0U,
                                             EINVAL);
    }
    memset(response, 0, sizeof(*response));
    local_request = *request;
    local_request.slot = (uint64_t)(uintptr_t)descriptor_handle;
    if (llam_broker_write_exact_handle((HANDLE)handle, &local_request, sizeof(local_request)) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    rc = llam_broker_read_exact_handle((HANDLE)handle, response, sizeof(*response));
    if (rc == 0) {
        rc = llam_broker_validate_response_frame_or_clear(response);
    }
    return rc;
}

#endif
