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

#include "runtime_internal.h"
#include "llam/io.h"
#include "cavo_lifetime_internal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "I/O loan:%d: %s errno=%d\n", __LINE__, #expr, errno); return false; } } while (0)
#define IO_LOAN_BYTES 4096U
#define IO_REUSED_BYTE 0x7eU

typedef struct io_loan_case {
    llam_runtime_t *runtime;
    llam_cancel_token_t *token;
    llam_task_t *operation;
#if LLAM_PLATFORM_WINDOWS
    HANDLE reader, writer;
#else
    int reader, writer;
#endif
    unsigned char *loan;
    atomic_uint returned, observer_done, failures;
    bool writing, close_request, target_closed, helper;
    ssize_t result;
    int error;
    size_t filled;
} io_loan_case_t;

static void io_check(io_loan_case_t *test, bool passed, const char *where) {
    if (!passed && atomic_fetch_add(&test->failures, 1U) == 0U)
        fprintf(stderr, "I/O loan %s/%s: %s errno=%d\n", test->writing ? "write" : "read",
                test->close_request ? "close" : "cancel", where, errno);
}

#if LLAM_PLATFORM_WINDOWS
static bool io_pipe_open(io_loan_case_t *test) {
    static unsigned serial;
    char name[128];
    (void)snprintf(name, sizeof(name), "\\\\.\\pipe\\llam-loan-%lu-%u", (unsigned long)GetCurrentProcessId(), ++serial);
    test->reader = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, NULL);
    CHECK(test->reader != INVALID_HANDLE_VALUE);
    test->writer = CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    CHECK(test->writer != INVALID_HANDLE_VALUE);
    CHECK(ConnectNamedPipe(test->reader, NULL) || GetLastError() == ERROR_PIPE_CONNECTED);
    return true;
}

static ssize_t native_transfer(HANDLE handle, bool writing, unsigned char *buffer, size_t size) {
    OVERLAPPED operation = {0};
    operation.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (operation.hEvent == NULL) return -1;
    DWORD bytes = 0U;
    BOOL done = writing ? WriteFile(handle, buffer, (DWORD)size, &bytes, &operation)
                        : ReadFile(handle, buffer, (DWORD)size, &bytes, &operation);
    if (!done && GetLastError() == ERROR_IO_PENDING)
        done = GetOverlappedResult(handle, &operation, &bytes, TRUE);
    CloseHandle(operation.hEvent);
    if (!done) { errno = EPIPE; return -1; }
    return (ssize_t)bytes;
}

static ssize_t io_native_write(io_loan_case_t *test, unsigned char *buffer, size_t size) {
    return native_transfer(test->writer, true, buffer, size);
}

static ssize_t io_native_read(io_loan_case_t *test, unsigned char *buffer, size_t size) {
    DWORD available = 0U;
    if (!PeekNamedPipe(test->reader, NULL, 0, NULL, &available, NULL)) { errno = EPIPE; return -1; }
    if (available == 0U) { errno = EAGAIN; return -1; }
    if (size > available) size = available;
    return native_transfer(test->reader, false, buffer, size);
}

static bool io_fill_pipe(io_loan_case_t *test) {
    DWORD capacity = 0U;
    CHECK(GetNamedPipeInfo(test->reader, NULL, NULL, &capacity, NULL));
    CHECK(capacity != 0U && capacity <= 1024U * 1024U);
    unsigned char filler[IO_LOAN_BYTES];
    memset(filler, 0x11, sizeof(filler));
    while (test->filled < capacity) {
        size_t count = capacity - test->filled;
        if (count > sizeof(filler)) count = sizeof(filler);
        ssize_t sent = io_native_write(test, filler, count);
        CHECK(sent > 0);
        test->filled += (size_t)sent;
    }
    return true;
}

static int io_close_end(io_loan_case_t *test, bool writer) {
    return llam_close_handle((llam_handle_t)(writer ? test->writer : test->reader));
}

static ssize_t io_borrow(io_loan_case_t *test) {
    return test->writing ? llam_write_handle((llam_handle_t)test->writer, test->loan, 1U)
                         : llam_read_handle((llam_handle_t)test->reader, test->loan, 1U);
}
#else
static bool io_pipe_open(io_loan_case_t *test) {
    int pair[2];
    CHECK(pipe(pair) == 0);
    test->reader = pair[0];
    test->writer = pair[1];
    CHECK(fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL) | O_NONBLOCK) == 0);
    CHECK(fcntl(pair[1], F_SETFL, fcntl(pair[1], F_GETFL) | O_NONBLOCK) == 0);
    return true;
}

