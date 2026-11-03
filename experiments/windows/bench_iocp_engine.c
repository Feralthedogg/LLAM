/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */

/* Layer B: real LLAM IOCP node submission/completion with no managed G wake. */
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "runtime_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_BYTES 1024U
#define ENGINE_ROUNDS 4U
#define ENGINE_EXCHANGES 256U

typedef struct engine_state {
    llam_runtime_t *runtime;
    llam_node_t *node;
    SOCKET client;
    SOCKET server;
    llam_io_req_t reads[2];
    atomic_uint completed[2];
    char sent[ENGINE_BYTES];
    char server_received[ENGINE_BYTES];
    char client_received[ENGINE_BYTES];
    uint64_t sends;
    uint64_t bytes_sent;
    uint64_t completions;
} engine_state_t;

static void engine_fail(const char *where, unsigned code) {
    fprintf(stderr, "engine-iocp: %s failed: %u\n", where, code);
    ExitProcess(1U); /* A pending request must not lose its OVERLAPPED storage. */
}

static uint64_t engine_qpc_ns(LARGE_INTEGER ticks, LARGE_INTEGER frequency) {
    return (uint64_t)((long double)ticks.QuadPart * 1000000000.0L /
                      (long double)frequency.QuadPart);
}

static void engine_make_pair(engine_state_t *state) {
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_len = (int)sizeof(address);
    const char no_delay = 1;
    u_long nonblocking = 1UL;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                          WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET ||
        bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
        engine_fail("listener setup", WSAGetLastError());
    }
    state->client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                               WSA_FLAG_OVERLAPPED);
    if (state->client == INVALID_SOCKET ||
        connect(state->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) != 0) {
        engine_fail("client connect", WSAGetLastError());
    }
    state->server = accept(listener, NULL, NULL);
    if (state->server == INVALID_SOCKET ||
        setsockopt(state->client, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0 ||
        setsockopt(state->server, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0 ||
        ioctlsocket(state->client, FIONBIO, &nonblocking) != 0 ||
        ioctlsocket(state->server, FIONBIO, &nonblocking) != 0) {
        engine_fail("server accept/options", WSAGetLastError());
    }
    closesocket(listener);
}

static void engine_post_read(engine_state_t *state, unsigned index,
                             SOCKET socket, char *buffer) {
    llam_io_req_t *req = &state->reads[index];

    atomic_store_explicit(&state->completed[index], 0U, memory_order_relaxed);
    llam_io_req_reset(req, state->runtime, UINT_MAX, UINT_MAX);
    req->kind = LLAM_IO_KIND_READ;
    req->fd = (llam_fd_t)socket;
    req->buf = buffer;
    req->count = ENGINE_BYTES;
    req->windows_test_completion_flag = &state->completed[index];
    if (!llam_node_submit_io_req(state->node, req)) {
        engine_fail("LLAM node submit", (unsigned)errno);
    }
    llam_kick_node(state->node);
}

static void engine_wait_read(engine_state_t *state, unsigned index) {
    unsigned zero = 0U;
    unsigned attempts = 0U;

    while (atomic_load_explicit(&state->completed[index],
                                memory_order_acquire) == 0U) {
        if (!WaitOnAddress(&state->completed[index], &zero, sizeof(zero), 1000U) &&
            GetLastError() != ERROR_TIMEOUT) {
            engine_fail("WaitOnAddress", GetLastError());
        }
        if (++attempts >= 4U) {
            engine_fail("completion timeout", WAIT_TIMEOUT);
        }
    }
    if (state->reads[index].error_code != 0 ||
        state->reads[index].result != ENGINE_BYTES) {
        engine_fail("read completion result",
                    (unsigned)state->reads[index].error_code);
    }
    state->completions += 1U;
}

static void engine_send(engine_state_t *state, SOCKET socket,
                        const char *buffer) {
    size_t sent = 0U;

    while (sent < ENGINE_BYTES) {
        int n = send(socket, buffer + sent, (int)(ENGINE_BYTES - sent), 0);

        if (n <= 0) {
            engine_fail("direct send", n == SOCKET_ERROR ? WSAGetLastError()
                                                          : ERROR_INVALID_DATA);
        }
        state->sends += 1U;
        state->bytes_sent += (unsigned)n;
        sent += (size_t)n;
    }
}

static uint64_t engine_run_once(uint64_t *submissions_out,
                                uint64_t *completions_out,
                                uint64_t *sends_out) {
    llam_runtime_opts_t options;
    engine_state_t state;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    unsigned exchange;
    uint64_t elapsed_ns;

    memset(&state, 0, sizeof(state));
    state.client = INVALID_SOCKET;
    state.server = INVALID_SOCKET;
    if (!QueryPerformanceFrequency(&frequency) ||
        llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        engine_fail("QPF/runtime options", (unsigned)errno);
    }
    options.deterministic = 1U;
    options.experimental_flags = 0U;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        engine_fail("runtime init", (unsigned)errno);
    }
    state.runtime = llam_runtime_default_storage();
    if (state.runtime == NULL || state.runtime->active_nodes != 1U ||
        !state.runtime->nodes[0].thread_started) {
        engine_fail("LLAM IOCP node", ERROR_INVALID_DATA);
    }
    state.node = &state.runtime->nodes[0];
    engine_make_pair(&state);
    QueryPerformanceCounter(&start);
    for (exchange = 0U; exchange < ENGINE_EXCHANGES; ++exchange) {
        unsigned i;

        for (i = 0U; i < ENGINE_BYTES; ++i) {
            state.sent[i] = (char)((exchange + i) & 0xFFU);
        }
        engine_post_read(&state, 0U, state.server, state.server_received);
        engine_post_read(&state, 1U, state.client, state.client_received);
        engine_send(&state, state.client, state.sent);
        engine_wait_read(&state, 0U);
        if (memcmp(state.sent, state.server_received, ENGINE_BYTES) != 0) {
            engine_fail("server record mismatch", ERROR_INVALID_DATA);
        }
        engine_send(&state, state.server, state.server_received);
        engine_wait_read(&state, 1U);
        if (memcmp(state.sent, state.client_received, ENGINE_BYTES) != 0) {
            engine_fail("client record mismatch", ERROR_INVALID_DATA);
        }
    }
    QueryPerformanceCounter(&end);
    elapsed_ns = engine_qpc_ns(end, frequency) - engine_qpc_ns(start, frequency);
    *submissions_out = atomic_load_explicit(&state.node->submit_calls,
                                            memory_order_relaxed);
    *completions_out = state.completions;
    *sends_out = state.sends;
    if (*submissions_out != 2U * ENGINE_EXCHANGES ||
        *completions_out != *submissions_out ||
        *sends_out != 2U * ENGINE_EXCHANGES ||
        state.bytes_sent != 2U * ENGINE_EXCHANGES * ENGINE_BYTES ||
        atomic_load_explicit(&state.node->pending_ops,
                             memory_order_acquire) != 0U) {
        engine_fail("final operation accounting", ERROR_INVALID_DATA);
    }
    llam_runtime_shutdown();
    closesocket(state.client);
    closesocket(state.server);
    return elapsed_ns;
}

int main(void) {
    WSADATA wsa;
    unsigned round;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
        engine_fail("SetPriorityClass", GetLastError());
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        engine_fail("WSAStartup", WSAGetLastError());
    }
    puts("engine-iocp: 1 KiB loopback records, one credit, LLAM node, no managed G wake");
    for (round = 0U; round < ENGINE_ROUNDS; ++round) {
        uint64_t submissions = 0U;
        uint64_t completions = 0U;
        uint64_t sends = 0U;
        uint64_t elapsed_ns = engine_run_once(&submissions, &completions,
                                               &sends);

        printf("engine-iocp: round=%u warmup=%u records=%u elapsed_ns=%llu "
               "ops_per_sec=%.2f submissions=%llu completions=%llu "
               "direct_sends=%llu\n",
               round, round == 0U, ENGINE_EXCHANGES,
               (unsigned long long)elapsed_ns,
               elapsed_ns != 0U
                   ? (double)ENGINE_EXCHANGES * 1000000000.0 /
                         (double)elapsed_ns
                   : 0.0,
               (unsigned long long)submissions,
               (unsigned long long)completions,
               (unsigned long long)sends);
    }
    WSACleanup();
    return 0;
}
