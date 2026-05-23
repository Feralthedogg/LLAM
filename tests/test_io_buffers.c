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
#include "../src/io/runtime_io_api_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif
#include <unistd.h>

#ifndef IOV_MAX
#define TEST_IOV_MAX 1024
#else
#define TEST_IOV_MAX IOV_MAX
#endif

#define OWNED_BUFFER_RACE_THREADS 4
#define OWNED_BUFFER_RACE_ITERS 4096
#define OWNED_BUFFER_RAW_RELEASE_MAX_COLLISION_SLOTS 131072U

#if defined(__linux__) && !defined(MAP_NORESERVE)
#define MAP_NORESERVE 0
#endif

typedef struct io_state {
    int stream_sv[2];
    int peek_sv[2];
    int null_fd;
    int read_null_fd;
    atomic_uint failures;
    atomic_uint reader_done;
    atomic_uint writer_done;
    int first_errno;
    char first_case[128];
} io_state_t;

typedef struct escaped_owned_state {
    int pipe_fds[2];
    llam_io_buffer_t *buffer;
    atomic_uint failures;
} escaped_owned_state_t;

typedef struct owned_buffer_race_state {
    atomic_uintptr_t current;
    atomic_uint ready;
    atomic_uint stop;
} owned_buffer_race_state_t;

static uintptr_t owned_buffer_slot_handle(const llam_io_buffer_t *buffer) {
    return ((uintptr_t)buffer) >> 32U;
}

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

