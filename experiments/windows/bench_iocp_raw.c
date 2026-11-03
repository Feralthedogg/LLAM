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

/* Layer A: one-credit, 1 KiB overlapped loopback exchange without LLAM. */
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAW_BYTES 1024U
#define RAW_BATCH 64U
#define RAW_ROUNDS 4U
#define RAW_EXCHANGES 256U

typedef enum raw_op_kind {
    RAW_CLIENT_SEND,
    RAW_SERVER_RECV,
    RAW_SERVER_SEND,
    RAW_CLIENT_RECV,
} raw_op_kind_t;

typedef struct raw_op {
    OVERLAPPED overlapped; /* lpOverlapped identifies this stable operation. */
    SOCKET socket;
    WSABUF wsabuf;
    DWORD flags;
    unsigned done;
    raw_op_kind_t kind;
    char data[RAW_BYTES];
} raw_op_t;

typedef struct raw_result {
    uint64_t elapsed_ns;
    uint64_t dequeue_ns;
    uint64_t thread_cpu_100ns;
    uint64_t submitted;
    uint64_t immediate;
    uint64_t pending;
    uint64_t completions;
    uint64_t dequeue_calls;
    uint64_t timeouts;
    uint64_t full_batches;
    uint64_t bytes;
    uint64_t direct_sends;
    uint64_t direct_bytes;
} raw_result_t;

typedef struct raw_run {
    HANDLE port;
    SOCKET client;
    SOCKET server;
    raw_op_t ops[4];
    raw_result_t result;
    unsigned exchange;
    unsigned outstanding;
    bool done[4];
    bool reads_only;
} raw_run_t;

static void raw_fail(const char *where, unsigned code) {
    fprintf(stderr, "raw-iocp: %s failed: %u\n", where, code);
    ExitProcess(1U); /* Pending OVERLAPPED storage must not be reused. */
}

static uint64_t raw_qpc_ns(LARGE_INTEGER ticks, LARGE_INTEGER frequency) {
    return (uint64_t)((long double)ticks.QuadPart * 1000000000.0L /
                      (long double)frequency.QuadPart);
}

static uint64_t raw_thread_cpu_100ns(void) {
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER k;
    ULARGE_INTEGER u;

    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        raw_fail("GetThreadTimes", GetLastError());
    }
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

static void raw_make_pair(SOCKET *client_out, SOCKET *server_out) {
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_len = (int)sizeof(address);
    const char no_delay = 1;
    u_long nonblocking = 1UL;

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
        raw_fail("listener setup", WSAGetLastError());
    }
    client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED);
    if (client == INVALID_SOCKET ||
        connect(client, (const struct sockaddr *)&address,
                (int)sizeof(address)) != 0) {
        raw_fail("client connect", WSAGetLastError());
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET ||
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0 ||
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                   (int)sizeof(no_delay)) != 0 ||
        ioctlsocket(client, FIONBIO, &nonblocking) != 0 ||
        ioctlsocket(server, FIONBIO, &nonblocking) != 0) {
        raw_fail("server accept/options", WSAGetLastError());
    }
    closesocket(listener);
    *client_out = client;
    *server_out = server;
}

static void raw_send_direct(raw_run_t *run, raw_op_t *op) {
    while (op->done < RAW_BYTES) {
        int sent = send(op->socket, op->data + op->done,
                        (int)(RAW_BYTES - op->done), 0);

        if (sent <= 0) {
            /* One-credit 1 KiB loopback should not encounter backpressure. */
            raw_fail("direct send", sent == SOCKET_ERROR ? WSAGetLastError()
                                                        : ERROR_INVALID_DATA);
        }
        run->result.direct_sends += 1U;
        run->result.direct_bytes += (unsigned)sent;
        op->done += (unsigned)sent;
    }
    run->done[op->kind] = true;
}

static void raw_post(raw_run_t *run, raw_op_t *op) {
    DWORD transferred = 0U;
    int rc;

    if (op->done >= RAW_BYTES) {
        raw_fail("post length", ERROR_INVALID_DATA);
    }
    memset(&op->overlapped, 0, sizeof(op->overlapped));
    op->wsabuf.buf = op->data + op->done;
    op->wsabuf.len = RAW_BYTES - op->done;
    op->flags = 0U;
    if (op->kind == RAW_CLIENT_SEND || op->kind == RAW_SERVER_SEND) {
        rc = WSASend(op->socket, &op->wsabuf, 1U, &transferred, 0U,
                     &op->overlapped, NULL);
    } else {
        rc = WSARecv(op->socket, &op->wsabuf, 1U, &transferred, &op->flags,
                     &op->overlapped, NULL);
    }
    run->result.submitted += 1U;
    run->outstanding += 1U;
    if (rc == 0) {
        /* Skip-on-success is off; even synchronous success posts an IOCP packet. */
        run->result.immediate += 1U;
        return;
    }
    if (WSAGetLastError() == WSA_IO_PENDING) {
        run->result.pending += 1U;
        return;
    }
    raw_fail("WSARecv/WSASend", WSAGetLastError());
}

