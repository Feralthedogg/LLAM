/**
 * @file tests/test_windows_iocp_io.c
 * @brief Native Windows IOCP socket round-trip test.
 *
 * @details
 * This test exercises the Windows request backend end to end:
 *  - server task parks in llam_accept(), completed by AcceptEx;
 *  - client task parks in llam_connect(), completed by ConnectEx;
 *  - test helpers submit reads/writes and supported polls directly through the
 *    backend issue path so WSARecv, WSASend, TCP POLLOUT, and UDP MSG_PEEK
 *    readiness are exercised even when direct socket syscalls would complete
 *    immediately.
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

#include "../src/io/runtime_io_api_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
int main(void) {
    puts("[test_windows_iocp_io] skipped");
    return 0;
}
#else

#include "../src/io/windows/runtime_io_watch_windows_internal.h"

#define LLAM_WINDOWS_ASSOC_RACE_TASKS 4U
#define LLAM_WINDOWS_ASSOC_RACE_ROUNDS 64U
#define LLAM_WINDOWS_SOCKET_REUSE_LIMIT 65536U
#define LLAM_WINDOWS_SOCKET_REUSE_TIMEOUT_MS 2000U

typedef struct windows_iocp_state {
    llam_fd_t listener;
    struct sockaddr_in addr;
    atomic_uint failures;
    atomic_uint server_done;
    atomic_uint client_done;
    atomic_uint assoc_done;
    int first_errno;
    char first_case[96];
} windows_iocp_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_windows_iocp_io] %s: errno=%d\n", message, errno);
    return 1;
}

static void test_note(const char *phase) {
    printf("[test_windows_iocp_io] %s\n", phase);
    fflush(stdout);
}

static int test_invalid_owned_fd_errors(void) {
    llam_io_buffer_t *owned = (llam_io_buffer_t *)(uintptr_t)1U;

    /*
     * Windows reports invalid socket probes through WSAGetLastError().  These
     * calls verify that LLAM normalizes the public invalid-fd sentinel to
     * EBADF before the owned-buffer path can allocate or downgrade the error
     * to ENOTSOCK.
     */
    errno = 0;
    if (llam_recv_owned(LLAM_INVALID_FD, 1U, 0, &owned) != -1 ||
        errno != EBADF ||
        owned != NULL) {
        return fail_errno("llam_recv_owned invalid fd did not preserve EBADF");
    }

    owned = (llam_io_buffer_t *)(uintptr_t)1U;
    errno = 0;
    if (llam_read_owned(LLAM_INVALID_FD, 1U, &owned) != -1 ||
        errno != EBADF ||
        owned != NULL) {
        return fail_errno("llam_read_owned invalid fd did not preserve EBADF");
    }
    return 0;
}

static void task_fail(windows_iocp_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void maybe_stop_after_all_tasks(windows_iocp_state_t *state) {
    if (atomic_load_explicit(&state->server_done, memory_order_acquire) == 1U &&
        atomic_load_explicit(&state->client_done, memory_order_acquire) == 1U &&
        atomic_load_explicit(&state->assoc_done, memory_order_acquire) == LLAM_WINDOWS_ASSOC_RACE_TASKS) {
        /*
         * The IOCP smoke validates request completion, not the idle-run drain
         * heuristic.  Request a cooperative stop once every joinable test task
         * has published completion so hosted runners do not wait on a dormant
         * completion port after the useful work is already done.
         */
        (void)llam_runtime_request_stop();
    }
}

static llam_fd_t create_overlapped_tcp_socket(void) {
    SOCKET socket_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

    if (socket_fd == INVALID_SOCKET) {
        errno = WSAGetLastError();
        return LLAM_INVALID_FD;
    }
    return (llam_fd_t)socket_fd;
}

static llam_fd_t create_overlapped_udp_socket(void) {
    SOCKET socket_fd = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);

    if (socket_fd == INVALID_SOCKET) {
        errno = WSAGetLastError();
        return LLAM_INVALID_FD;
    }
    return (llam_fd_t)socket_fd;
}

static atomic_uint g_socket_authority_create_calls;
static DWORD g_socket_authority_create_flags;

