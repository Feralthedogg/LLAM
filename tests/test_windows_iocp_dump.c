/**
 * @file tests/test_windows_iocp_dump.c
 * @brief Windows IOCP pending-request runtime dump test.
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

#include "llam/runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
int main(void) {
    puts("[test_windows_iocp_dump] skipped");
    return 0;
}
#else

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

typedef struct windows_iocp_dump_state {
    llam_fd_t listener;
    struct sockaddr_in addr;
    atomic_uint accept_entered;
    atomic_uint dump_seen;
    atomic_uint accepted_done;
    atomic_uint failures;
    int first_errno;
    char first_case[128];
} windows_iocp_dump_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_windows_iocp_dump] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(windows_iocp_dump_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int winerr_to_errno(DWORD error_code) {
    switch (error_code) {
    case WSAECONNRESET:
    case WSAECONNABORTED:
        return ECONNRESET;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINVAL:
        return EINVAL;
    default:
        return EIO;
    }
}

static llam_fd_t create_overlapped_tcp_socket(void) {
    SOCKET socket_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

    if (socket_fd == INVALID_SOCKET) {
        errno = winerr_to_errno(WSAGetLastError());
        return LLAM_INVALID_FD;
    }
    return (llam_fd_t)socket_fd;
}

static int setup_listener(windows_iocp_dump_state_t *state) {
    llam_fd_t listener;
    struct sockaddr_in addr;
    int opt = 1;
    int addr_len = (int)sizeof(addr);

    listener = create_overlapped_tcp_socket();
    if (LLAM_FD_IS_INVALID(listener)) {
        return -1;
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, (int)sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (const struct sockaddr *)&addr, (int)sizeof(addr)) != 0 ||
        listen(listener, SOMAXCONN) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        errno = winerr_to_errno(WSAGetLastError());
        closesocket(listener);
        return -1;
    }
    state->listener = listener;
    state->addr = addr;
    return 0;
}

static char *capture_runtime_dump(void) {
    char temp_dir[MAX_PATH];
    char temp_path[MAX_PATH];
    char *buffer;
    __int64 size;
    int fd;
    int nread;

    if (GetTempPathA((DWORD)sizeof(temp_dir), temp_dir) == 0 ||
        GetTempFileNameA(temp_dir, "lid", 0, temp_path) == 0) {
        return NULL;
    }
    fd = _open(temp_path, _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        DeleteFileA(temp_path);
        return NULL;
    }

    llam_dump_runtime_state(fd);
    size = _lseeki64(fd, 0, SEEK_END);
    if (size < 0 || _lseeki64(fd, 0, SEEK_SET) < 0) {
        _close(fd);
        DeleteFileA(temp_path);
        return NULL;
    }
    buffer = calloc((size_t)size + 1U, 1U);
    if (buffer == NULL) {
        _close(fd);
        DeleteFileA(temp_path);
        return NULL;
    }
    nread = _read(fd, buffer, (unsigned)size);
    _close(fd);
    DeleteFileA(temp_path);
    if (nread < 0) {
        free(buffer);
        return NULL;
    }
    buffer[nread] = '\0';
    return buffer;
}

static bool dump_contains_pending_accept(const char *dump, llam_fd_t listener) {
    char fd_pattern[64];

    (void)snprintf(fd_pattern, sizeof(fd_pattern), "fd=%llu", (unsigned long long)listener);
    return strstr(dump, "wait_owner=io_req") != NULL &&
           strstr(dump, "kind=accept") != NULL &&
           strstr(dump, fd_pattern) != NULL &&
           strstr(dump, "wait_mode=") != NULL &&
           strstr(dump, "active_io_waiters=") != NULL;
}

static void accept_task(void *arg) {
    windows_iocp_dump_state_t *state = arg;
    llam_fd_t accepted;

    atomic_store_explicit(&state->accept_entered, 1U, memory_order_release);
    accepted = llam_accept(state->listener, NULL, NULL);
    if (LLAM_FD_IS_INVALID(accepted)) {
        task_fail(state, "llam_accept pending dump release failed", errno);
        return;
    }
    closesocket(accepted);
    atomic_fetch_add_explicit(&state->accepted_done, 1U, memory_order_relaxed);
}

static int connect_once(const windows_iocp_dump_state_t *state) {
    llam_fd_t client = create_overlapped_tcp_socket();
    int rc;

    if (LLAM_FD_IS_INVALID(client)) {
        return -1;
    }
    rc = connect(client, (const struct sockaddr *)&state->addr, (int)sizeof(state->addr));
    if (rc != 0) {
        errno = winerr_to_errno(WSAGetLastError());
        closesocket(client);
        return -1;
    }
    closesocket(client);
    return 0;
}

static DWORD WINAPI dump_thread_main(LPVOID arg) {
    windows_iocp_dump_state_t *state = arg;
    unsigned i;

    for (i = 0U; i < 200U; ++i) {
        if (atomic_load_explicit(&state->accept_entered, memory_order_acquire) != 0U) {
            break;
        }
        Sleep(1U);
    }

    for (i = 0U; i < 250U; ++i) {
        char *dump = capture_runtime_dump();

        if (dump != NULL) {
            if (dump_contains_pending_accept(dump, state->listener)) {
                atomic_store_explicit(&state->dump_seen, 1U, memory_order_release);
                free(dump);
                break;
            }
            free(dump);
        }
        Sleep(5U);
    }

    if (atomic_load_explicit(&state->dump_seen, memory_order_acquire) == 0U) {
        task_fail(state, "runtime dump did not expose pending IOCP accept");
    }
    if (connect_once(state) != 0) {
        task_fail(state, "failed to release pending accept", errno);
    }
    return 0;
}

int main(void) {
    windows_iocp_dump_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *acceptor;
    HANDLE dump_thread = NULL;
    int failed = 0;

    memset(&state, 0, sizeof(state));
    state.listener = LLAM_INVALID_FD;
    atomic_init(&state.accept_entered, 0U);
    atomic_init(&state.dump_seen, 0U);
    atomic_init(&state.accepted_done, 0U);
    atomic_init(&state.failures, 0U);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_opts_init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_init_ex failed");
    }
    if (setup_listener(&state) != 0) {
        failed = fail_errno("listener setup failed");
        goto shutdown;
    }

    acceptor = llam_spawn(accept_task, &state, NULL);
    if (acceptor == NULL) {
        failed = fail_errno("accept task spawn failed");
        goto shutdown;
    }
    dump_thread = CreateThread(NULL, 0, dump_thread_main, &state, 0, NULL);
    if (dump_thread == NULL) {
        failed = fail_errno("dump thread create failed");
        (void)connect_once(&state);
    }
    if (llam_run() != 0) {
        failed = fail_errno("llam_run failed");
    }
    if (llam_join(acceptor) != 0) {
        failed = fail_errno("accept task join failed");
    }
    if (dump_thread != NULL) {
        (void)WaitForSingleObject(dump_thread, INFINITE);
        CloseHandle(dump_thread);
    }

shutdown:
    if (!LLAM_FD_IS_INVALID(state.listener)) {
        closesocket(state.listener);
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_windows_iocp_dump] task failed at %s errno=%d\n",
                state.first_case,
                state.first_errno);
        failed = 1;
    }
    if (failed == 0 &&
        (atomic_load_explicit(&state.dump_seen, memory_order_relaxed) != 1U ||
         atomic_load_explicit(&state.accepted_done, memory_order_relaxed) != 1U)) {
        fprintf(stderr, "[test_windows_iocp_dump] missing dump or accept completion\n");
        failed = 1;
    }
    if (failed == 0) {
        puts("[test_windows_iocp_dump] ok");
    }
    return failed;
}
#endif