static void escaped_owned_task_fail(escaped_owned_state_t *state) {
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static void *owned_buffer_accessor_race_thread(void *arg) {
    owned_buffer_race_state_t *state = arg;
    uintptr_t checksum = 0U;

    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        llam_io_buffer_t *buffer =
            (llam_io_buffer_t *)atomic_load_explicit(&state->current, memory_order_acquire);

        if (buffer != NULL) {
            /*
             * These accessors are allowed to race with release from another
             * unmanaged thread. The test intentionally does not dereference the
             * returned data pointer because ownership can be consumed
             * immediately after the accessor returns; the bug being guarded is
             * a UAF inside the accessor itself.
             */
            checksum += (uintptr_t)llam_io_buffer_data(buffer);
            checksum += llam_io_buffer_size(buffer);
            checksum += llam_io_buffer_capacity(buffer);
        }
    }
    return (void *)checksum;
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
    int poll_sv[2] = {-1, -1};
    int null_fd = -1;
    int read_null_fd = -1;
    int invalid_poll_fd = -1;
    int poll_rc;
    llam_iovec_t single_iov;
    llam_iovec_t null_tail_iov[2];
    short revents = 0;
    ssize_t bytes;
    char one = 0;

    errno = 0;
    if (llam_read_owned(-1, 8U, NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_read_owned NULL out did not fail with EINVAL");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_read_owned(LLAM_INVALID_FD, 0U, &buffer) != 0 || buffer != NULL) {
        return test_fail("llam_read_owned zero size did not return zero with NULL buffer");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_recv_owned(LLAM_INVALID_FD, 0U, 0, &buffer) != 0 || buffer != NULL) {
        return test_fail("llam_recv_owned zero size did not return zero with NULL buffer");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_read_owned(LLAM_INVALID_FD, SIZE_MAX, &buffer) != -1 ||
        errno != EINVAL ||
        buffer != NULL) {
        return test_fail("llam_read_owned oversized count did not fail before allocation");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_recv_owned(LLAM_INVALID_FD, SIZE_MAX, 0, &buffer) != -1 ||
        errno != EINVAL ||
        buffer != NULL) {
        return test_fail("llam_recv_owned oversized count did not fail before allocation");
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    /*
     * recv-owned should preserve native recv(2) error identity for invalid
     * descriptors.  Socket probing is only a type check; it must not turn EBADF
     * into ENOTSOCK before the operation can run.
     */
    if (llam_recv_owned(LLAM_INVALID_FD, 1U, 0, &buffer) != -1 ||
        errno != EBADF ||
        buffer != NULL) {
        return test_fail("llam_recv_owned invalid fd did not preserve EBADF");
    }
    single_iov.iov_base = &one;
    single_iov.iov_len = 1U;
    errno = 0;
    if (llam_writev(LLAM_INVALID_FD, &single_iov, TEST_IOV_MAX + 1) != -1 ||
        errno != EINVAL) {
        return test_fail("llam_writev oversized iovcnt did not fail before reading iov");
    }
    null_tail_iov[0].iov_base = &one;
    null_tail_iov[0].iov_len = 1U;
    null_tail_iov[1].iov_base = NULL;
    null_tail_iov[1].iov_len = 1U;
    null_fd = open("/dev/null", O_WRONLY);
    if (null_fd < 0) {
        return test_fail_errno("direct devnull open failed");
    }
    errno = 0;
    /*
     * Outside managed task context, llam_writev promises to delegate to the
     * platform vector-write primitive. Darwin and Linux /dev/null accept a
     * NULL non-empty buffer because the kernel never reads user bytes; LLAM
     * must not reject that native success before the syscall.
     */
    if (llam_writev((llam_fd_t)null_fd, null_tail_iov, 2) != 2) {
        close_if_valid(&null_fd);
        return test_fail("unmanaged writev did not preserve native devnull semantics");
    }
    read_null_fd = open("/dev/null", O_RDONLY);
    if (read_null_fd < 0) {
        close_if_valid(&null_fd);
        return test_fail_errno("direct read devnull open failed");
    }
    errno = 0;
    /*
     * POSIX handles are documented as fd aliases. The handle wrapper must not
     * reject a NULL buffer before delegating to llam_read/llam_write, because
     * devices such as /dev/null can complete without touching user memory.
     */
    if (llam_read_handle((llam_handle_t)read_null_fd, NULL, 1U) != 0 ||
        llam_write_handle((llam_handle_t)null_fd, NULL, 1U) != 1) {
        close_if_valid(&read_null_fd);
        close_if_valid(&null_fd);
        return test_fail("unmanaged handle I/O did not preserve POSIX fd semantics");
    }
    close_if_valid(&read_null_fd);
    close_if_valid(&null_fd);

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
    if (llam_io_buffer_data(buffer) != NULL ||
        llam_io_buffer_size(buffer) != 0U ||
        llam_io_buffer_capacity(buffer) != 0U) {
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        return test_fail("released owned buffer handle remained observable");
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
    invalid_poll_fd = poll_fds[0];
    close_if_valid(&poll_fds[0]);
    revents = 0;
    poll_rc = llam_poll_fd((llam_fd_t)invalid_poll_fd, POLLIN, 0, &revents);
    if (poll_rc != 1 || (revents & POLLNVAL) == 0) {
        close_if_valid(&poll_fds[1]);
        return test_fail("invalid fd poll did not report POLLNVAL");
    }
    close_if_valid(&poll_fds[1]);

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

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, poll_sv) != 0) {
        return test_fail_errno("poll socketpair setup failed");
    }
    if (write_all_native(poll_sv[1], "y", 1U) != 0) {
        close_if_valid(&poll_sv[0]);
        close_if_valid(&poll_sv[1]);
        return test_fail_errno("poll socketpair write failed");
    }
    revents = 0;
    poll_rc = llam_poll_fd((llam_fd_t)poll_sv[0], POLLIN | POLLOUT, 0, &revents);
    if (poll_rc != 1 || (revents & POLLIN) == 0 || (revents & POLLOUT) == 0) {
        close_if_valid(&poll_sv[0]);
        close_if_valid(&poll_sv[1]);
        return test_fail("multi-event poll did not return one ready descriptor");
    }
    close_if_valid(&poll_sv[0]);
    close_if_valid(&poll_sv[1]);
    return 0;
}

#if defined(__linux__)
static int test_read_owned_invalid_fd_before_allocation(void) {
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        return test_fail_errno("invalid-fd owned-read fork failed");
    }
    if (pid == 0) {
        llam_io_buffer_t *buffer = (llam_io_buffer_t *)&status;
        struct rlimit limit;
        ssize_t bytes;

        /*
         * This child proves the invalid descriptor path does not try to reserve
         * the caller-requested owned buffer first. Before the fix,
         * llam_read_owned(LLAM_INVALID_FD, 256MiB, ...) returned ENOMEM under
         * this limit instead of the native EBADF descriptor error.
         */
        limit.rlim_cur = 128U * 1024U * 1024U;
        limit.rlim_max = 128U * 1024U * 1024U;
        if (setrlimit(RLIMIT_AS, &limit) != 0) {
            _exit(10);
        }
        errno = 0;
        bytes = llam_read_owned(LLAM_INVALID_FD, 256U * 1024U * 1024U, &buffer);
        if (bytes == -1 && errno == EBADF && buffer == NULL) {
            _exit(0);
        }
        if (bytes == -1 && errno == ENOMEM) {
            _exit(2);
        }
        _exit(3);
    }

    if (waitpid(pid, &status, 0) != pid) {
        return test_fail_errno("invalid-fd owned-read waitpid failed");
    }
    if (!WIFEXITED(status)) {
        return test_fail("invalid-fd owned-read child did not exit cleanly");
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        return 0;
    case 2:
        return test_fail("llam_read_owned invalid fd allocated before returning EBADF");
    case 10:
        return test_fail("invalid-fd owned-read RLIMIT_AS setup failed");
    default:
        return test_fail("llam_read_owned invalid fd returned an unexpected result");
    }
}
#endif

