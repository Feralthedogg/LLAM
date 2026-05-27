/**
 * @file tests/test_connect_io.c
 * @brief Runtime-aware connect tests for direct, managed, and invalid paths.
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

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct connect_state {
    int listener_fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} connect_state_t;

typedef struct invalid_connect_state {
    atomic_uint failures;
    int observed_errno;
} invalid_connect_state_t;

typedef struct invalid_accept_state {
    int listener_fd;
    atomic_uint failures;
    int observed_errno;
} invalid_accept_state_t;

typedef struct accept_reuse_state {
    int listener_fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    int client_fds[8];
    int first_accepted_fd;
    int second_accept_fd;
    atomic_uint accept_started;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} accept_reuse_state_t;

typedef struct accept_reuse_client_arg {
    accept_reuse_state_t *state;
    unsigned index;
} accept_reuse_client_arg_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_connect_io] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_connect_io] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void close_if_valid(int *fd) {
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void task_fail(connect_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void accept_reuse_fail(accept_reuse_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int make_loopback_listener(int *listener_out, struct sockaddr_storage *addr_out, socklen_t *addrlen_out) {
    struct sockaddr_in addr;
    socklen_t len = (socklen_t)sizeof(addr);
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0U);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr *)(const void *)&addr, (socklen_t)sizeof(addr)) != 0 ||
        listen(fd, 32) != 0 ||
        getsockname(fd, (struct sockaddr *)(void *)&addr, &len) != 0) {
        close_if_valid(&fd);
        return -1;
    }

    memset(addr_out, 0, sizeof(*addr_out));
    memcpy(addr_out, &addr, sizeof(addr));
    *addrlen_out = len;
    *listener_out = fd;
    return 0;
}

static int write_all_llam(int fd, const char *data, size_t len) {
    size_t offset = 0U;

    while (offset < len) {
        ssize_t written = llam_write(fd, data + offset, len - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int read_exact_llam(int fd, char *data, size_t len) {
    size_t offset = 0U;

    while (offset < len) {
        ssize_t bytes = llam_read(fd, data + offset, len - offset);

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes == 0) {
            errno = ECONNRESET;
            return -1;
        }
        offset += (size_t)bytes;
    }
    return 0;
}

static void accept_echo_task(void *arg) {
    connect_state_t *state = arg;
    char buf[4];
    int fd;

    fd = llam_accept(state->listener_fd, NULL, NULL);
    if (fd < 0) {
        task_fail(state, "llam_accept", errno);
        return;
    }
    if (read_exact_llam(fd, buf, sizeof(buf)) != 0) {
        task_fail(state, "accept read_exact", errno);
        close_if_valid(&fd);
        return;
    }
    if (memcmp(buf, "ping", sizeof(buf)) != 0) {
        task_fail(state, "accept payload mismatch", EPROTO);
        close_if_valid(&fd);
        return;
    }
    if (write_all_llam(fd, "pong", 4U) != 0) {
        task_fail(state, "accept write_all", errno);
        close_if_valid(&fd);
        return;
    }
    close_if_valid(&fd);
}

static void connect_echo_task(void *arg) {
    connect_state_t *state = arg;
    char buf[4];
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        task_fail(state, "client socket", errno);
        return;
    }
    if (llam_connect(fd, (const struct sockaddr *)(const void *)&state->addr, state->addrlen) != 0) {
        task_fail(state, "llam_connect", errno);
        close_if_valid(&fd);
        return;
    }
    if (write_all_llam(fd, "ping", 4U) != 0) {
        task_fail(state, "connect write_all", errno);
        close_if_valid(&fd);
        return;
    }
    if (read_exact_llam(fd, buf, sizeof(buf)) != 0) {
        task_fail(state, "connect read_exact", errno);
        close_if_valid(&fd);
        return;
    }
    if (memcmp(buf, "pong", sizeof(buf)) != 0) {
        task_fail(state, "connect payload mismatch", EPROTO);
    }
    close_if_valid(&fd);
}

static void invalid_connect_task(void *arg) {
    invalid_connect_state_t *state = arg;
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        state->observed_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9U);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    errno = 0;
    if (llam_connect(fd, (const struct sockaddr *)(const void *)&addr, 1U) != -1 || errno == 0) {
        state->observed_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    close_if_valid(&fd);
}

static void invalid_accept_task(void *arg) {
    invalid_accept_state_t *state = arg;
    struct sockaddr_storage addr;
    socklen_t addrlen = (socklen_t)sizeof(addr);

    errno = 0;
    if (!LLAM_FD_IS_INVALID(llam_accept(state->listener_fd, NULL, &addrlen)) ||
        errno != EINVAL) {
        state->observed_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }

    errno = 0;
    if (!LLAM_FD_IS_INVALID(llam_accept(state->listener_fd, (struct sockaddr *)(void *)&addr, NULL)) ||
        errno != EINVAL) {
        state->observed_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void accept_reuse_first_task(void *arg) {
    accept_reuse_state_t *state = arg;
    int fd;

    atomic_store_explicit(&state->accept_started, 1U, memory_order_release);
    fd = llam_accept(state->listener_fd, NULL, NULL);
    if (fd < 0) {
        accept_reuse_fail(state, "first llam_accept", errno);
        return;
    }
    state->first_accepted_fd = fd;
}

static void accept_reuse_client_task(void *arg) {
    accept_reuse_client_arg_t *client = arg;
    accept_reuse_state_t *state = client->state;
    int fd;

    while (atomic_load_explicit(&state->accept_started, memory_order_acquire) == 0U) {
        llam_yield();
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        accept_reuse_fail(state, "reuse client socket", errno);
        return;
    }
    if (llam_connect(fd, (const struct sockaddr *)(const void *)&state->addr, state->addrlen) != 0) {
        int saved_errno = errno;

        close_if_valid(&fd);
        accept_reuse_fail(state, "reuse client connect", saved_errno);
        return;
    }
    state->client_fds[client->index] = fd;
}

static void accept_reuse_second_task(void *arg) {
    accept_reuse_state_t *state = arg;
    int fd;

    fd = llam_accept(state->listener_fd, NULL, NULL);
    if (fd >= 0) {
        state->second_accept_fd = fd;
        /*
         * No client ever connects to the replacement listener in this phase.
         * Success means the accept watch leaked a ready fd from the old listener.
         */
        accept_reuse_fail(state, "replacement listener accepted stale fd", EPROTO);
        return;
    }
    if (errno != ECANCELED) {
        accept_reuse_fail(state, "replacement accept unexpected errno", errno);
    }
}