static SOCKET capture_socket_authority_create(
    int address_family,
    int socket_type,
    int protocol,
    LPWSAPROTOCOL_INFOW protocol_info,
    GROUP group,
    DWORD flags) {
    atomic_fetch_add_explicit(
        &g_socket_authority_create_calls,
        1U,
        memory_order_relaxed);
    g_socket_authority_create_flags = flags;
    return WSASocketW(
        address_family,
        socket_type,
        protocol,
        protocol_info,
        group,
        flags);
}

static SOCKET reacquire_same_udp_socket_value(SOCKET wanted,
                                              unsigned *out_attempts) {
    SOCKET *held;
    SOCKET result = INVALID_SOCKET;
    unsigned held_count = 0U;
    unsigned attempts;

    if (out_attempts != NULL) {
        *out_attempts = 0U;
    }
    held = calloc(LLAM_WINDOWS_SOCKET_REUSE_LIMIT, sizeof(*held));
    if (held == NULL) {
        errno = ENOMEM;
        return INVALID_SOCKET;
    }
    for (attempts = 1U;
         attempts <= LLAM_WINDOWS_SOCKET_REUSE_LIMIT;
         ++attempts) {
        SOCKET current =
            (SOCKET)create_overlapped_udp_socket();

        if (current == INVALID_SOCKET) {
            break;
        }
        if (current == wanted) {
            result = current;
            break;
        }
        held[held_count++] = current;
    }
    for (unsigned i = 0U; i < held_count; ++i) {
        (void)closesocket(held[i]);
    }
    free(held);
    if (out_attempts != NULL) {
        *out_attempts =
            attempts <= LLAM_WINDOWS_SOCKET_REUSE_LIMIT
                ? attempts
                : LLAM_WINDOWS_SOCKET_REUSE_LIMIT;
    }
    return result;
}