static void io_writer_task(void *arg) {
    io_state_t *state = arg;

    llam_yield();
    if (llam_write(state->stream_sv[1], "hello", 5U) != 5) {
        task_fail(state, "llam_write stream", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->writer_done, 1U, memory_order_relaxed);
}

#if defined(__linux__)
static int fill_pipe_until_eagain(int fd) {
    char bytes[4096];
    int saved_flags;

    memset(bytes, 'w', sizeof(bytes));
    saved_flags = fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0 || fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
        return -1;
    }
    for (;;) {
        ssize_t written = write(fd, bytes, sizeof(bytes));

        if (written > 0) {
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            errno = 0;
            return 0;
        }
        return -1;
    }
}

static int managed_linux_oversized_rw_edges(io_state_t *state) {
    int pipe_fds[2] = {-1, -1};
    void *huge_buf;
    size_t huge_count = ((size_t)UINT_MAX) + 1U;
    int saved_errno = 0;

    huge_buf = mmap(NULL,
                    huge_count,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                    -1,
                    0);
    if (huge_buf == MAP_FAILED) {
        task_fail(state, "linux oversized rw mmap", errno);
        return -1;
    }

    if (pipe(pipe_fds) != 0) {
        saved_errno = errno;
        (void)munmap(huge_buf, huge_count);
        errno = saved_errno;
        task_fail(state, "linux oversized read pipe", errno);
        return -1;
    }
    errno = 0;
    if (llam_read(pipe_fds[0], huge_buf, huge_count) != -1 || errno != EINVAL) {
        saved_errno = errno;
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        (void)munmap(huge_buf, huge_count);
        task_fail(state, "linux oversized read async count", saved_errno);
        return -1;
    }
    close_if_valid(&pipe_fds[0]);
    close_if_valid(&pipe_fds[1]);

    if (pipe(pipe_fds) != 0) {
        saved_errno = errno;
        (void)munmap(huge_buf, huge_count);
        errno = saved_errno;
        task_fail(state, "linux oversized write pipe", errno);
        return -1;
    }
    if (fill_pipe_until_eagain(pipe_fds[1]) != 0) {
        saved_errno = errno;
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        (void)munmap(huge_buf, huge_count);
        task_fail(state, "linux oversized write pipe fill", saved_errno);
        return -1;
    }
    errno = 0;
    if (llam_write(pipe_fds[1], huge_buf, huge_count) != -1 || errno != EINVAL) {
        saved_errno = errno;
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        (void)munmap(huge_buf, huge_count);
        task_fail(state, "linux oversized write async count", saved_errno);
        return -1;
    }
    close_if_valid(&pipe_fds[0]);
    close_if_valid(&pipe_fds[1]);
    (void)munmap(huge_buf, huge_count);
    return 0;
}
#endif