static void accept_reuse_stop_task(void *arg) {
    (void)arg;

    (void)llam_sleep_ns(20000000ULL);
    (void)llam_runtime_request_stop();
}

static int test_invalid_direct_connect(void) {
    struct sockaddr_in addr;
    int fd;

    errno = 0;
    if (llam_connect(-1, NULL, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_connect(NULL) did not fail with EINVAL");
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return test_fail_errno("invalid-direct socket failed");
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9U);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    errno = 0;
    if (llam_connect(fd, (const struct sockaddr *)(const void *)&addr, 1U) != -1 || errno == 0) {
        close_if_valid(&fd);
        return test_fail("llam_connect invalid addrlen unexpectedly succeeded");
    }
    close_if_valid(&fd);
    return 0;
}

static int test_invalid_direct_accept_address_pair(void) {
    struct sockaddr_storage addr;
    socklen_t addrlen = 0U;
    socklen_t peer_len = (socklen_t)sizeof(addr);
    int listener = -1;
    int client = -1;
    int accepted = -1;

    if (make_loopback_listener(&listener, &addr, &addrlen) != 0) {
        return test_fail_errno("invalid-direct accept listener setup failed");
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        close_if_valid(&listener);
        return test_fail_errno("invalid-direct accept client socket failed");
    }
    if (connect(client, (const struct sockaddr *)(const void *)&addr, addrlen) != 0) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail_errno("invalid-direct accept client connect failed");
    }

    errno = 0;
    if (!LLAM_FD_IS_INVALID(llam_accept(listener, NULL, &peer_len)) ||
        errno != EINVAL) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail("llam_accept(NULL, addrlen) did not fail with EINVAL");
    }

    errno = 0;
    if (!LLAM_FD_IS_INVALID(llam_accept(listener, (struct sockaddr *)(void *)&addr, NULL)) ||
        errno != EINVAL) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail("llam_accept(addr, NULL) did not fail with EINVAL");
    }

    /*
     * The invalid calls must be rejected before accept(2), otherwise a malformed
     * FFI call can consume a pending connection while reporting success.
     */
    accepted = accept(listener, NULL, NULL);
    if (accepted < 0) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail_errno("invalid-direct accept guard consumed pending connection");
    }

    close_if_valid(&accepted);
    close_if_valid(&client);
    close_if_valid(&listener);
    return 0;
}