static int bind_loopback_udp(SOCKET receiver,
                             struct sockaddr_in *out_address) {
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    if (receiver == INVALID_SOCKET || out_address == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(receiver,
             (const struct sockaddr *)&address,
             (int)sizeof(address)) != 0 ||
        getsockname(receiver,
                    (struct sockaddr *)&address,
                    &address_length) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    *out_address = address;
    return 0;
}

static int wait_for_native_udp_completion(SOCKET receiver,
                                          OVERLAPPED *overlapped) {
    DWORD bytes = 0U;
    DWORD flags = 0U;

    for (unsigned i = 0U; i < 200U; ++i) {
        if (WSAGetOverlappedResult(
                receiver, overlapped, &bytes, FALSE, &flags)) {
            return bytes == 1U ? 0 : -1;
        }
        if (WSAGetLastError() != WSA_IO_INCOMPLETE) {
            return -1;
        }
        Sleep(10U);
    }
    return -1;
}

static int init_isolated_iocp_node(llam_node_t *node) {
    int lock_rc;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(node, 0, sizeof(*node));
    node->runtime = llam_runtime_default_storage();
    lock_rc = pthread_mutex_init(&node->windows_assoc_lock, NULL);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    node->windows_assoc_lock_initialized = true;
    if (llam_windows_iocp_create(
            NULL, &node->windows_iocp_handle) != 0) {
        (void)pthread_mutex_destroy(&node->windows_assoc_lock);
        node->windows_assoc_lock_initialized = false;
        return -1;
    }
    return 0;
}

static void cleanup_isolated_iocp_node(llam_node_t *node) {
    llam_windows_fd_assoc_t *association;

    if (node == NULL) {
        return;
    }
    if (node->windows_assoc_lock_initialized) {
        (void)pthread_mutex_lock(&node->windows_assoc_lock);
    }
    association =
        (llam_windows_fd_assoc_t *)node->windows_fd_assoc_head;
    node->windows_fd_assoc_head = NULL;
    if (node->windows_assoc_lock_initialized) {
        (void)pthread_mutex_unlock(&node->windows_assoc_lock);
    }
    while (association != NULL) {
        llam_windows_fd_assoc_t *next = association->next;

        association->next = NULL;
        llam_windows_fd_assoc_destroy(association);
        association = next;
    }
    llam_windows_iocp_close(node->windows_iocp_handle);
    node->windows_iocp_handle = NULL;
    if (node->windows_assoc_lock_initialized) {
        (void)pthread_mutex_destroy(&node->windows_assoc_lock);
        node->windows_assoc_lock_initialized = false;
    }
}

static bool isolated_assoc_snapshot(
    llam_node_t *node,
    SOCKET socket_fd,
    uint64_t *generation_out,
    uintptr_t *authority_out) {
    llam_windows_fd_assoc_t *association;
    bool found = false;

    if (node == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&node->windows_assoc_lock);
    association =
        (llam_windows_fd_assoc_t *)node->windows_fd_assoc_head;
    while (association != NULL) {
        if ((uintptr_t)association->fd ==
            (uintptr_t)socket_fd) {
            if (generation_out != NULL) {
                *generation_out = association->generation;
            }
            if (authority_out != NULL) {
                *authority_out = association->authority;
            }
            found =
                association->is_socket &&
                association->owner_node == node &&
                !association->closing;
            break;
        }
        association = association->next;
    }
    (void)pthread_mutex_unlock(&node->windows_assoc_lock);
    return found;
}

static int test_socket_authority_is_non_inheritable(void) {
    llam_node_t node;
    SOCKET source = INVALID_SOCKET;
    uintptr_t authority = (uintptr_t)INVALID_SOCKET;
    DWORD source_flags = 0U;
    DWORD authority_flags = 0U;
    int failed = 1;
    bool node_initialized = false;

    if (init_isolated_iocp_node(&node) != 0) {
        return fail_errno("socket authority test IOCP node setup failed");
    }
    node_initialized = true;
    source = WSASocketW(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP,
        NULL,
        0U,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (source == INVALID_SOCKET ||
        !GetHandleInformation((HANDLE)(uintptr_t)source, &source_flags) ||
        (source_flags & HANDLE_FLAG_INHERIT) != 0U) {
        (void)fail_errno("non-inheritable source socket setup failed");
        goto cleanup;
    }

    atomic_store_explicit(
        &g_socket_authority_create_calls,
        0U,
        memory_order_relaxed);
    g_socket_authority_create_flags = 0U;
    llam_windows_test_set_socket_authority_create_hook(
        capture_socket_authority_create);
    if (llam_windows_associate_fd(&node, (llam_fd_t)source) != 0) {
        (void)fail_errno("socket authority association failed");
        goto cleanup;
    }
    llam_windows_test_set_socket_authority_create_hook(NULL);
    if (atomic_load_explicit(
            &g_socket_authority_create_calls,
            memory_order_relaxed) != 1U ||
        g_socket_authority_create_flags !=
            (WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)) {
        fprintf(stderr,
                "[test_windows_iocp_io] socket authority creation did not "
                "request atomic non-inheritance calls=%u flags=0x%08lx\n",
                atomic_load_explicit(
                    &g_socket_authority_create_calls,
                    memory_order_relaxed),
                (unsigned long)g_socket_authority_create_flags);
        goto cleanup;
    }
    if (!isolated_assoc_snapshot(&node, source, NULL, &authority) ||
        !GetHandleInformation((HANDLE)authority, &authority_flags) ||
        (authority_flags & HANDLE_FLAG_INHERIT) != 0U) {
        (void)fail_errno("retained socket authority is inheritable");
        goto cleanup;
    }
    failed = 0;

cleanup:
    llam_windows_test_set_socket_authority_create_hook(NULL);
    if (source != INVALID_SOCKET) {
        (void)closesocket(source);
    }
    if (node_initialized) {
        cleanup_isolated_iocp_node(&node);
    }
    return failed;
}

static int test_raw_socket_reuse_revalidates_iocp(void) {
    llam_windows_iocp_completion_t completion;
    llam_node_t node;
    SOCKET original = INVALID_SOCKET;
    SOCKET receiver = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    SOCKET wanted = INVALID_SOCKET;
    struct sockaddr_in address;
    struct sockaddr_storage from;
    OVERLAPPED overlapped;
    WSABUF buffer;
    DWORD flags = 0U;
    DWORD bytes = 0U;
    size_t count = 0U;
    unsigned attempts = 0U;
    uint64_t initial_generation = 0U;
    uint64_t replacement_generation = 0U;
    uintptr_t initial_authority = 0U;
    uintptr_t replacement_authority = 0U;
    int from_length = (int)sizeof(from);
    char byte = '\0';
    int failed = 1;
    bool operation_submitted = false;
    bool node_initialized = false;

    if (init_isolated_iocp_node(&node) != 0) {
        return fail_errno("socket reuse test IOCP node setup failed");
    }
    node_initialized = true;
    original = (SOCKET)create_overlapped_udp_socket();
    if (original == INVALID_SOCKET ||
        llam_windows_associate_fd(&node, (llam_fd_t)original) != 0) {
        (void)fail_errno("initial socket IOCP association failed");
        goto cleanup;
    }
    if (!isolated_assoc_snapshot(
            &node,
            original,
            &initial_generation,
            &initial_authority)) {
        (void)fail_errno(
            "initial socket association metadata missing");
        goto cleanup;
    }
    wanted = original;
    if (closesocket(original) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        (void)fail_errno("raw close before socket reuse failed");
        goto cleanup;
    }
    original = INVALID_SOCKET;
    receiver = reacquire_same_udp_socket_value(wanted, &attempts);
    if (receiver == INVALID_SOCKET) {
        printf("[test_windows_iocp_io] socket reuse unavailable attempts=%u; skipped\n",
               attempts);
        failed = 0;
        goto cleanup;
    }
    /*
     * The numeric cache still describes the closed object. This call must ask
     * the kernel about the replacement instead of treating the equal value as
     * proof of an existing association.
     */
    if (llam_windows_associate_fd(
            &node, (llam_fd_t)receiver) != 0 ||
        bind_loopback_udp(receiver, &address) != 0) {
        (void)fail_errno("replacement socket IOCP revalidation failed");
        goto cleanup;
    }
    if (!isolated_assoc_snapshot(
            &node,
            receiver,
            &replacement_generation,
            &replacement_authority) ||
        replacement_generation <= initial_generation ||
        replacement_authority == initial_authority) {
        fprintf(stderr,
                "[test_windows_iocp_io] replacement did not publish a new "
                "owned association generation\n");
        goto cleanup;
    }

    memset(&from, 0, sizeof(from));
    memset(&overlapped, 0, sizeof(overlapped));
    memset(&completion, 0, sizeof(completion));
    buffer.buf = &byte;
    buffer.len = 1U;
    if (WSARecvFrom(receiver,
                    &buffer,
                    1U,
                    &bytes,
                    &flags,
                    (struct sockaddr *)&from,
                    &from_length,
                    &overlapped,
                    NULL) != SOCKET_ERROR ||
        WSAGetLastError() != WSA_IO_PENDING) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        (void)fail_errno("replacement receive did not pend");
        goto cleanup;
    }
    operation_submitted = true;
    sender = (SOCKET)create_overlapped_udp_socket();
    if (sender == INVALID_SOCKET ||
        sendto(sender,
               "x",
               1,
               0,
               (const struct sockaddr *)&address,
               (int)sizeof(address)) != 1) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        (void)fail_errno("replacement receive trigger failed");
        goto cleanup;
    }
    if (llam_windows_iocp_drain(
            node.windows_iocp_handle,
            &completion,
            1U,
            LLAM_WINDOWS_SOCKET_REUSE_TIMEOUT_MS,
            &count) != 0 ||
        count != 1U ||
        completion.key != (uintptr_t)&node ||
        completion.overlapped != (uintptr_t)&overlapped ||
        completion.bytes != 1U) {
        fprintf(stderr,
                "[test_windows_iocp_io] replacement completed without source "
                "IOCP packet attempts=%u count=%zu key=%llu\n",
                attempts,
                count,
                (unsigned long long)completion.key);
        goto cleanup;
    }
    operation_submitted = false;
    failed = 0;

cleanup:
    if (operation_submitted && receiver != INVALID_SOCKET) {
        (void)CancelIoEx((HANDLE)(uintptr_t)receiver, &overlapped);
        (void)wait_for_native_udp_completion(receiver, &overlapped);
    }
    if (sender != INVALID_SOCKET) {
        (void)closesocket(sender);
    }
    if (receiver != INVALID_SOCKET) {
        (void)closesocket(receiver);
    }
    if (original != INVALID_SOCKET) {
        (void)closesocket(original);
    }
    if (node_initialized) {
        cleanup_isolated_iocp_node(&node);
    }
    return failed;
}