static void io_reader_task(void *arg) {
    io_state_t *state = arg;
    llam_io_buffer_t *buffer = NULL;
    llam_iovec_t huge_tail_iov[17];
    llam_iovec_t null_tail_iov[2];
    char hello[5];
    char one = 'x';
    short revents = 0;
    ssize_t bytes;

#if defined(__linux__)
    if (managed_linux_oversized_rw_edges(state) != 0) {
        return;
    }
#endif

    /*
     * LLAM_INVALID_FD is a public sentinel, not an ignored poll-array slot.
     * Managed direct polling must report it as POLLNVAL instead of converting
     * native poll(-1) semantics into a false zero-timeout.
     */
    errno = 0;
    if (llam_poll_fd(LLAM_INVALID_FD, POLLIN, 0, &revents) != 1 ||
        (revents & POLLNVAL) == 0) {
        task_fail(state, "managed LLAM_INVALID_FD poll", errno);
        return;
    }
    /*
     * Owned-buffer zero-byte operations are API-level no-ops. They must not
     * allocate, inspect the descriptor, or fail inside managed task context.
     */
    buffer = (llam_io_buffer_t *)&buffer;
    if (llam_read_owned(LLAM_INVALID_FD, 0U, &buffer) != 0 || buffer != NULL) {
        task_fail(state, "managed zero-size read_owned", errno);
        return;
    }
    buffer = (llam_io_buffer_t *)&buffer;
    if (llam_recv_owned(LLAM_INVALID_FD, 0U, 0, &buffer) != 0 || buffer != NULL) {
        task_fail(state, "managed zero-size recv_owned", errno);
        return;
    }
    buffer = (llam_io_buffer_t *)&buffer;
    errno = 0;
    if (llam_recv_owned(LLAM_INVALID_FD, 1U, 0, &buffer) != -1 ||
        errno != EBADF ||
        buffer != NULL) {
        task_fail(state, "managed recv_owned invalid fd errno", errno);
        return;
    }
    null_tail_iov[0].iov_base = &one;
    null_tail_iov[0].iov_len = 1U;
    null_tail_iov[1].iov_base = NULL;
    null_tail_iov[1].iov_len = 1U;
    errno = 0;
    /*
     * The native direct writev fast path should make the same decision as the
     * kernel for descriptors such as /dev/null. Fallback-only validation is
     * still tested below with an iovec count that bypasses the direct path.
     */
    if (llam_writev((llam_fd_t)state->null_fd, null_tail_iov, 2) != 2) {
        task_fail(state, "managed direct writev devnull null tail", errno);
        return;
    }
    errno = 0;
    if (llam_read_handle((llam_handle_t)state->read_null_fd, NULL, 1U) != 0 ||
        llam_write_handle((llam_handle_t)state->null_fd, NULL, 1U) != 1) {
        task_fail(state, "managed handle I/O POSIX alias semantics", errno);
        return;
    }
    /*
     * Use many moderate lengths, not one huge tail. Linux may legally validate
     * each /dev/null slice lazily and report EFAULT for an oversized single
     * buffer before the fallback aggregate-limit path is exercised.
     */
    for (int i = 0; i < 17; ++i) {
        huge_tail_iov[i].iov_base = &one;
        huge_tail_iov[i].iov_len = ((size_t)SSIZE_MAX / 16U) + 1U;
    }
    errno = 0;
    /*
     * The managed writev fallback handles iovcnt > 16 by writing slices one at
     * a time. It still must reject an aggregate length that native writev would
     * reject before any slice is written; otherwise an invalid tail can be
     * hidden behind a partial success.
     */
    if (llam_writev(state->null_fd, huge_tail_iov, 17) != -1 || errno != EINVAL) {
        task_fail(state, "managed writev huge aggregate length", errno);
        return;
    }

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
    state.null_fd = -1;
    state.read_null_fd = -1;
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
    state.null_fd = open("/dev/null", O_WRONLY);
    if (state.null_fd < 0) {
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        return test_fail_errno("devnull open failed");
    }
    state.read_null_fd = open("/dev/null", O_RDONLY);
    if (state.read_null_fd < 0) {
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        close_if_valid(&state.null_fd);
        return test_fail_errno("read devnull open failed");
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
        close_if_valid(&state.null_fd);
        close_if_valid(&state.read_null_fd);
        return test_fail_errno("llam_runtime_init failed");
    }
    if (llam_spawn(io_reader_task, &state, NULL) == NULL ||
        llam_spawn(io_writer_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        close_if_valid(&state.null_fd);
        close_if_valid(&state.read_null_fd);
        return test_fail_errno("llam_spawn failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        close_if_valid(&state.stream_sv[0]);
        close_if_valid(&state.stream_sv[1]);
        close_if_valid(&state.peek_sv[0]);
        close_if_valid(&state.peek_sv[1]);
        close_if_valid(&state.null_fd);
        close_if_valid(&state.read_null_fd);
        return test_fail_errno("llam_run failed");
    }
    llam_runtime_shutdown();

    close_if_valid(&state.stream_sv[0]);
    close_if_valid(&state.stream_sv[1]);
    close_if_valid(&state.peek_sv[0]);
    close_if_valid(&state.peek_sv[1]);
    close_if_valid(&state.null_fd);
    close_if_valid(&state.read_null_fd);

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

static void escaped_owned_reader_task(void *arg) {
    escaped_owned_state_t *state = arg;
    ssize_t bytes;

    bytes = llam_read_owned(state->pipe_fds[0], 16U, &state->buffer);
    if (bytes != 4 ||
        state->buffer == NULL ||
        llam_io_buffer_size(state->buffer) != 4U ||
        memcmp(llam_io_buffer_data(state->buffer), "hold", 4U) != 0) {
        llam_io_buffer_release(state->buffer);
        state->buffer = NULL;
        escaped_owned_task_fail(state);
    }
}

static int test_owned_buffer_release_after_shutdown(void) {
    escaped_owned_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    state.pipe_fds[0] = -1;
    state.pipe_fds[1] = -1;
    atomic_init(&state.failures, 0U);

    if (pipe(state.pipe_fds) != 0) {
        return test_fail_errno("escaped owned pipe setup failed");
    }
    if (write_all_native(state.pipe_fds[1], "hold", 4U) != 0) {
        close_if_valid(&state.pipe_fds[0]);
        close_if_valid(&state.pipe_fds[1]);
        return test_fail_errno("escaped owned pipe write failed");
    }
    close_if_valid(&state.pipe_fds[1]);

    if (llam_runtime_init(NULL) != 0) {
        close_if_valid(&state.pipe_fds[0]);
        return test_fail_errno("escaped owned runtime init failed");
    }
    task = llam_spawn(escaped_owned_reader_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        close_if_valid(&state.pipe_fds[0]);
        return test_fail_errno("escaped owned spawn failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        close_if_valid(&state.pipe_fds[0]);
        llam_io_buffer_release(state.buffer);
        return test_fail_errno("escaped owned runtime run failed");
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        close_if_valid(&state.pipe_fds[0]);
        llam_io_buffer_release(state.buffer);
        return test_fail_errno("escaped owned task join failed");
    }
    close_if_valid(&state.pipe_fds[0]);
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U || state.buffer == NULL) {
        llam_runtime_shutdown();
        llam_io_buffer_release(state.buffer);
        return test_fail("escaped owned task did not produce a buffer");
    }

    /*
     * The public owned-buffer handle is caller-owned after a successful read.
     * Releasing it after shutdown must not touch freed runtime allocator slabs.
     */
    llam_runtime_shutdown();
    if (llam_io_buffer_size(state.buffer) != 4U ||
        memcmp(llam_io_buffer_data(state.buffer), "hold", 4U) != 0) {
        llam_io_buffer_release(state.buffer);
        return test_fail("escaped owned buffer payload changed after shutdown");
    }
    llam_io_buffer_release(state.buffer);
    if (llam_io_buffer_data(state.buffer) != NULL ||
        llam_io_buffer_size(state.buffer) != 0U ||
        llam_io_buffer_capacity(state.buffer) != 0U) {
        return test_fail("released escaped owned buffer handle remained observable");
    }
    llam_io_buffer_release(state.buffer);
    return 0;
}

static int test_owned_buffer_accessor_release_race(void) {
    owned_buffer_race_state_t state;
    pthread_t threads[OWNED_BUFFER_RACE_THREADS];
    unsigned started = 0U;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.current, (uintptr_t)NULL);
    atomic_init(&state.ready, 0U);
    atomic_init(&state.stop, 0U);

    for (unsigned i = 0U; i < OWNED_BUFFER_RACE_THREADS; ++i) {
        if (pthread_create(&threads[i], NULL, owned_buffer_accessor_race_thread, &state) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            for (unsigned j = 0U; j < started; ++j) {
                (void)pthread_join(threads[j], NULL);
            }
            return test_fail_errno("owned-buffer race pthread_create failed");
        }
        started += 1U;
    }
    while (atomic_load_explicit(&state.ready, memory_order_acquire) != OWNED_BUFFER_RACE_THREADS) {
        (void)sched_yield();
    }

    for (unsigned i = 0U; i < OWNED_BUFFER_RACE_ITERS; ++i) {
        llam_io_buffer_t *buffer = NULL;
        int pipe_fds[2] = {-1, -1};
        ssize_t bytes;

        if (pipe(pipe_fds) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            for (unsigned j = 0U; j < started; ++j) {
                (void)pthread_join(threads[j], NULL);
            }
            return test_fail_errno("owned-buffer race pipe setup failed");
        }
        if (write_all_native(pipe_fds[1], "race", 4U) != 0) {
            close_if_valid(&pipe_fds[0]);
            close_if_valid(&pipe_fds[1]);
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            for (unsigned j = 0U; j < started; ++j) {
                (void)pthread_join(threads[j], NULL);
            }
            return test_fail_errno("owned-buffer race pipe write failed");
        }
        close_if_valid(&pipe_fds[1]);
        bytes = llam_read_owned(pipe_fds[0], 8U, &buffer);
        close_if_valid(&pipe_fds[0]);
        if (bytes != 4 || buffer == NULL) {
            llam_io_buffer_release(buffer);
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            for (unsigned j = 0U; j < started; ++j) {
                (void)pthread_join(threads[j], NULL);
            }
            return test_fail("owned-buffer race read_owned failed");
        }

        atomic_store_explicit(&state.current, (uintptr_t)buffer, memory_order_release);
        for (unsigned spin = 0U; spin < 16U; ++spin) {
            (void)sched_yield();
        }
        llam_io_buffer_release(buffer);
        atomic_store_explicit(&state.current, (uintptr_t)NULL, memory_order_release);
    }

    atomic_store_explicit(&state.stop, 1U, memory_order_release);
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            return test_fail_errno("owned-buffer race pthread_join failed");
        }
    }
    return 0;
}