static void raw_start_exchange(raw_run_t *run) {
    unsigned i;

    memset(run->done, 0, sizeof(run->done));
    for (i = 0U; i < 4U; ++i) {
        run->ops[i].done = 0U;
    }
    memset(run->ops[RAW_SERVER_RECV].data, 0, RAW_BYTES);
    memset(run->ops[RAW_CLIENT_RECV].data, 0, RAW_BYTES);
    for (i = 0U; i < RAW_BYTES; ++i) {
        run->ops[RAW_CLIENT_SEND].data[i] =
            (char)((run->exchange + i) & 0xFFU);
    }
    raw_post(run, &run->ops[RAW_SERVER_RECV]);
    raw_post(run, &run->ops[RAW_CLIENT_RECV]);
    if (run->reads_only) {
        raw_send_direct(run, &run->ops[RAW_CLIENT_SEND]);
    } else {
        raw_post(run, &run->ops[RAW_CLIENT_SEND]);
    }
}

static void raw_complete(raw_run_t *run, const OVERLAPPED_ENTRY *entry) {
    uintptr_t pointer = (uintptr_t)entry->lpOverlapped;
    uintptr_t begin = (uintptr_t)&run->ops[0];
    uintptr_t end = begin + sizeof(run->ops);
    raw_op_t *op;
    DWORD transferred = 0U;
    DWORD flags = 0U;
    unsigned kind;

    if (pointer < begin || pointer >= end ||
        (pointer - begin) % sizeof(raw_op_t) != 0U ||
        run->outstanding == 0U) {
        raw_fail("unknown or duplicate completion", ERROR_INVALID_DATA);
    }
    op = (raw_op_t *)pointer;
    kind = (unsigned)op->kind;
    if (run->done[kind] ||
        !WSAGetOverlappedResult(op->socket, &op->overlapped, &transferred,
                                FALSE, &flags)) {
        raw_fail("overlapped result", WSAGetLastError());
    }
    if (transferred != entry->dwNumberOfBytesTransferred) {
        raw_fail("completion byte disagreement", ERROR_INVALID_DATA);
    }
    run->outstanding -= 1U;
    run->result.completions += 1U;
    run->result.bytes += transferred;
    if (transferred == 0U || transferred > RAW_BYTES - op->done) {
        raw_fail("zero/oversized completion", ERROR_INVALID_DATA);
    }
    op->done += transferred;
    if (op->done < RAW_BYTES) {
        raw_post(run, op);
        return;
    }
    run->done[kind] = true;
    if (op->kind == RAW_SERVER_RECV) {
        if (memcmp(op->data, run->ops[RAW_CLIENT_SEND].data, RAW_BYTES) != 0) {
            raw_fail("server request mismatch", ERROR_INVALID_DATA);
        }
        memcpy(run->ops[RAW_SERVER_SEND].data, op->data, RAW_BYTES);
        if (run->reads_only) {
            raw_send_direct(run, &run->ops[RAW_SERVER_SEND]);
        } else {
            raw_post(run, &run->ops[RAW_SERVER_SEND]);
        }
    } else if (op->kind == RAW_CLIENT_RECV &&
               memcmp(op->data, run->ops[RAW_CLIENT_SEND].data, RAW_BYTES) != 0) {
        raw_fail("client echo mismatch", ERROR_INVALID_DATA);
    }
    if (run->done[RAW_CLIENT_SEND] && run->done[RAW_SERVER_RECV] &&
        run->done[RAW_SERVER_SEND] && run->done[RAW_CLIENT_RECV]) {
        if (run->outstanding != 0U) {
            raw_fail("completion accounting", ERROR_INVALID_DATA);
        }
        run->exchange += 1U;
        if (run->exchange < RAW_EXCHANGES) {
            raw_start_exchange(run);
        }
    }
}