static int test_raw_socket_reuse_rejects_foreign_iocp(void) {
    llam_node_t node;
    void *foreign_port = NULL;
    SOCKET original = INVALID_SOCKET;
    SOCKET replacement = INVALID_SOCKET;
    SOCKET wanted = INVALID_SOCKET;
    unsigned attempts = 0U;
    int failed = 1;
    bool node_initialized = false;

    if (init_isolated_iocp_node(&node) != 0) {
        return fail_errno("foreign source IOCP setup failed");
    }
    node_initialized = true;
    if (llam_windows_iocp_create(NULL, &foreign_port) != 0) {
        cleanup_isolated_iocp_node(&node);
        return fail_errno("foreign IOCP setup failed");
    }
    original = (SOCKET)create_overlapped_udp_socket();
    if (original == INVALID_SOCKET ||
        llam_windows_associate_fd(&node, (llam_fd_t)original) != 0) {
        (void)fail_errno("foreign test initial association failed");
        goto cleanup;
    }
    wanted = original;
    if (closesocket(original) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        (void)fail_errno("foreign test raw close failed");
        goto cleanup;
    }
    original = INVALID_SOCKET;
    replacement =
        reacquire_same_udp_socket_value(wanted, &attempts);
    if (replacement == INVALID_SOCKET) {
        printf("[test_windows_iocp_io] foreign socket reuse unavailable "
               "attempts=%u; skipped\n",
               attempts);
        failed = 0;
        goto cleanup;
    }
    if (CreateIoCompletionPort((HANDLE)(uintptr_t)replacement,
                               (HANDLE)foreign_port,
                               (ULONG_PTR)UINT64_C(0x464f524549474e),
                               0U) == NULL) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        (void)fail_errno("foreign IOCP association failed");
        goto cleanup;
    }
    errno = 0;
    if (llam_windows_associate_fd(
            &node, (llam_fd_t)replacement) == 0) {
        fprintf(stderr,
                "[test_windows_iocp_io] stale numeric association accepted "
                "foreign replacement attempts=%u\n",
                attempts);
        goto cleanup;
    }
    if (isolated_assoc_snapshot(
            &node, replacement, NULL, NULL)) {
        fprintf(stderr,
                "[test_windows_iocp_io] rejected foreign replacement left "
                "stale source metadata\n");
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (replacement != INVALID_SOCKET) {
        (void)closesocket(replacement);
    }
    if (original != INVALID_SOCKET) {
        (void)closesocket(original);
    }
    llam_windows_iocp_close(foreign_port);
    if (node_initialized) {
        cleanup_isolated_iocp_node(&node);
    }
    return failed;
}