static int make_native_owned_buffer(const char *payload, llam_io_buffer_t **out) {
    int pipe_fds[2] = {-1, -1};
    ssize_t bytes;

    *out = NULL;
    if (pipe(pipe_fds) != 0) {
        return -1;
    }
    if (write_all_native(pipe_fds[1], payload, strlen(payload)) != 0) {
        close_if_valid(&pipe_fds[0]);
        close_if_valid(&pipe_fds[1]);
        return -1;
    }
    close_if_valid(&pipe_fds[1]);
    bytes = llam_read_owned(pipe_fds[0], 32U, out);
    close_if_valid(&pipe_fds[0]);
    if (bytes != (ssize_t)strlen(payload) || *out == NULL) {
        llam_io_buffer_release(*out);
        *out = NULL;
        errno = EIO;
        return -1;
    }
    return 0;
}

static int test_owned_buffer_stale_release_reuse_guard(void) {
    llam_io_buffer_t *old_buffer = NULL;
    llam_io_buffer_t *new_buffer = NULL;
    uintptr_t old_slot;

    for (unsigned i = 0U; i < 20U; ++i) {
        llam_io_buffer_t *warm_buffer = NULL;

        if (make_native_owned_buffer("warm", &warm_buffer) != 0) {
            return test_fail_errno("owned-buffer stale-release warmup failed");
        }
        llam_io_buffer_release(warm_buffer);
    }

    if (make_native_owned_buffer("old", &old_buffer) != 0) {
        return test_fail_errno("owned-buffer stale-release old allocation failed");
    }
    old_slot = owned_buffer_slot_handle(old_buffer);
    llam_io_buffer_release(old_buffer);

    for (unsigned i = 0U; i < 256U; ++i) {
        if (make_native_owned_buffer("new", &new_buffer) != 0) {
            return test_fail_errno("owned-buffer stale-release new allocation failed");
        }
        if (owned_buffer_slot_handle(new_buffer) == old_slot) {
            /*
             * Release consumes the public buffer handle.  If the allocator
             * immediately reuses the same public handle slot for a new buffer,
             * the stale handle generation must not release or hide it.
             */
            llam_io_buffer_release(old_buffer);
            if (llam_io_buffer_size(new_buffer) != 3U ||
                memcmp(llam_io_buffer_data(new_buffer), "new", 3U) != 0) {
                llam_io_buffer_release(new_buffer);
                return test_fail("stale owned-buffer release consumed a reused buffer");
            }
            llam_io_buffer_release(new_buffer);
            return 0;
        }
        llam_io_buffer_release(new_buffer);
        new_buffer = NULL;
    }

    return 0;
}

