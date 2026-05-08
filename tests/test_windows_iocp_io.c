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

typedef struct windows_iocp_state {
    llam_fd_t listener;
    struct sockaddr_in addr;
    atomic_uint failures;
    atomic_uint server_done;
    atomic_uint client_done;
    int first_errno;
    char first_case[96];
} windows_iocp_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_windows_iocp_io] %s: errno=%d\n", message, errno);
    return 1;
}

static void task_fail(windows_iocp_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
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
    if (issue_backend_poll(receiver, POLLIN, 5000, &revents) != 1 || (revents & POLLIN) == 0) {
        task_fail(state, "udp IOCP WSARecvFrom(MSG_PEEK) readiness failed", errno);
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
    bool use_native_tcp_pollin = native_tcp_pollin != NULL &&
                                 native_tcp_pollin[0] != '\0' &&
                                 strcmp(native_tcp_pollin, "0") != 0;

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
}

static void client_task(void *arg) {
    windows_iocp_state_t *state = arg;
    llam_fd_t client = create_overlapped_tcp_socket();
    char buffer[4];
    ssize_t nwritten;
    ssize_t nread;
    short revents = 0;

    if (LLAM_FD_IS_INVALID(client)) {
        task_fail(state, "client socket create failed", errno);
        return;
    }
    if (llam_connect(client, (const struct sockaddr *)&state->addr, (socklen_t)sizeof(state->addr)) != 0) {
        task_fail(state, "llam_connect/ConnectEx failed", errno);
        closesocket(client);
        return;
    }
    if (issue_backend_poll(client, POLLOUT, 5000, &revents) != 1 || (revents & POLLOUT) == 0) {
        task_fail(state, "tcp IOCP WSASend POLLOUT readiness failed", errno);
        closesocket(client);
        return;
    }
    nwritten = issue_backend_write(client, "ping", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write/WSASend on client failed", errno);
        closesocket(client);
        return;
    }
    nread = issue_backend_read(client, buffer, sizeof(buffer));
    if (nread != (ssize_t)sizeof(buffer) || memcmp(buffer, "pong", sizeof(buffer)) != 0) {
        task_fail(state, "llam_read/WSARecv on client failed", errno);
        closesocket(client);
        return;
    }
    nwritten = issue_backend_write(client, "own!", 4U);
    if (nwritten != 4) {
        task_fail(state, "llam_write before owned recv failed", errno);
        closesocket(client);
        return;
    }
    nread = issue_backend_read(client, buffer, sizeof(buffer));
    if (nread != (ssize_t)sizeof(buffer) || memcmp(buffer, "done", sizeof(buffer)) != 0) {
        task_fail(state, "llam_read after owned recv failed", errno);
        closesocket(client);
        return;
    }
    run_udp_poll_peek_test(state);
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) != 0U) {
        closesocket(client);
        return;
    }
    closesocket(client);
    atomic_fetch_add_explicit(&state->client_done, 1U, memory_order_relaxed);
}

int main(void) {
    windows_iocp_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *server;
    llam_task_t *client;

    memset(&state, 0, sizeof(state));
    state.listener = LLAM_INVALID_FD;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.server_done, 0U);
    atomic_init(&state.client_done, 0U);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_opts_init failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_init_ex failed");
    }
    if (setup_listener(&state) != 0) {
        llam_runtime_shutdown();
        return fail_errno("listener setup failed");
    }

    server = llam_spawn(server_task, &state, NULL);
    client = llam_spawn(client_task, &state, NULL);
    if (server == NULL || client == NULL) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_spawn failed");
    }
    if (llam_run() != 0) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_run failed");
    }
    if (llam_join(server) != 0 || llam_join(client) != 0) {
        closesocket(state.listener);
        llam_runtime_shutdown();
        return fail_errno("llam_join failed");
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
        atomic_load_explicit(&state.client_done, memory_order_relaxed) != 1U) {
        fprintf(stderr, "[test_windows_iocp_io] missing completion\n");
        return 1;
    }
    puts("[test_windows_iocp_io] ok");
    return 0;
}
#endif
