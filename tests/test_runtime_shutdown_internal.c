/**
 * @file tests/test_runtime_shutdown_internal.c
 * @brief Internal shutdown and watch-cleanup invariants.
 *
 * @details
 * This test intentionally reaches into private runtime state to exercise
 * teardown paths that are difficult to reach deterministically through the
 * public API.  Leak-enabled sanitizer jobs use this path to catch ownership
 * regressions in queued I/O readiness cleanup.
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

#include "runtime_internal.h"

#if LLAM_RUNTIME_BACKEND_DARWIN
#include "io/darwin/runtime_io_watch_darwin_internal.h"
#elif LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_watch_linux_internal.h"
#elif LLAM_RUNTIME_BACKEND_WINDOWS
#include "io/windows/runtime_io_watch_windows_internal.h"
#endif

#include <errno.h>
#include <limits.h>
#if !LLAM_RUNTIME_BACKEND_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_errno(const char *message) {
    fprintf(stderr, "test_runtime_shutdown_internal: %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int fail_msg(const char *message) {
    fprintf(stderr, "test_runtime_shutdown_internal: %s\n", message);
    return 1;
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    return llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
}

#if !LLAM_RUNTIME_BACKEND_WINDOWS
static void close_if_valid(int *fd) {
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static int make_loopback_listener(int *listener_out) {
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    if (listener_out == NULL) {
        errno = EINVAL;
        return -1;
    }
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
        listen(fd, 16) != 0) {
        close_if_valid(&fd);
        return -1;
    }
    *listener_out = fd;
    return 0;
}
#endif

static int exercise_recv_ready_copy_payload_shutdown(void) {
    llam_recv_watch_t *watch;
    llam_recv_ready_t *ready;
    unsigned char *payload;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node");
    }

    watch = calloc(1U, sizeof(*watch));
    ready = calloc(1U, sizeof(*ready));
    payload = malloc(4096U);
    if (watch == NULL || ready == NULL || payload == NULL) {
        free(payload);
        free(ready);
        free(watch);
        llam_runtime_shutdown();
        return fail_errno("allocation failed");
    }
    memset(payload, 0x5a, 4096U);

    /*
     * Simulate a copied recv completion that shutdown owns.  If teardown only
     * frees the ready node and forgets copy_data, leak-enabled jobs report it.
     */
    ready->copy_data = payload;
    ready->copy_capacity = 4096U;
    ready->size = 4096U;
    ready->has_buffer = false;
    watch->ready_head = ready;
    watch->ready_tail = ready;
    watch->ready_depth = 1U;

    {
        int lock_rc = pthread_mutex_lock(&g_llam_runtime.nodes[0].watch_lock);

        if (lock_rc != 0) {
            errno = lock_rc;
            free(payload);
            free(ready);
            free(watch);
            llam_runtime_shutdown();
            return fail_errno("watch lock failed");
        }
    }
    watch->next = g_llam_runtime.nodes[0].recv_watches;
    g_llam_runtime.nodes[0].recv_watches = watch;
    {
        int unlock_rc = pthread_mutex_unlock(&g_llam_runtime.nodes[0].watch_lock);

        if (unlock_rc != 0) {
            errno = unlock_rc;
            llam_runtime_shutdown();
            return fail_errno("watch unlock failed");
        }
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_recv_ready_pop_without_transfer(void) {
#if LLAM_RUNTIME_BACKEND_DARWIN || LLAM_RUNTIME_BACKEND_LINUX
    llam_recv_watch_t watch;
    unsigned char data[64];
    size_t size = 0U;

    memset(&watch, 0, sizeof(watch));
    memset(data, 0x33, sizeof(data));
#if LLAM_RUNTIME_BACKEND_DARWIN
    if (!llam_recv_watch_push_ready_copy(&watch, data, sizeof(data))) {
        return fail_errno("recv ready copy push failed");
    }
#else
    {
        unsigned char *copy = malloc(sizeof(data));

        if (copy == NULL) {
            return fail_errno("recv ready copy allocation failed");
        }
        memcpy(copy, data, sizeof(data));
        if (!llam_recv_watch_push_ready(&watch, sizeof(data), 0U, false, UINT_MAX, copy, sizeof(data))) {
            return fail_errno("recv ready push failed");
        }
    }
#endif

    /*
     * Pop without requesting copy_data ownership.  The watch helper must free
     * the queued payload itself; leak-enabled sanitizer jobs catch regressions.
     */
    if (!llam_recv_watch_pop_ready(&watch, &size, NULL, NULL, NULL, NULL, NULL)) {
        return fail_errno("recv ready pop failed");
    }
    if (size != sizeof(data) || watch.ready_head != NULL || watch.ready_tail != NULL || watch.ready_depth != 0U) {
        return fail_msg("recv ready pop violated queue invariants");
    }
#endif
    return 0;
}

static int exercise_close_purges_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_DARWIN || LLAM_RUNTIME_BACKEND_LINUX
    llam_node_t *node;
    llam_accept_watch_t *watch;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for close watch purge");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for close watch purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_shutdown();
        return fail_errno("listener setup failed for close watch purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("ready fd setup failed for close watch purge");
    }

    node = &g_llam_runtime.nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("watch lock failed for close watch purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("accept watch ready setup failed for close watch purge");
    }
    /*
     * The watch owns ready_pipe[0] from this point.  The close-boundary cleanup
     * must release it before the runtime reaches full shutdown.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("watch unlock failed for close watch purge");
    }

    if (llam_close(listener) != 0) {
        close_if_valid(&ready_pipe[1]);
        listener = -1;
        llam_runtime_shutdown();
        return fail_errno("llam_close failed during close watch purge");
    }
    listener = -1;
    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_shutdown();
        return fail_msg("llam_close did not purge accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_host_close_purges_explicit_runtime_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_DARWIN || LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t *runtime = NULL;
    llam_node_t *node;
    llam_accept_watch_t *watch;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    if (llam_runtime_create(NULL, 0U, &runtime) != 0) {
        return fail_errno("explicit runtime init failed for host close watch purge");
    }
    if (runtime == NULL || runtime->nodes == NULL || runtime->active_nodes == 0U) {
        llam_runtime_destroy(runtime);
        return fail_msg("explicit runtime initialized without an I/O node for host close watch purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_destroy(runtime);
        return fail_errno("listener setup failed for explicit host close watch purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("ready fd setup failed for explicit host close watch purge");
    }

    node = &runtime->nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("watch lock failed for explicit host close watch purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("accept watch ready setup failed for explicit host close watch purge");
    }
    /*
     * This models an embedder-owned fd closed from a host thread.  There is no
     * TLS task/shard cursor, so close-boundary cleanup must scan live explicit
     * runtimes instead of only the legacy default runtime.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("watch unlock failed for explicit host close watch purge");
    }

    if (llam_close(listener) != 0) {
        close_if_valid(&ready_pipe[1]);
        listener = -1;
        llam_runtime_destroy(runtime);
        return fail_errno("llam_close failed during explicit host close watch purge");
    }
    listener = -1;
    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(runtime);
        return fail_msg("host llam_close did not purge explicit-runtime accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_destroy(runtime);
#endif
    return 0;
}

typedef struct managed_close_state {
    int fd;
    int rc;
    int error;
} managed_close_state_t;

static void managed_close_task(void *arg) {
    managed_close_state_t *state = arg;

    if (state == NULL) {
        return;
    }
    state->rc = llam_close(state->fd);
    state->error = errno;
}

static int exercise_managed_close_purges_peer_runtime_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_DARWIN || LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t *closer_runtime = NULL;
    llam_runtime_t *watch_runtime = NULL;
    llam_node_t *node;
    llam_accept_watch_t *watch;
    llam_task_t *task = NULL;
    managed_close_state_t close_state;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    memset(&close_state, 0, sizeof(close_state));
    close_state.fd = -1;
    close_state.rc = -1;
    if (llam_runtime_create(NULL, 0U, &closer_runtime) != 0 ||
        llam_runtime_create(NULL, 0U, &watch_runtime) != 0) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("explicit runtime init failed for managed peer close purge");
    }
    if (watch_runtime == NULL || watch_runtime->nodes == NULL || watch_runtime->active_nodes == 0U) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_msg("explicit watch runtime initialized without an I/O node for managed peer close purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("listener setup failed for managed peer close purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("ready fd setup failed for managed peer close purge");
    }

    node = &watch_runtime->nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("watch lock failed for managed peer close purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("accept watch ready setup failed for managed peer close purge");
    }
    /*
     * The fd namespace is process-wide.  A managed task in one runtime can
     * close an embedder-owned descriptor while another runtime still has idle
     * readiness cached for that descriptor number.  close-boundary cleanup must
     * therefore cover every live runtime, not just the task's owner runtime.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("watch unlock failed for managed peer close purge");
    }

    close_state.fd = listener;
    task = llam_runtime_spawn_ex(closer_runtime, managed_close_task, &close_state, NULL, 0U);
    if (task == NULL ||
        llam_runtime_run_handle(closer_runtime) != 0 ||
        llam_join(task) != 0) {
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("managed close task failed for peer close purge");
    }
    task = NULL;
    listener = -1;
    if (close_state.rc != 0) {
        errno = close_state.error;
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("managed llam_close failed for peer close purge");
    }

    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_msg("managed llam_close did not purge peer-runtime accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_destroy(watch_runtime);
    llam_runtime_destroy(closer_runtime);
#endif
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX
static bool io_uring_unavailable_for_direct_internal_test(int rc) {
    int err = -rc;

    return err == EPERM || err == EACCES || err == ENOSYS || err == EOPNOTSUPP;
}
#endif

static int exercise_linux_oversized_submit_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));

    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for oversized submit test failed");
    }

    node.runtime = &runtime;
    atomic_init(&node.pending_ops, 1U);
    req.kind = LLAM_IO_KIND_WRITE;
    req.count = (size_t)UINT_MAX + 1U;
    req.owner_shard = UINT_MAX;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * Oversized backend requests are rejected before SQE acquisition.  If the
     * guard consumes an SQE and then completes the request locally, a later
     * ring submit can observe an uninitialized SQE.
     */
    llam_io_submit_one(&node, &req);
    if (req.result != -1 || req.error_code != EINVAL) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("oversized Linux I/O submit did not report EINVAL");
    }
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("oversized Linux I/O submit consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
#endif
    return 0;
}