static int setup_listener(windows_iocp_state_t *state) {
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
    if (bind(listener, (const struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
        errno = WSAGetLastError();
        closesocket(listener);
        return -1;
    }
    if (listen(listener, SOMAXCONN) != 0) {
        errno = WSAGetLastError();
        closesocket(listener);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        errno = WSAGetLastError();
        closesocket(listener);
        return -1;
    }

    state->listener = listener;
    state->addr = addr;
    return 0;
}

static ssize_t issue_backend_read(llam_fd_t fd, void *buf, size_t count) {
    llam_io_req_t *req = llam_api_io_req_acquire(g_llam_tls_shard);
    ssize_t result;

    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }
    req->kind = LLAM_IO_KIND_READ;
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->recv_flags = 0;
    req->use_recv_op = true;
    if (llam_issue_io(req, false, 0U) != 0) {
        int saved_errno = errno;

        llam_api_io_req_release(g_llam_tls_shard, req);
        errno = saved_errno;
        return -1;
    }
    result = req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

static ssize_t issue_backend_write(llam_fd_t fd, const void *buf, size_t count) {
    llam_io_req_t *req = llam_api_io_req_acquire(g_llam_tls_shard);
    ssize_t result;

    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }
    req->kind = LLAM_IO_KIND_WRITE;
    req->fd = fd;
    req->buf = (void *)buf;
    req->count = count;
    if (llam_issue_io(req, false, 0U) != 0) {
        int saved_errno = errno;

        llam_api_io_req_release(g_llam_tls_shard, req);
        errno = saved_errno;
        return -1;
    }
    result = req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

static int issue_backend_poll(llam_fd_t fd, short events, int timeout_ms, short *revents) {
    llam_io_req_t *req = llam_api_io_req_acquire(g_llam_tls_shard);
    int result;

    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }
    req->kind = LLAM_IO_KIND_POLL;
    req->fd = fd;
    req->poll_events = events;
    req->timeout_ms = timeout_ms;
    if (llam_issue_io(req, timeout_ms >= 0, timeout_ms >= 0 ? llam_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0U) != 0) {
        int saved_errno = errno;

        llam_api_io_req_release(g_llam_tls_shard, req);
        errno = saved_errno;
        return -1;
    }
    result = (int)req->result;
    if (revents != NULL) {
        *revents = req->poll_revents;
    }
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

static void run_udp_poll_peek_test(windows_iocp_state_t *state) {
    llam_fd_t receiver = LLAM_INVALID_FD;
    llam_fd_t sender = LLAM_INVALID_FD;
    struct sockaddr_in recv_addr;
    int recv_addr_len = (int)sizeof(recv_addr);
    char buffer[4];
    short revents = 0;
    const char *native_udp_pollin = getenv("LLAM_WINDOWS_IOCP_UDP_POLLIN");
    bool use_native_udp_pollin = llam_env_flag_value(native_udp_pollin, 0U) != 0U;
    int rc;

    receiver = create_overlapped_udp_socket();
    sender = create_overlapped_udp_socket();
    if (LLAM_FD_IS_INVALID(receiver) || LLAM_FD_IS_INVALID(sender)) {
        task_fail(state, "udp socket create failed", errno);
        goto cleanup;
    }

    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0;
    if (bind(receiver, (const struct sockaddr *)&recv_addr, (int)sizeof(recv_addr)) != 0) {
        errno = WSAGetLastError();
        task_fail(state, "udp receiver bind failed", errno);
        goto cleanup;
    }
    memset(&recv_addr, 0, sizeof(recv_addr));
    if (getsockname(receiver, (struct sockaddr *)&recv_addr, &recv_addr_len) != 0) {
        errno = WSAGetLastError();
        task_fail(state, "udp receiver getsockname failed", errno);
        goto cleanup;
    }
    rc = sendto(sender, "udp!", 4, 0, (const struct sockaddr *)&recv_addr, (int)sizeof(recv_addr));
    if (rc != 4) {
        errno = WSAGetLastError();
        task_fail(state, "udp sendto failed", errno);
        goto cleanup;
    }
    /*
     * Native UDP IOCP polling is an opt-in backend probe.  Default CI verifies
     * the public poll contract through the immediate platform readiness path,
     * which is deterministic on hosted Windows loopback and still guarantees
     * MSG_PEEK-style readiness does not consume the datagram.
     */
    rc = use_native_udp_pollin ? issue_backend_poll(receiver, POLLIN, 5000, &revents)
                              : llam_poll_fd(receiver, POLLIN, 5000, &revents);
    if (rc != 1 || (revents & POLLIN) == 0) {
        task_fail(state, use_native_udp_pollin ?
                             "udp IOCP WSARecvFrom(MSG_PEEK) readiness failed" :
                             "udp public poll readiness failed",
                  errno);
        goto cleanup;
    }
    memset(buffer, 0, sizeof(buffer));
    rc = recvfrom(receiver, buffer, sizeof(buffer), 0, NULL, NULL);
    if (rc != (int)sizeof(buffer) || memcmp(buffer, "udp!", sizeof(buffer)) != 0) {
        errno = rc == SOCKET_ERROR ? WSAGetLastError() : EINVAL;
        task_fail(state, "udp poll consumed or corrupted datagram", errno);
        goto cleanup;
    }

cleanup:
    if (!LLAM_FD_IS_INVALID(receiver)) {
        closesocket(receiver);
    }
    if (!LLAM_FD_IS_INVALID(sender)) {
        closesocket(sender);
    }
}

static void assoc_forget_race_task(void *arg) {
    windows_iocp_state_t *state = arg;
    llam_runtime_t *rt = g_llam_tls_shard != NULL ? g_llam_tls_shard->runtime : NULL;
    llam_node_t *node;

    test_note("assoc task start");
    if (rt == NULL || rt->nodes == NULL || rt->active_nodes == 0U) {
        task_fail(state, "assoc race missing runtime node", EINVAL);
        return;
    }
    node = &rt->nodes[0];
    for (unsigned i = 0U; i < LLAM_WINDOWS_ASSOC_RACE_ROUNDS; ++i) {
        llam_fd_t fd = create_overlapped_udp_socket();

        if (LLAM_FD_IS_INVALID(fd)) {
            task_fail(state, "assoc race socket create", errno);
            return;
        }
        /*
         * This used to mutate node->windows_fd_assoc_head without locking while
         * llam_close() forgot entries from task context.  Multiple tasks doing
         * associate/close cycles now exercise that metadata path directly.
         */
        if (llam_windows_associate_fd(node, fd) != 0) {
            int saved_errno = errno;

            closesocket(fd);
            task_fail(state, "assoc race IOCP associate", saved_errno);
            return;
        }
        if (llam_close(fd) != 0) {
            task_fail(state, "assoc race llam_close", errno);
            return;
        }
        /*
         * The association stress is intentionally CPU/syscall heavy and runs
         * inside managed tasks.  Yield each iteration so this regression guard
         * does not starve the accept/connect round-trip tasks on hosted
         * Windows runners with a small worker count.
         */
        llam_yield();
    }
    atomic_fetch_add_explicit(&state->assoc_done, 1U, memory_order_relaxed);
    test_note("assoc task done");
    maybe_stop_after_all_tasks(state);
}

static void server_task(void *arg) {
    windows_iocp_state_t *state = arg;
    char buffer[4];
    llam_io_buffer_t *owned = NULL;
    const void *owned_data;
    llam_fd_t accepted;
    ssize_t nread;
    ssize_t nwritten;
    short revents = 0;
    const char *native_tcp_pollin = getenv("LLAM_WINDOWS_IOCP_TCP_POLLIN");
    bool use_native_tcp_pollin = llam_env_flag_value(native_tcp_pollin, 0U) != 0U;

    test_note("server task start");
    accepted = llam_accept(state->listener, NULL, NULL);
    if (LLAM_FD_IS_INVALID(accepted)) {
        task_fail(state, "llam_accept/AcceptEx failed", errno);
        return;
    }
    if (use_native_tcp_pollin) {
        if (issue_backend_poll(accepted, POLLIN, 5000, &revents) != 1 || (revents & POLLIN) == 0) {
            task_fail(state, "tcp IOCP WSARecv(MSG_PEEK) POLLIN readiness failed", errno);
            closesocket(accepted);
            return;
        }
    } else if (llam_poll_fd(accepted, POLLIN, 5000, &revents) != 1 || (revents & POLLIN) == 0) {
        task_fail(state, "llam_poll_fd TCP POLLIN fallback readiness failed", errno);
        closesocket(accepted);
        return;
    }
    nread = issue_backend_read(accepted, buffer, sizeof(buffer));
    if (nread != (ssize_t)sizeof(buffer) || memcmp(buffer, "ping", sizeof(buffer)) != 0) {
        task_fail(state, "llam_read/WSARecv after IOCP poll failed", errno);
        closesocket(accepted);
        return;
    }
    nwritten = issue_backend_write(accepted, "pong", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write/WSASend on server failed", errno);
        closesocket(accepted);
        return;
    }
    nread = llam_recv_owned(accepted, sizeof(buffer), 0, &owned);
    if (nread != (ssize_t)sizeof(buffer) || owned == NULL) {
        task_fail(state, "llam_recv_owned/WSARecv on server failed", errno);
        closesocket(accepted);
        return;
    }
    owned_data = llam_io_buffer_data(owned);
    if (llam_io_buffer_size(owned) != sizeof(buffer) || owned_data == NULL ||
        memcmp(owned_data, "own!", sizeof(buffer)) != 0) {
        llam_io_buffer_release(owned);
        task_fail(state, "llam_recv_owned payload mismatch", errno);
        closesocket(accepted);
        return;
    }
    llam_io_buffer_release(owned);
    nwritten = issue_backend_write(accepted, "done", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write after owned recv failed", errno);
        closesocket(accepted);
        return;
    }
    closesocket(accepted);
    atomic_fetch_add_explicit(&state->server_done, 1U, memory_order_relaxed);
    test_note("server task done");
    maybe_stop_after_all_tasks(state);
}

static void client_task(void *arg) {
    windows_iocp_state_t *state = arg;
    llam_fd_t client = create_overlapped_tcp_socket();
    char buffer[4];
    ssize_t nwritten;
    ssize_t nread;
    short revents = 0;

    test_note("client task start");
    if (LLAM_FD_IS_INVALID(client)) {
        task_fail(state, "client socket create failed", errno);
        return;
    }
    test_note("client connect");
    if (llam_connect(client, (const struct sockaddr *)&state->addr, (socklen_t)sizeof(state->addr)) != 0) {
        task_fail(state, "llam_connect/ConnectEx failed", errno);
        closesocket(client);
        return;
    }
    test_note("client pollout");
    if (issue_backend_poll(client, POLLOUT, 5000, &revents) != 1 || (revents & POLLOUT) == 0) {
        task_fail(state, "tcp IOCP WSASend POLLOUT readiness failed", errno);
        closesocket(client);
        return;
    }
    test_note("client write ping");
    nwritten = issue_backend_write(client, "ping", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write/WSASend on client failed", errno);
        closesocket(client);
        return;
    }
    test_note("client read pong");
    nread = issue_backend_read(client, buffer, sizeof(buffer));
    if (nread != (ssize_t)sizeof(buffer) || memcmp(buffer, "pong", sizeof(buffer)) != 0) {
        task_fail(state, "llam_read/WSARecv on client failed", errno);
        closesocket(client);
        return;
    }
    test_note("client write owned payload");
    nwritten = issue_backend_write(client, "own!", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write before owned recv failed", errno);
        closesocket(client);
        return;
    }
    test_note("client read done");
    nread = issue_backend_read(client, buffer, sizeof(buffer));
    if (nread != (ssize_t)sizeof(buffer) || memcmp(buffer, "done", sizeof(buffer)) != 0) {
        task_fail(state, "llam_read after owned recv failed", errno);
        closesocket(client);
        return;
    }
    test_note("client udp poll");
    run_udp_poll_peek_test(state);
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) != 0U) {
        closesocket(client);
        return;
    }
    closesocket(client);
    atomic_fetch_add_explicit(&state->client_done, 1U, memory_order_relaxed);
    test_note("client task done");
    maybe_stop_after_all_tasks(state);
}