static ssize_t io_native_write(io_loan_case_t *test, unsigned char *buffer, size_t size) {
    return write(test->writer, buffer, size);
}

static ssize_t io_native_read(io_loan_case_t *test, unsigned char *buffer, size_t size) {
    return read(test->reader, buffer, size);
}

static bool io_fill_pipe(io_loan_case_t *test) {
    unsigned char filler[IO_LOAN_BYTES];
    memset(filler, 0x11, sizeof(filler));
    while (test->filled <= 1024U * 1024U) {
        ssize_t sent = io_native_write(test, filler, sizeof(filler));
        if (sent < 0) { CHECK(errno == EAGAIN || errno == EWOULDBLOCK); return true; }
        CHECK(sent != 0);
        test->filled += (size_t)sent;
    }
    CHECK(false);
}

static int io_close_end(io_loan_case_t *test, bool writer) {
    return llam_close(writer ? test->writer : test->reader);
}

static ssize_t io_borrow(io_loan_case_t *test) {
    return test->writing ? llam_write(test->writer, test->loan, 1U)
                         : llam_read(test->reader, test->loan, 1U);
}
#endif

static bool io_case_create(io_loan_case_t *test, bool writing, bool closing, bool helper) {
    memset(test, 0, sizeof(*test));
    atomic_init(&test->returned, 0U);
    atomic_init(&test->observer_done, 0U);
    atomic_init(&test->failures, 0U);
    test->writing = writing;
    test->close_request = closing;
    test->helper = helper;
    test->loan = malloc(IO_LOAN_BYTES);
    CHECK(test->loan != NULL);
    memset(test->loan, 0x55, IO_LOAN_BYTES);
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    options.worker_count = 1U;
    CHECK(llam_runtime_create_compat(&options, sizeof(options), &test->runtime) == 0);
    /* Before any task/request is published, simulate a backend without byte
     * operations. This exercises the real public fallback, not the socket-only
     * direct-blocking optimization. */
    if (helper) {
        for (unsigned i = 0U; i < test->runtime->active_nodes; ++i) {
            test->runtime->nodes[i].supports_read = false;
            test->runtime->nodes[i].supports_write = false;
        }
    }
    test->token = llam_runtime_cancel_token_create(test->runtime);
    CHECK(test->token != NULL && io_pipe_open(test));
    if (writing) CHECK(io_fill_pipe(test));
    return true;
}

static void io_operation(void *data) {
    io_loan_case_t *test = data;
    test->result = io_borrow(test);
    test->error = errno;
    /* The caller has its loan back. Reuse the same allocation immediately;
     * a late read corrupts this marker and a late write transmits it. */
    memset(test->loan, IO_REUSED_BYTE, IO_LOAN_BYTES);
    atomic_store_explicit(&test->returned, 1U, memory_order_release);
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (!atomic_load_explicit(&test->observer_done, memory_order_acquire) && llam_now_ns() < limit)
        llam_yield();
    io_check(test, atomic_load(&test->observer_done) != 0U, "observer completed");
}

static bool io_operation_waiting(io_loan_case_t *test) {
    llam_task_t *task = llam_task_resolve_public_handle(test->operation);
    if (task == NULL) return false;
    unsigned reason = atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    bool waiting = false;
    if (reason == LLAM_WAIT_IO && !test->helper) {
        llam_io_req_t *request = atomic_load_explicit(&task->active_io_req, memory_order_acquire);
        waiting = request != NULL && atomic_load_explicit(&request->wait_mode, memory_order_acquire) == LLAM_IO_WAIT_MODE_INFLIGHT;
    } else if (reason == LLAM_WAIT_BLOCKING) {
        llam_block_job_t *job = llam_task_active_block_job_load(task);
        waiting = job != NULL && atomic_load_explicit(&job->state, memory_order_acquire) == LLAM_BLOCK_JOB_RUNNING;
#if LLAM_PLATFORM_LINUX || LLAM_PLATFORM_WINDOWS
        if (!test->helper) waiting = false;
#endif
    }
    llam_task_end_public_op(task);
    return waiting;
}

/* Peer handles have never been submitted to LLAM. Any synchronous native wait
 * stays on the blocking pool rather than blocking a scheduler worker. */