static int exercise_linux_invalid_request_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));

    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for invalid request test failed");
    }

    node.runtime = &runtime;
    atomic_init(&node.pending_ops, 1U);
    req.kind = (llam_io_kind_t)UINT_MAX;
    req.owner_shard = UINT_MAX;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * Unsupported request kinds complete locally.  They must be rejected before
     * SQE acquisition, otherwise the ring can later submit a stale entry.
     */
    llam_io_submit_one(&node, &req);
    if (req.result != -1 || req.error_code != EINVAL) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("invalid Linux I/O request did not report EINVAL");
    }
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("invalid Linux I/O request consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
#endif
    return 0;
}

static int exercise_linux_invalid_control_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_control_op_t *op;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));

    rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (rc != 0) {
        errno = rc;
        return fail_errno("watch lock init for invalid control test failed");
    }
    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        pthread_mutex_destroy(&node.watch_lock);
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for invalid control test failed");
    }

    op = calloc(1U, sizeof(*op));
    if (op == NULL) {
        io_uring_queue_exit(&node.ring);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_errno("control op allocation failed");
    }

    node.runtime = &runtime;
    op->kind = (llam_io_control_kind_t)UINT_MAX;
    /*
     * Invalid or malformed control operations are a defensive path, but they
     * still must not burn an SQE before being dropped locally.
     */
    llam_io_submit_control_op(&node, op);
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("invalid Linux control submit consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
    pthread_mutex_destroy(&node.watch_lock);
#endif
    return 0;
}