static raw_result_t raw_run_once(bool reads_only) {
    raw_run_t run;
    OVERLAPPED_ENTRY entries[RAW_BATCH];
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER dequeue_start;
    LARGE_INTEGER dequeue_end;
    uint64_t cpu_start;
    unsigned consecutive_timeouts = 0U;

    memset(&run, 0, sizeof(run));
    run.reads_only = reads_only;
    run.client = INVALID_SOCKET;
    run.server = INVALID_SOCKET;
    raw_make_pair(&run.client, &run.server);
    run.port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0U, 15U);
    if (run.port == NULL ||
        CreateIoCompletionPort((HANDLE)(uintptr_t)run.client, run.port, 1U,
                               0U) != run.port ||
        CreateIoCompletionPort((HANDLE)(uintptr_t)run.server, run.port, 2U,
                               0U) != run.port ||
        !QueryPerformanceFrequency(&frequency)) {
        raw_fail("IOCP association/QPF", GetLastError());
    }
    run.ops[RAW_CLIENT_SEND].socket = run.client;
    run.ops[RAW_SERVER_RECV].socket = run.server;
    run.ops[RAW_SERVER_SEND].socket = run.server;
    run.ops[RAW_CLIENT_RECV].socket = run.client;
    for (unsigned i = 0U; i < 4U; ++i) {
        run.ops[i].kind = (raw_op_kind_t)i;
    }
    cpu_start = raw_thread_cpu_100ns();
    QueryPerformanceCounter(&start);
    raw_start_exchange(&run);
    while (run.exchange < RAW_EXCHANGES) {
        ULONG count = 0U;
        DWORD error_code;

        QueryPerformanceCounter(&dequeue_start);
        if (!GetQueuedCompletionStatusEx(run.port, entries, RAW_BATCH, &count,
                                         1000U, FALSE)) {
            error_code = GetLastError();
            if (error_code == WAIT_TIMEOUT && ++consecutive_timeouts < 4U) {
                run.result.timeouts += 1U;
                continue;
            }
            raw_fail("GetQueuedCompletionStatusEx", error_code);
        }
        QueryPerformanceCounter(&dequeue_end);
        run.result.dequeue_ns +=
            raw_qpc_ns(dequeue_end, frequency) -
            raw_qpc_ns(dequeue_start, frequency);
        run.result.dequeue_calls += 1U;
        if (count == RAW_BATCH) {
            run.result.full_batches += 1U;
        }
        consecutive_timeouts = 0U;
        for (ULONG i = 0U; i < count; ++i) {
            raw_complete(&run, &entries[i]);
        }
    }
    QueryPerformanceCounter(&end);
    run.result.elapsed_ns = raw_qpc_ns(end, frequency) -
                            raw_qpc_ns(start, frequency);
    run.result.thread_cpu_100ns = raw_thread_cpu_100ns() - cpu_start;
    if (run.outstanding != 0U ||
        run.result.submitted != run.result.completions) {
        raw_fail("final accounting", ERROR_INVALID_DATA);
    }
    closesocket(run.client);
    closesocket(run.server);
    CloseHandle(run.port);
    return run.result;
}

int main(void) {
    WSADATA wsa;
    const char *reads_only_env = getenv("LLAM_RAW_IOCP_READS_ONLY");
    bool reads_only = reads_only_env != NULL &&
                      strcmp(reads_only_env, "1") == 0;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
        raw_fail("SetPriorityClass", GetLastError());
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        raw_fail("WSAStartup", WSAGetLastError());
    }
    printf("raw-iocp: 1 KiB loopback records, one credit, batch=64, "
           "port concurrency=15, skip-on-success=off, io_mix=%s\n",
           reads_only ? "overlapped-read/direct-send" : "fully-overlapped");
    for (unsigned round = 0U; round < RAW_ROUNDS; ++round) {
        raw_result_t result = raw_run_once(reads_only);
        double ops_per_sec = result.elapsed_ns != 0U
                                 ? (double)RAW_EXCHANGES * 1000000000.0 /
                                       (double)result.elapsed_ns
                                 : 0.0;

        printf("raw-iocp: round=%u warmup=%u records=%u elapsed_ns=%llu "
               "ops_per_sec=%.2f thread_cpu_us=%.1f dequeue_calls=%llu "
               "completions=%llu entries_per_call=%.3f full_batches=%llu "
               "dequeue_wait_ns=%llu immediate=%llu pending=%llu "
               "submissions=%llu bytes=%llu direct_sends=%llu "
               "direct_bytes=%llu timeouts=%llu\n",
               round, round == 0U, RAW_EXCHANGES,
               (unsigned long long)result.elapsed_ns,
               ops_per_sec, (double)result.thread_cpu_100ns / 10.0,
               (unsigned long long)result.dequeue_calls,
               (unsigned long long)result.completions,
               result.dequeue_calls != 0U
                   ? (double)result.completions / (double)result.dequeue_calls
                   : 0.0,
               (unsigned long long)result.full_batches,
               (unsigned long long)result.dequeue_ns,
               (unsigned long long)result.immediate,
               (unsigned long long)result.pending,
               (unsigned long long)result.submitted,
               (unsigned long long)result.bytes,
               (unsigned long long)result.direct_sends,
               (unsigned long long)result.direct_bytes,
               (unsigned long long)result.timeouts);
    }
    WSACleanup();
    return 0;
}
