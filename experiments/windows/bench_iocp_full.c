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
 * See LICENSE.
 */

/* Layer D: the same one-credit 1 KiB loopback exchange through public LLAM I/O. */
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "llam/runtime.h"
#include "llam/io.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FULL_BYTES 1024U
#define FULL_ROUNDS 4U
#define FULL_EXCHANGES 256U

typedef struct full_state {
    llam_fd_t client;
    llam_fd_t server;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} full_state_t;

static void full_fail(const char *where, int code) {
    fprintf(stderr, "full-llam: %s failed: %d\n", where, code);
    ExitProcess(1U);
}

static uint64_t full_qpc_ns(LARGE_INTEGER ticks, LARGE_INTEGER frequency) {
    return (uint64_t)((long double)ticks.QuadPart * 1000000000.0L /
                      (long double)frequency.QuadPart);
}

static void full_make_pair(llam_fd_t *client_out, llam_fd_t *server_out) {
    SOCKET listener;
    SOCKET client;
    SOCKET server;
    struct sockaddr_in address;
    int address_len = (int)sizeof(address);
    const char no_delay = 1;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                          WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET ||
        bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
        full_fail("listener setup", WSAGetLastError());
    }
    client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED);
    if (client == INVALID_SOCKET ||
        connect(client, (const struct sockaddr *)&address,
                (int)sizeof(address)) != 0) {
        full_fail("client connect", WSAGetLastError());
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET ||
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0 ||
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0) {
        full_fail("server accept/options", WSAGetLastError());
    }
    closesocket(listener);
    *client_out = (llam_fd_t)client;
    *server_out = (llam_fd_t)server;
}

static void full_read_exact(llam_fd_t fd, char *buffer, size_t length) {
    size_t done = 0U;

    while (done < length) {
        ssize_t received = llam_read(fd, buffer + done, length - done);

        if (received <= 0 || (size_t)received > length - done) {
            full_fail("llam_read", errno);
        }
        done += (size_t)received;
    }
}

static void full_write_exact(llam_fd_t fd, const char *buffer, size_t length) {
    size_t done = 0U;

    while (done < length) {
        ssize_t written = llam_write(fd, buffer + done, length - done);

        if (written <= 0 || (size_t)written > length - done) {
            full_fail("llam_write", errno);
        }
        done += (size_t)written;
    }
}

static void full_server_task(void *argument) {
    full_state_t *state = (full_state_t *)argument;
    unsigned exchange;

    for (exchange = 0U; exchange < FULL_EXCHANGES; ++exchange) {
        char record[FULL_BYTES];
        unsigned i;

        full_read_exact(state->server, record, sizeof(record));
        for (i = 0U; i < FULL_BYTES; ++i) {
            if ((unsigned char)record[i] != (unsigned char)((exchange + i) & 0xFFU)) {
                full_fail("server record mismatch", ERROR_INVALID_DATA);
            }
        }
        full_write_exact(state->server, record, sizeof(record));
    }
}

static void full_client_task(void *argument) {
    full_state_t *state = (full_state_t *)argument;
    unsigned exchange;

    QueryPerformanceCounter(&state->start);
    for (exchange = 0U; exchange < FULL_EXCHANGES; ++exchange) {
        char sent[FULL_BYTES];
        char echoed[FULL_BYTES];
        unsigned i;

        for (i = 0U; i < FULL_BYTES; ++i) {
            sent[i] = (char)((exchange + i) & 0xFFU);
        }
        full_write_exact(state->client, sent, sizeof(sent));
        full_read_exact(state->client, echoed, sizeof(echoed));
        if (memcmp(sent, echoed, sizeof(sent)) != 0) {
            full_fail("client echo mismatch", ERROR_INVALID_DATA);
        }
    }
    QueryPerformanceCounter(&state->end);
}

/* Layer C: retain managed park/wake and the same I/O, without record checks. */
static void full_minimal_server_task(void *argument) {
    full_state_t *state = (full_state_t *)argument;
    unsigned exchange;

    for (exchange = 0U; exchange < FULL_EXCHANGES; ++exchange) {
        char record[FULL_BYTES];

        full_read_exact(state->server, record, sizeof(record));
        full_write_exact(state->server, record, sizeof(record));
    }
}