static int exercise_linux_wait_cqe_interrupt_policy(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    /*
     * io_uring_wait_cqe_timeout can be interrupted by process signals.  The
     * worker should simply retry later; treating EINTR like a backend failure
     * poisons otherwise healthy runtimes under profilers or signal-heavy tests.
     */
    if (llam_linux_wait_cqe_error_is_fatal(EINTR)) {
        return fail_msg("Linux CQ wait EINTR was treated as fatal");
    }
    if (llam_linux_wait_cqe_error_is_fatal(ETIME)) {
        return fail_msg("Linux CQ wait timeout was treated as fatal");
    }
    if (!llam_linux_wait_cqe_error_is_fatal(EIO)) {
        return fail_msg("Linux CQ wait EIO was not treated as fatal");
    }
#endif
    return 0;
}

static int exercise_completion_drops_stale_cancel_control(void) {
#if LLAM_RUNTIME_BACKEND_DARWIN || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_node_t *node;
    llam_io_req_t req;
    int lock_rc;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for cancel-control completion race");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for cancel-control completion race");
    }

    node = &g_llam_runtime.nodes[0];
    memset(&req, 0, sizeof(req));
    req.kind = LLAM_IO_KIND_READ;
    req.owner_runtime = &g_llam_runtime;
    req.owner_shard = UINT_MAX;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 1U);

    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch lock failed for cancel-control completion race");
    }
    if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_REQ_CANCEL, &req) != 0) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        llam_runtime_shutdown();
        return fail_errno("queue cancel control failed for completion race");
    }
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch unlock failed for cancel-control completion race");
    }

    /*
     * This models natural I/O completion winning before the queued cancel
     * control is processed. Completion must unlink that control before the
     * waiting task can resume and release the request storage. On Linux this
     * also prevents a stale io_uring cancel SQE from targeting a later request
     * that reuses the same embedded request address.
     */