static int test_direct_connect_success(void) {
    struct sockaddr_storage addr;
    socklen_t addrlen = 0U;
    int listener = -1;
    int client = -1;
    int accepted = -1;

    if (make_loopback_listener(&listener, &addr, &addrlen) != 0) {
        return test_fail_errno("direct listener setup failed");
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        close_if_valid(&listener);
        return test_fail_errno("direct client socket failed");
    }
    if (llam_connect(client, (const struct sockaddr *)(const void *)&addr, addrlen) != 0) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail_errno("direct llam_connect failed");
    }
    accepted = accept(listener, NULL, NULL);
    if (accepted < 0) {
        close_if_valid(&client);
        close_if_valid(&listener);
        return test_fail_errno("direct accept failed");
    }
    close_if_valid(&accepted);
    close_if_valid(&client);
    close_if_valid(&listener);
    return 0;
}

static int test_managed_connect_success_and_invalid(void) {
    connect_state_t state;
    invalid_connect_state_t invalid_state;
    invalid_accept_state_t invalid_accept;
    llam_runtime_opts_t opts;
    llam_task_t *accept_task;
    llam_task_t *connect_task;
    llam_task_t *invalid_task;
    llam_task_t *invalid_accept_task_handle;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    memset(&invalid_state, 0, sizeof(invalid_state));
    memset(&invalid_accept, 0, sizeof(invalid_accept));
    state.listener_fd = -1;
    invalid_accept.listener_fd = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&invalid_state.failures, 0U);
    atomic_init(&invalid_accept.failures, 0U);

    if (make_loopback_listener(&state.listener_fd, &state.addr, &state.addrlen) != 0) {
        return test_fail_errno("managed listener setup failed");
    }
    invalid_accept.listener_fd = state.listener_fd;

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        close_if_valid(&state.listener_fd);
        return test_fail_errno("llam_runtime_init failed");
    }

    accept_task = llam_spawn(accept_echo_task, &state, NULL);
    connect_task = llam_spawn(connect_echo_task, &state, NULL);
    invalid_task = llam_spawn(invalid_connect_task, &invalid_state, NULL);
    invalid_accept_task_handle = llam_spawn(invalid_accept_task, &invalid_accept, NULL);
    if (accept_task == NULL || connect_task == NULL || invalid_task == NULL ||
        invalid_accept_task_handle == NULL) {
        rc = test_fail_errno("llam_spawn failed");
    } else if (llam_run() != 0) {
        rc = test_fail_errno("llam_run failed");
    }

    llam_runtime_shutdown();
    close_if_valid(&state.listener_fd);

    if (rc != 0) {
        return rc;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_connect_io] managed connect case failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        return 1;
    }
    if (atomic_load_explicit(&invalid_state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_connect_io] managed invalid connect expectation failed errno=%d (%s)\n",
                invalid_state.observed_errno,
                strerror(invalid_state.observed_errno));
        return 1;
    }
    if (atomic_load_explicit(&invalid_accept.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_connect_io] managed invalid accept expectation failed errno=%d (%s)\n",
                invalid_accept.observed_errno,
                strerror(invalid_accept.observed_errno));
        return 1;
    }
    return 0;
}