static void full_minimal_client_task(void *argument) {
    full_state_t *state = (full_state_t *)argument;
    char sent[FULL_BYTES];
    char echoed[FULL_BYTES];
    unsigned exchange;

    memset(sent, 0x7b, sizeof(sent));
    QueryPerformanceCounter(&state->start);
    for (exchange = 0U; exchange < FULL_EXCHANGES; ++exchange) {
        full_write_exact(state->client, sent, sizeof(sent));
        full_read_exact(state->client, echoed, sizeof(echoed));
    }
    QueryPerformanceCounter(&state->end);
}

static uint64_t full_run_once(llam_runtime_stats_t *stats_out, bool dump,
                              bool minimal) {
    llam_runtime_opts_t options;
    full_state_t state;
    llam_task_t *server_task;
    llam_task_t *client_task;
    uint64_t elapsed_ns;

    memset(&state, 0, sizeof(state));
    if (!QueryPerformanceFrequency(&state.frequency)) {
        full_fail("QueryPerformanceFrequency", (int)GetLastError());
    }
    full_make_pair(&state.client, &state.server);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        full_fail("runtime opts init", errno);
    }
    options.deterministic = 1U;
    options.experimental_flags = 0U;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        full_fail("runtime init", errno);
    }
    server_task = llam_spawn(minimal ? full_minimal_server_task : full_server_task,
                             &state, NULL);
    client_task = llam_spawn(minimal ? full_minimal_client_task : full_client_task,
                             &state, NULL);
    if (server_task == NULL || client_task == NULL || llam_run() != 0 ||
        llam_join(server_task) != 0 || llam_join(client_task) != 0 ||
        llam_runtime_collect_stats(stats_out) != 0) {
        full_fail("runtime execute/collect", errno);
    }
    elapsed_ns = full_qpc_ns(state.end, state.frequency) -
                 full_qpc_ns(state.start, state.frequency);
    if (dump) {
        llam_dump_runtime_state(2);
    }
    llam_runtime_shutdown();
    closesocket((SOCKET)state.client);
    closesocket((SOCKET)state.server);
    return elapsed_ns;
}

int main(void) {
    WSADATA wsa;
    unsigned round;
    const char *minimal_env = getenv("LLAM_BENCH_MINIMAL");
    bool minimal = minimal_env != NULL && strcmp(minimal_env, "1") == 0;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
        full_fail("SetPriorityClass", (int)GetLastError());
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        full_fail("WSAStartup", WSAGetLastError());
    }
    printf("full-llam: 1 KiB loopback records, one credit, deterministic one-M, "
           "Win10 IOCP batch=64, skip-on-success=off, mode=%s\n",
           minimal ? "minimal-C" : "validated-D");
    for (round = 0U; round < FULL_ROUNDS; ++round) {
        llam_runtime_stats_t stats;
        const char *dump_env = getenv("LLAM_BENCH_DUMP");
        uint64_t elapsed_ns = full_run_once(
            &stats, round == FULL_ROUNDS - 1U && dump_env != NULL &&
                        strcmp(dump_env, "1") == 0, minimal);
        double ops_per_sec = elapsed_ns != 0U
                                 ? (double)FULL_EXCHANGES * 1000000000.0 /
                                       (double)elapsed_ns
                                 : 0.0;

        printf("full-llam: round=%u warmup=%u records=%u elapsed_ns=%llu "
               "ops_per_sec=%.2f ctx_switches=%llu io_submits=%llu "
               "io_completions=%llu active_workers=%u active_nodes=%u\n",
               round, round == 0U, FULL_EXCHANGES,
               (unsigned long long)elapsed_ns, ops_per_sec,
               (unsigned long long)stats.ctx_switches,
               (unsigned long long)stats.io_submits,
               (unsigned long long)stats.io_completions,
               stats.active_workers, stats.active_nodes);
    }
    WSACleanup();
    return 0;
}
