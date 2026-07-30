/**
 * @file tests/test_security_capability_windows_session.inc
 * @brief Windows named-pipe broker session-boundary regression checks.
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

static int broker_windows_wait_pipe_bytes(HANDLE handle,
                                          DWORD expected_bytes,
                                          DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    for (;;) {
        DWORD available = 0U;

        if (!PeekNamedPipe(handle, NULL, 0U, NULL, &available, NULL)) {
            errno = llam_broker_windows_pipe_errno(GetLastError());
            return -1;
        }
        if (available >= expected_bytes) {
            return 0;
        }
        if (GetTickCount64() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        Sleep(1U);
    }
}

static int test_broker_pipe_session_request_budget_returns_to_listener(void) {
    char name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_pipe_local_server_state_t state;
    HANDLE server_thread = NULL;
    llam_handle_t attacker = LLAM_INVALID_HANDLE;
    llam_handle_t victim = LLAM_INVALID_HANDLE;
    llam_broker_wire_request_t stop_request;
    llam_broker_wire_response_t stop_response;
    bool broker_initialized = false;
    bool server_stopped = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    (void)snprintf(name,
                   sizeof(name),
                   "llam-broker-session-budget-%lu-%llu",
                   (unsigned long)GetCurrentProcessId(),
                   (unsigned long long)GetTickCount64());
    memset(&state, 0, sizeof(state));
    state.broker = &broker;
    state.name = name;
    state.max_connections = 2U;
    state.rc = -1;
    server_thread = CreateThread(NULL, 0U, broker_pipe_local_server_thread, &state, 0U, NULL);
    if (server_thread == NULL || llam_broker_connect_pipe(name, &attacker) != 0) {
        fprintf(stderr, "[test_security_capability] session-budget server setup failed errno=%d\n", errno);
        goto done;
    }
    for (size_t i = 0U; i < LLAM_BROKER_SESSION_REQUEST_MAX; ++i) {
        if (broker_pipe_request_ok(attacker, LLAM_BROKER_WIRE_OP_PING) != 0) {
            fprintf(stderr,
                    "[test_security_capability] Windows session budget rejected request %zu errno=%d\n",
                    i,
                    errno);
            goto done;
        }
    }
    /* Keep the first client handle open. A vulnerable unbounded serve loop
     * remains parked on it and never makes the exclusive pipe available to the
     * victim. The fixed loop disconnects after the bounded request count. */
    if (llam_broker_connect_pipe(name, &victim) != 0 ||
        broker_pipe_request_ok(victim, LLAM_BROKER_WIRE_OP_PING) != 0) {
        fprintf(stderr,
                "[test_security_capability] Windows session budget did not return to listener errno=%d\n",
                errno);
        goto done;
    }
    /*
     * Delay the STOP response read so an immediate DisconnectNamedPipe would
     * deterministically discard it.  The graceful-close handshake must retain
     * the response without using an unbounded FlushFileBuffers call.
     */
    request_init(&stop_request, LLAM_BROKER_WIRE_OP_STOP);
    memset(&stop_response, 0, sizeof(stop_response));
    if (broker_windows_overlapped_exact((HANDLE)victim,
                                        true,
                                        &stop_request,
                                        (DWORD)sizeof(stop_request)) != 0) {
        fprintf(stderr,
                "[test_security_capability] Windows session budget STOP write failed errno=%d\n",
                errno);
        goto done;
    }
    if (broker_windows_wait_pipe_bytes((HANDLE)victim,
                                       (DWORD)sizeof(stop_response),
                                       2000U) != 0) {
        fprintf(stderr,
                "[test_security_capability] Windows session budget STOP response was not buffered errno=%d\n",
                errno);
        goto done;
    }
    Sleep(50U);
    if (broker_windows_overlapped_exact((HANDLE)victim,
                                        false,
                                        &stop_response,
                                        (DWORD)sizeof(stop_response)) != 0 ||
        llam_broker_validate_response_frame_or_clear(&stop_response) != 0 ||
        stop_response.status != 0) {
        fprintf(stderr,
                "[test_security_capability] Windows session budget lost STOP response errno=%d\n",
                errno);
        goto done;
    }
    llam_broker_close_handle(victim);
    victim = LLAM_INVALID_HANDLE;
    if (WaitForSingleObject(server_thread, 5000U) != WAIT_OBJECT_0 || state.rc != 0) {
        goto done;
    }
    server_stopped = true;
    rc = 0;

done:
    if (!llam_handle_is_invalid(attacker)) {
        llam_broker_close_handle(attacker);
    }
    if (!llam_handle_is_invalid(victim)) {
        llam_broker_close_handle(victim);
    }
    if (server_thread != NULL) {
        if (!server_stopped) {
            for (unsigned i = 0U; i < 3U; ++i) {
                (void)broker_pipe_connect_and_close(name);
            }
            if (WaitForSingleObject(server_thread, 5000U) != WAIT_OBJECT_0) {
                (void)TerminateThread(server_thread, 1U);
                broker_initialized = false;
            }
        }
        CloseHandle(server_thread);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}