static int test_managed_accept_watch_rejects_reused_listener_fd(void) {
#if defined(__APPLE__)
    accept_reuse_state_t state;
    accept_reuse_client_arg_t client_args[8];
    llam_runtime_opts_t opts;
    llam_task_t *first_accept_task;
    llam_task_t *client_tasks[8];
    llam_task_t *second_accept_task;
    llam_task_t *stop_task;
    int old_listener_fd;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    state.listener_fd = -1;
    state.first_accepted_fd = -1;
    state.second_accept_fd = -1;
    for (size_t i = 0U; i < sizeof(state.client_fds) / sizeof(state.client_fds[0]); ++i) {
        state.client_fds[i] = -1;
    }
    atomic_init(&state.accept_started, 0U);
    atomic_init(&state.failures, 0U);

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("accept-reuse runtime init failed");
    }

    if (make_loopback_listener(&state.listener_fd, &state.addr, &state.addrlen) != 0) {
        rc = test_fail_errno("accept-reuse listener setup failed");
        goto done;
    }
    old_listener_fd = state.listener_fd;

    first_accept_task = llam_spawn(accept_reuse_first_task, &state, NULL);
    if (first_accept_task == NULL) {
        rc = test_fail_errno("accept-reuse first accept spawn failed");
        goto done;
    }
    for (size_t i = 0U; i < sizeof(client_tasks) / sizeof(client_tasks[0]); ++i) {
        client_args[i].state = &state;
        client_args[i].index = (unsigned)i;
        client_tasks[i] = llam_spawn(accept_reuse_client_task, &client_args[i], NULL);
        if (client_tasks[i] == NULL) {
            rc = test_fail_errno("accept-reuse client spawn failed");
            goto done;
        }
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("accept-reuse first run failed");
        goto done;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("accept-reuse setup phase failed");
        goto done;
    }

    /*
     * Keep every old accepted/client fd open, close only the listener, then open a
     * new listener.  The kernel should reuse the just-freed listener fd; if LLAM's
     * accept watch is keyed only by fd number, the next managed accept can consume
     * a buffered connection from the old listener without any new client.
     */
    close_if_valid(&state.listener_fd);
    if (make_loopback_listener(&state.listener_fd, &state.addr, &state.addrlen) != 0) {
        rc = test_fail_errno("accept-reuse replacement listener setup failed");
        goto done;
    }
    if (state.listener_fd != old_listener_fd) {
        rc = test_fail("accept-reuse replacement listener did not reuse fd");
        goto done;
    }

    atomic_store_explicit(&state.accept_started, 0U, memory_order_release);
    second_accept_task = llam_spawn(accept_reuse_second_task, &state, NULL);
    stop_task = llam_spawn(accept_reuse_stop_task, NULL, NULL);
    if (second_accept_task == NULL || stop_task == NULL) {
        rc = test_fail_errno("accept-reuse second phase spawn failed");
        goto done;
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("accept-reuse second run failed");
        goto done;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_connect_io] accept-reuse failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        rc = 1;
    }

done:
    llam_runtime_shutdown();
    close_if_valid(&state.second_accept_fd);
    close_if_valid(&state.first_accepted_fd);
    close_if_valid(&state.listener_fd);
    for (size_t i = 0U; i < sizeof(state.client_fds) / sizeof(state.client_fds[0]); ++i) {
        close_if_valid(&state.client_fds[i]);
    }
    return rc;
#else
    return 0;
#endif
}

int main(void) {
    if (test_invalid_direct_connect() != 0 ||
        test_invalid_direct_accept_address_pair() != 0 ||
        test_direct_connect_success() != 0 ||
        test_managed_connect_success_and_invalid() != 0 ||
        test_managed_accept_watch_rejects_reused_listener_fd() != 0) {
        return 1;
    }
    printf("[test_connect_io] ok\n");
    return 0;
}