int main(void) {
    windows_iocp_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *server;
    llam_task_t *client;
    llam_task_t *assoc_tasks[LLAM_WINDOWS_ASSOC_RACE_TASKS];

    memset(&state, 0, sizeof(state));
    state.listener = LLAM_INVALID_FD;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.server_done, 0U);
    atomic_init(&state.client_done, 0U);
    atomic_init(&state.assoc_done, 0U);
    memset(assoc_tasks, 0, sizeof(assoc_tasks));

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_opts_init failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_init_ex failed");
    }
    test_note("begin invalid fd probes");
    if (test_invalid_owned_fd_errors() != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    test_note("begin raw socket reuse probes");
    if (test_socket_authority_is_non_inheritable() != 0 ||
        test_raw_socket_reuse_revalidates_iocp() != 0 ||
        test_raw_socket_reuse_rejects_foreign_iocp() != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    test_note("begin listener setup");
    if (setup_listener(&state) != 0) {
        llam_runtime_shutdown();
        return fail_errno("listener setup failed");
    }

    test_note("begin spawn");
    server = llam_spawn(server_task, &state, NULL);
    client = llam_spawn(client_task, &state, NULL);
    for (unsigned i = 0U; i < LLAM_WINDOWS_ASSOC_RACE_TASKS; ++i) {
        assoc_tasks[i] = llam_spawn(assoc_forget_race_task, &state, NULL);
    }
    if (server == NULL || client == NULL) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_spawn failed");
    }
    for (unsigned i = 0U; i < LLAM_WINDOWS_ASSOC_RACE_TASKS; ++i) {
        if (assoc_tasks[i] == NULL) {
            closesocket(state.listener);
            llam_runtime_shutdown();
            return fail_errno("llam_spawn assoc race failed");
        }
    }
    test_note("begin run");
    if (llam_run() != 0) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_run failed");
    }
    test_note("begin join");
    if (llam_join(server) != 0 || llam_join(client) != 0) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_join failed");
    }
    for (unsigned i = 0U; i < LLAM_WINDOWS_ASSOC_RACE_TASKS; ++i) {
        if (llam_join(assoc_tasks[i]) != 0) {
            closesocket(state.listener);
            llam_runtime_shutdown();
            return fail_errno("llam_join assoc race failed");
        }
    }
    closesocket(state.listener);
    llam_runtime_shutdown();

    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_windows_iocp_io] task failed at %s errno=%d\n",
                state.first_case,
                state.first_errno);
        return 1;
    }
    if (atomic_load_explicit(&state.server_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.client_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.assoc_done, memory_order_relaxed) != LLAM_WINDOWS_ASSOC_RACE_TASKS) {
        fprintf(stderr, "[test_windows_iocp_io] missing completion\n");
        return 1;
    }
    puts("[test_windows_iocp_io] ok");
    return 0;
}
#endif