static llam_io_buffer_t *allocate_internal_test_buffer(const char *payload, llam_io_buffer_t **out_handle) {
    llam_io_buffer_t *buffer;
    size_t len = strlen(payload);

    *out_handle = NULL;
    buffer = calloc(1U, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }
    buffer->detached_wrapper = true;
    buffer->capacity = sizeof(buffer->inline_data);
    buffer->size = len <= buffer->capacity ? len : buffer->capacity;
    buffer->data = buffer->inline_data;
    memcpy(buffer->inline_data, payload, buffer->size);
    if (llam_io_buffer_public_register(buffer) != 0) {
        free(buffer);
        return NULL;
    }
    *out_handle = llam_io_buffer_public_handle(buffer);
    if (*out_handle == NULL) {
        llam_io_buffer_release_raw(buffer);
        errno = EINVAL;
        return NULL;
    }
    return buffer;
}

static int test_owned_buffer_raw_release_decodable_pointer_guard(void) {
    llam_io_buffer_t *target = NULL;
    llam_io_buffer_t *target_handle = NULL;
    llam_io_buffer_t **dummy_handles = NULL;
    size_t raw_decoded_slot;
    size_t dummy_count;
    int rc = 0;

    target = allocate_internal_test_buffer("raw!", &target_handle);
    if (target == NULL) {
        return test_fail_errno("owned-buffer raw-release target allocation failed");
    }

    raw_decoded_slot = owned_buffer_slot_handle(target);
    if (raw_decoded_slot >= OWNED_BUFFER_RAW_RELEASE_MAX_COLLISION_SLOTS) {
        /*
         * Some allocators map heap objects high enough that reproducing the old
         * collision would make the test too expensive. The production guard is
         * still exercised on common compact mappings where raw pointer high bits
         * fall into the public slot table range.
         */
        llam_io_buffer_release(target_handle);
        return 0;
    }

    dummy_count = raw_decoded_slot + 1U;
    dummy_handles = calloc(dummy_count, sizeof(*dummy_handles));
    if (dummy_handles == NULL) {
        llam_io_buffer_release(target_handle);
        return test_fail_errno("owned-buffer raw-release dummy handle allocation failed");
    }

    for (size_t i = 0U; i < dummy_count; ++i) {
        llam_io_buffer_t *dummy = allocate_internal_test_buffer("x", &dummy_handles[i]);

        if (dummy == NULL) {
            rc = test_fail_errno("owned-buffer raw-release dummy allocation failed");
            dummy_count = i;
            goto cleanup;
        }
        (void)dummy;
    }

    /*
     * The raw wrapper address now decodes to an in-range public slot. Internal
     * raw release must still prefer live-list membership; otherwise setup/error
     * cleanup can silently leak a live wrapper when the registry has grown.
     */
    llam_io_buffer_release_raw(target);
    if (llam_io_buffer_size(target_handle) != 0U) {
        rc = test_fail("raw owned-buffer release was hidden by public-handle decode");
        llam_io_buffer_release(target_handle);
    }
    target = NULL;
    target_handle = NULL;

cleanup:
    for (size_t i = 0U; i < dummy_count; ++i) {
        llam_io_buffer_release(dummy_handles[i]);
    }
    free(dummy_handles);
    if (target_handle != NULL) {
        llam_io_buffer_release(target_handle);
    }
    return rc;
}

int main(void) {
    if (test_direct_owned_read_and_poll() != 0 ||
#if defined(__linux__)
        test_read_owned_invalid_fd_before_allocation() != 0 ||
#endif
        test_managed_io_paths() != 0 ||
        test_owned_buffer_release_after_shutdown() != 0 ||
        test_owned_buffer_accessor_release_race() != 0 ||
        test_owned_buffer_stale_release_reuse_guard() != 0 ||
        test_owned_buffer_raw_release_decodable_pointer_guard() != 0) {
        return 1;
    }
    printf("[test_io_buffers] ok\n");
    return 0;
}