#if LLAM_RUNTIME_BACKEND_DARWIN
    llam_io_complete_req(node, &req, 0, false);
#elif LLAM_RUNTIME_BACKEND_LINUX
    llam_io_complete_req(node, &req, 0, 0U, false);
#else
    llam_windows_complete_req(node, &req, 0, false);
#endif

    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch relock failed for cancel-control completion race");
    }
    if (node->control_head != NULL || node->control_tail != NULL) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        llam_runtime_shutdown();
        return fail_msg("completion left a stale cancel control queued");
    }
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch final unlock failed for cancel-control completion race");
    }
    if (atomic_load_explicit(&req.cancel_queued, memory_order_acquire) != 0U ||
        atomic_load_explicit(&req.wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE) {
        llam_runtime_shutdown();
        return fail_msg("completion did not clear cancel/wait ownership");
    }

    llam_runtime_shutdown();
#endif
    return 0;
}

int main(void) {
    if (exercise_recv_ready_copy_payload_shutdown() != 0) {
        return 1;
    }
    if (exercise_recv_ready_pop_without_transfer() != 0) {
        return 1;
    }
    if (exercise_close_purges_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_host_close_purges_explicit_runtime_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_managed_close_purges_peer_runtime_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_linux_oversized_submit_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_invalid_request_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_invalid_control_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_wait_cqe_interrupt_policy() != 0) {
        return 1;
    }
    if (exercise_completion_drops_stale_cancel_control() != 0) {
        return 1;
    }
    printf("test_runtime_shutdown_internal ok\n");
    return 0;
}