static void *io_probe_peer(void *data) {
    io_loan_case_t *test = data;
    unsigned char bytes[IO_LOAN_BYTES];
    if (!test->writing) {
        bytes[0] = 0x33U;
        ssize_t sent = io_native_write(test, bytes, 1U);
        io_check(test, sent == 1 || (test->close_request && sent < 0), "late peer write");
        return NULL;
    }
    for (;;) {
        ssize_t received = io_native_read(test, bytes, sizeof(bytes));
        if (received <= 0) break;
        for (ssize_t i = 0; i < received; ++i)
            io_check(test, bytes[i] != IO_REUSED_BYTE, "backend read a returned write buffer");
    }
    return NULL;
}

static bool io_wait_for_phase(io_loan_case_t *test, bool completion) {
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    while (llam_now_ns() < limit) {
        if (completion ? atomic_load_explicit(&test->returned, memory_order_acquire) != 0U
                       : io_operation_waiting(test)) return true;
        if (llam_sleep_ns(UINT64_C(100000)) != 0) break;
    }
    io_check(test, false, completion ? "I/O return timeout" : "no actual pending operation");
    return false;
}

static void io_controller(void *data) {
    io_loan_case_t *test = data;
    bool pending = io_wait_for_phase(test, false);
    if (test->close_request) {
        io_check(test, io_close_end(test, test->writing) == 0, "close pending operation");
        test->target_closed = true;
    } else io_check(test, llam_cancel_token_cancel(test->token) == 0, "request cancellation");
    if (pending && io_wait_for_phase(test, true)) {
        io_check(test, test->result == 1 || (!test->writing && test->close_request && test->result == 0) ||
                       (test->result == -1 && test->error != 0), "terminal result");
        void *unused;
        for (unsigned i = 0U; i != 4U; ++i) {
            io_check(test, llam_call_blocking_result(io_probe_peer, test, &unused) == 0, "probe late access");
            io_check(test, llam_sleep_ns(UINT64_C(5000000)) == 0, "allow backend progress");
            for (size_t j = 0U; j < IO_LOAN_BYTES; ++j)
                io_check(test, test->loan[j] == IO_REUSED_BYTE, "backend wrote a returned read buffer");
        }
    }
    atomic_store(&test->observer_done, 1U);
    if (!pending || !atomic_load(&test->returned)) (void)llam_runtime_request_stop_handle(test->runtime);
}

static bool io_run_case(bool writing, bool closing, bool helper) {
    io_loan_case_t test;
    CHECK(io_case_create(&test, writing, closing, helper));
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.cancel_token = test.token;
    test.operation = llam_runtime_spawn_ex(test.runtime, io_operation, &test, &options, sizeof(options));
    llam_task_t *controller = llam_runtime_spawn_ex(test.runtime, io_controller, &test, NULL, 0U);
    CHECK(test.operation != NULL && controller != NULL);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(test.operation) == 0 && llam_join(controller) == 0);
    CHECK(atomic_load(&test.failures) == 0U);
    if (!test.target_closed || test.writing) CHECK(io_close_end(&test, false) == 0);
    if (!test.target_closed || !test.writing) CHECK(io_close_end(&test, true) == 0);
    CHECK(llam_cancel_token_destroy(test.token) == 0);
    llam_runtime_destroy(test.runtime);
    free(test.loan);
    return true;
}

bool cavo_io_lifetime_cases(bool force_helper) {
#if LLAM_PLATFORM_WINDOWS
    CHECK(!force_helper);
    CHECK(_putenv_s("LLAM_DIRECT_BLOCKING_IO", "0") == 0);
#else
    CHECK(setenv("LLAM_DIRECT_BLOCKING_IO", "0", 1) == 0);
    void (*previous_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    CHECK(previous_sigpipe != SIG_ERR);
#endif
    for (unsigned writing = 0U; writing != 2U; ++writing) {
        for (unsigned closing = 0U; closing != 2U; ++closing) {
            fprintf(stderr, "I/O loan case: %s %s %s\n", writing ? "write" : "read",
                    closing ? "close" : "cancel", force_helper ? "helper" : "backend");
            if (!io_run_case(writing != 0U, closing != 0U, force_helper)) return false;
        }
    }
#if !LLAM_PLATFORM_WINDOWS
    CHECK(signal(SIGPIPE, previous_sigpipe) != SIG_ERR);
#endif
    puts("Cavo I/O loans: pending read/write cancellation and close preserve terminal buffer access");
    return true;
}
