/**
 * @file tests/test_windows_handle_io.c
 * @brief Native Windows HANDLE I/O smoke test.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "llam/runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
int main(void) {
    puts("[test_windows_handle_io] skipped");
    return 0;
}
#else

#include <windows.h>

typedef struct windows_handle_state {
    llam_handle_t pipe_reader;
    llam_handle_t pipe_writer;
    llam_handle_t event_handle;
    atomic_uint failures;
    atomic_uint reader_done;
    atomic_uint writer_done;
    atomic_uint poller_done;
    atomic_uint signaler_done;
    int first_errno;
    char first_case[96];
} windows_handle_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_windows_handle_io] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int winerr_to_errno(DWORD error_code) {
    switch (error_code) {
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_OPERATION_ABORTED:
        return ECANCELED;
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
        return EPIPE;
    default:
        return EIO;
    }
}

static void task_fail(windows_handle_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int setup_pipe(windows_handle_state_t *state) {
    char name[128];
    HANDLE reader;
    HANDLE writer;

    (void)snprintf(name, sizeof(name), "\\\\.\\pipe\\llam-handle-%lu", (unsigned long)GetCurrentProcessId());
    reader = CreateNamedPipeA(name,
                              PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1,
                              4096,
                              4096,
                              0,
                              NULL);
    if (reader == INVALID_HANDLE_VALUE) {
        errno = winerr_to_errno(GetLastError());
        return -1;
    }
    writer = CreateFileA(name,
                         GENERIC_WRITE,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                         NULL);
    if (writer == INVALID_HANDLE_VALUE) {
        DWORD error_code = GetLastError();

        CloseHandle(reader);
        errno = winerr_to_errno(error_code);
        return -1;
    }
    if (!ConnectNamedPipe(reader, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        DWORD error_code = GetLastError();

        CloseHandle(writer);
        CloseHandle(reader);
        errno = winerr_to_errno(error_code);
        return -1;
    }
    state->pipe_reader = (llam_handle_t)reader;
    state->pipe_writer = (llam_handle_t)writer;
    return 0;
}

static void reader_task(void *arg) {
    windows_handle_state_t *state = arg;
    char buf[4];

    if (llam_read_handle(state->pipe_reader, buf, sizeof(buf)) != (ssize_t)sizeof(buf) ||
        memcmp(buf, "ping", sizeof(buf)) != 0) {
        task_fail(state, "llam_read_handle named pipe", errno != 0 ? errno : EIO);
        return;
    }
    atomic_fetch_add_explicit(&state->reader_done, 1U, memory_order_relaxed);
}

static void writer_task(void *arg) {
    windows_handle_state_t *state = arg;

    if (llam_write_handle(state->pipe_writer, "ping", 4U) != 4) {
        task_fail(state, "llam_write_handle named pipe", errno != 0 ? errno : EIO);
        return;
    }
    atomic_fetch_add_explicit(&state->writer_done, 1U, memory_order_relaxed);
}

static void poller_task(void *arg) {
    windows_handle_state_t *state = arg;
    short revents = 0;

    if (llam_poll_handle(state->event_handle, POLLIN, 5000, &revents) != 1 || (revents & POLLIN) == 0) {
        task_fail(state, "llam_poll_handle event", errno != 0 ? errno : EIO);
        return;
    }
    atomic_fetch_add_explicit(&state->poller_done, 1U, memory_order_relaxed);
}

static void signaler_task(void *arg) {
    windows_handle_state_t *state = arg;

    /*
     * Keep the signal later than the 10 ms cancellation slice used by the
     * blocking HANDLE fallback.  This catches regressions where a finite
     * llam_poll_handle timeout reports the first slice timeout as the whole
     * caller timeout.
     */
    if (llam_sleep_ns(100000000ULL) != 0) {
        task_fail(state, "llam_sleep_ns before SetEvent", errno != 0 ? errno : EIO);
        return;
    }
    if (!SetEvent((HANDLE)state->event_handle)) {
        task_fail(state, "SetEvent", winerr_to_errno(GetLastError()));
        return;
    }
    atomic_fetch_add_explicit(&state->signaler_done, 1U, memory_order_relaxed);
}

int main(void) {
    windows_handle_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *reader;
    llam_task_t *writer;
    llam_task_t *poller;
    llam_task_t *signaler;
    int failed = 0;

    memset(&state, 0, sizeof(state));
    state.pipe_reader = LLAM_INVALID_HANDLE;
    state.pipe_writer = LLAM_INVALID_HANDLE;
    state.event_handle = LLAM_INVALID_HANDLE;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.reader_done, 0U);
    atomic_init(&state.writer_done, 0U);
    atomic_init(&state.poller_done, 0U);
    atomic_init(&state.signaler_done, 0U);

    if (setup_pipe(&state) != 0) {
        return fail_errno("setup_pipe failed");
    }
    state.event_handle = (llam_handle_t)CreateEventA(NULL, TRUE, FALSE, NULL);
    if (LLAM_HANDLE_IS_INVALID(state.event_handle)) {
        failed = fail_errno("CreateEventA failed");
        goto cleanup;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        failed = fail_errno("llam_runtime_opts_init failed");
        goto cleanup;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        failed = fail_errno("llam_runtime_init_ex failed");
        goto cleanup;
    }

    reader = llam_spawn(reader_task, &state, NULL);
    writer = llam_spawn(writer_task, &state, NULL);
    poller = llam_spawn(poller_task, &state, NULL);
    signaler = llam_spawn(signaler_task, &state, NULL);
    if (reader == NULL || writer == NULL || poller == NULL || signaler == NULL) {
        failed = fail_errno("llam_spawn failed");
        goto shutdown;
    }
    if (llam_run() != 0) {
        failed = fail_errno("llam_run failed");
        goto shutdown;
    }
    if (llam_join(reader) != 0 || llam_join(writer) != 0 ||
        llam_join(poller) != 0 || llam_join(signaler) != 0) {
        failed = fail_errno("llam_join failed");
        goto shutdown;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_windows_handle_io] task failed at %s errno=%d\n",
                state.first_case,
                state.first_errno);
        failed = 1;
    } else if (atomic_load_explicit(&state.reader_done, memory_order_relaxed) != 1U ||
               atomic_load_explicit(&state.writer_done, memory_order_relaxed) != 1U ||
               atomic_load_explicit(&state.poller_done, memory_order_relaxed) != 1U ||
               atomic_load_explicit(&state.signaler_done, memory_order_relaxed) != 1U) {
        fprintf(stderr, "[test_windows_handle_io] missing completion\n");
        failed = 1;
    }

shutdown:
    llam_runtime_shutdown();
cleanup:
    if (!LLAM_HANDLE_IS_INVALID(state.event_handle)) {
        CloseHandle((HANDLE)state.event_handle);
    }
    if (!LLAM_HANDLE_IS_INVALID(state.pipe_writer)) {
        CloseHandle((HANDLE)state.pipe_writer);
    }
    if (!LLAM_HANDLE_IS_INVALID(state.pipe_reader)) {
        CloseHandle((HANDLE)state.pipe_reader);
    }
    if (failed == 0) {
        puts("[test_windows_handle_io] ok");
    }
    return failed;
}
#endif
