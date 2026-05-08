/**
 * @file tests/test_io_buffers.c
 * @brief Runtime read/write, poll, owned-buffer, and recv-peek tests.
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
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct io_state {
    int stream_sv[2];
    int peek_sv[2];
    atomic_uint failures;
    atomic_uint reader_done;
    atomic_uint writer_done;
    int first_errno;
    char first_case[128];
} io_state_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_io_buffers] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_io_buffers] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void close_if_valid(int *fd) {
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void task_fail(io_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int write_all_native(int fd, const char *data, size_t len) {
    size_t offset = 0U;

    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);

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

static int test_direct_owned_read_and_poll(void) {
    llam_io_buffer_t *buffer = NULL;
    int pipe_fds[2] = {-1, -1};
    int poll_fds[2] = {-1, -1};
    short revents = 0;
    ssize_t bytes;
    char one = 0;

    errno = 0;
    if (llam_read_owned(-1, 8U, NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_read_owned NULL out did not fail with EINVAL");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_read_owned(-1, 0U, &buffer) != -1 || errno != EINVAL || buffer != NULL) {
        return test_fail("llam_read_owned zero size did not fail with EINVAL");
    }

    if (pipe(pipe_fds) != 0) {
        return test_fail_errno("pipe setup failed");
    }
    if (write_all_native(pipe_fds[1], "direct", 6U) != 0) {
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        return test_fail_errno("direct pipe write failed");
    }
    bytes = llam_read_owned(pipe_fds[0], 16U, &buffer);
    if (bytes != 6 ||
        buffer == NULL ||
        llam_io_buffer_size(buffer) != 6U ||
        llam_io_buffer_capacity(buffer) < 6U ||
        memcmp(llam_io_buffer_data(buffer), "direct", 6U) != 0) {
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        llam_io_buffer_release(buffer);
        return test_fail("direct llam_read_owned returned unexpected buffer");
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;
    close_if_valid(&pipe_fds[1]);
    bytes = llam_read_owned(pipe_fds[0], 16U, &buffer);
    if (bytes != 0 || buffer != NULL) {
        close_if_valid(&pipe_fds[0]);
        llam_io_buffer_release(buffer);
        return test_fail("direct llam_read_owned EOF did not return zero with NULL buffer");
    }
    errno = 0;
    if (llam_recv_owned(pipe_fds[0], 8U, 0, &buffer) != -1 || errno != ENOTSOCK) {
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        llam_io_buffer_release(buffer);
        return test_fail("llam_recv_owned on pipe did not fail with ENOTSOCK");
    }
    close_if_valid(&pipe_fds[0]);
    close_if_valid(&pipe_fds[1]);

    if (pipe(poll_fds) != 0) {
        return test_fail_errno("poll pipe setup failed");
    }
    if (llam_poll_fd(poll_fds[0], POLLIN, 0, &revents) != 0 || revents != 0) {
        close_if_valid(&poll_fds[0]);
        close_if_valid(&poll_fds[1]);
        return test_fail("empty pipe poll did not time out immediately");
    }
    errno = 0;
    if (llam_read_when_ready(poll_fds[0], &one, 1U, 0) != -1 || errno != ETIMEDOUT) {
        close_if_valid(&poll_fds[0]);
        close_if_valid(&poll_fds[1]);
        return test_fail("empty pipe fused read did not time out immediately");
    }
    if (write_all_native(poll_fds[1], "x", 1U) != 0) {
        close_if_valid(&poll_fds[0]);
        close_if_valid(&poll_fds[1]);
        return test_fail_errno("poll pipe write failed");
    }
    revents = 0;
    if (llam_poll_fd(poll_fds[0], POLLIN, 0, &revents) <= 0 || (revents & POLLIN) == 0) {
        close_if_valid(&poll_fds[0]);
        close_if_valid(&poll_fds[1]);
        return test_fail("ready pipe poll did not report POLLIN");
    }
    if (llam_read_when_ready(poll_fds[0], &one, 1U, 0) != 1 || one != 'x') {
        close_if_valid(&poll_fds[0]);
        close_if_valid(&poll_fds[1]);
        return test_fail("ready pipe fused read failed");
    }
    close_if_valid(&poll_fds[0]);
    close_if_valid(&poll_fds[1]);
    return 0;
}

static void io_writer_task(void *arg) {
    io_state_t *state = arg;

    llam_yield();
    if (llam_write(state->stream_sv[1], "hello", 5U) != 5) {
        task_fail(state, "llam_write stream", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->writer_done, 1U, memory_order_relaxed);
}

static void io_reader_task(void *arg) {
    io_state_t *state = arg;
    llam_io_buffer_t *buffer = NULL;
    char hello[5];
    ssize_t bytes;

    bytes = llam_read_when_ready(state->stream_sv[0], hello, sizeof(hello), 1000);
    if (bytes != 5 || memcmp(hello, "hello", 5U) != 0) {
        task_fail(state, "managed llam_read_when_ready stream", EPROTO);
        return;
    }

    bytes = llam_recv_owned(state->peek_sv[0], 4U, MSG_PEEK, &buffer);
    if (bytes != 4 ||
        buffer == NULL ||
        llam_io_buffer_size(buffer) != 4U ||
        memcmp(llam_io_buffer_data(buffer), "peek", 4U) != 0) {
        llam_io_buffer_release(buffer);
        task_fail(state, "managed llam_recv_owned MSG_PEEK", EPROTO);
        return;
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;

    bytes = llam_read_owned(state->peek_sv[0], 4U, &buffer);
    if (bytes != 4 ||
        buffer == NULL ||
        llam_io_buffer_size(buffer) != 4U ||
        memcmp(llam_io_buffer_data(buffer), "peek", 4U) != 0) {
        llam_io_buffer_release(buffer);
        task_fail(state, "managed read after MSG_PEEK", EPROTO);
        return;
    }
    llam_io_buffer_release(buffer);
    atomic_fetch_add_explicit(&state->reader_done, 1U, memory_order_relaxed);
}

static int test_managed_io_paths(void) {
    io_state_t state;
    llam_runtime_opts_t opts;

    memset(&state, 0, sizeof(state));
    state.stream_sv[0] = -1;
    state.stream_sv[1] = -1;
    state.peek_sv[0] = -1;
    state.peek_sv[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.reader_done, 0U);
    atomic_init(&state.writer_done, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.stream_sv) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, state.peek_sv) != 0) {
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("socketpair setup failed");
    }
    if (write_all_native(state.peek_sv[1], "peek", 4U) != 0) {
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("peek socket prewrite failed");
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    opts.profile = LLAM_RUNTIME_PROFILE_IO_LATENCY;
    if (llam_runtime_init(&opts) != 0) {
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("llam_runtime_init failed");
    }
    if (llam_spawn(io_reader_task, &state, NULL) == NULL ||
        llam_spawn(io_writer_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("llam_spawn failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("llam_run failed");
    }
    llam_runtime_shutdown();

    close_if_valid(&state.stream_sv[0]);
    close_if_valid(&state.stream_sv[1]);
    close_if_valid(&state.peek_sv[0]);
    close_if_valid(&state.peek_sv[1]);

    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_io_buffers] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        return 1;
    }
    if (atomic_load_explicit(&state.reader_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.writer_done, memory_order_relaxed) != 1U) {
        return test_fail("managed I/O tasks did not both complete");
    }
    return 0;
}

int main(void) {
    if (test_direct_owned_read_and_poll() != 0 ||
        test_managed_io_paths() != 0) {
        return 1;
    }
    printf("[test_io_buffers] ok\n");
    return 0;
}
