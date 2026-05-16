/**
 * @file tests/test_runtime_io_dump.c
 * @brief Runtime dump coverage for live I/O wait ownership.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_PLATFORM_POSIX
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#endif

#if LLAM_PLATFORM_POSIX
typedef struct io_dump_state {
    int read_fd;
    int write_fd;
    atomic_uint reader_entered;
    atomic_uint dump_seen;
    atomic_uint failures;
    char first_failure[160];
} io_dump_state_t;

static int fail_now(const char *message) {
    fprintf(stderr, "[test_runtime_io_dump] %s\n", message);
    return 1;
}

static void task_fail(io_dump_state_t *state, const char *message) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        (void)snprintf(state->first_failure, sizeof(state->first_failure), "%s", message);
    }
}

static void wake_reader_after_setup_failure(io_dump_state_t *state) {
    ssize_t written;

    /*
     * The fallback path still has to drive the parked poll task to completion.
     * Check write(2) explicitly so Linux CI with warn_unused_result keeps this
     * failure path covered instead of treating it as a discarded diagnostic.
     */
    do {
        written = write(state->write_fd, "x", 1U);
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
        task_fail(state, "failed to wake reader after setup failure");
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static char *capture_runtime_dump(void) {
    char path[] = "/tmp/llam-io-dump-test.XXXXXX";
    char *buffer = NULL;
    ssize_t nread;
    off_t end;
    int fd;

    fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }
    (void)unlink(path);

    llam_dump_runtime_state(fd);
    end = lseek(fd, 0, SEEK_END);
    if (end < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        (void)close(fd);
        return NULL;
    }

    buffer = calloc((size_t)end + 1U, 1U);
    if (buffer == NULL) {
        (void)close(fd);
        return NULL;
    }
    nread = read(fd, buffer, (size_t)end);
    (void)close(fd);
    if (nread < 0) {
        free(buffer);
        return NULL;
    }
    buffer[nread] = '\0';
    return buffer;
}

static bool dump_contains_pending_io_or_blocking_fallback(const char *dump, int read_fd) {
    char fd_pattern[32];

    (void)snprintf(fd_pattern, sizeof(fd_pattern), "fd=%d", read_fd);
    if (strstr(dump, "wait_owner=io_req") != NULL &&
        strstr(dump, "kind=poll") != NULL &&
        strstr(dump, fd_pattern) != NULL &&
        strstr(dump, "wait_mode=") != NULL &&
        strstr(dump, "active_io_waiters=") != NULL) {
        return true;
    }

    return strstr(dump, "wait_owner=blocking_job") != NULL &&
           strstr(dump, "block_job=") != NULL &&
           strstr(dump, "io_fallbacks=") != NULL;
}

static void *dump_thread_main(void *arg) {
    io_dump_state_t *state = arg;
    unsigned i;

    for (i = 0U; i < 200U; ++i) {
        if (atomic_load_explicit(&state->reader_entered, memory_order_acquire) != 0U) {
            break;
        }
        usleep(1000U);
    }

    for (i = 0U; i < 250U; ++i) {
        char *dump = capture_runtime_dump();

        if (dump != NULL) {
            if (dump_contains_pending_io_or_blocking_fallback(dump, state->read_fd)) {
                atomic_store_explicit(&state->dump_seen, 1U, memory_order_release);
                free(dump);
                break;
            }
            free(dump);
        }
        usleep(5000U);
    }

    if (atomic_load_explicit(&state->dump_seen, memory_order_acquire) == 0U) {
        char *dump = capture_runtime_dump();

        if (dump != NULL && getenv("LLAM_TEST_IO_DUMP_DEBUG") != NULL) {
            fprintf(stderr, "[test_runtime_io_dump] final dump:\n%s\n", dump);
        }
        free(dump);
        task_fail(state, "runtime dump did not expose pending I/O ownership");
    }
    if (write(state->write_fd, "x", 1U) != 1) {
        task_fail(state, "failed to release blocked read task");
    }
    return NULL;
}

static void pending_poll_task(void *arg) {
    io_dump_state_t *state = arg;
    short revents = 0;
    int rc;

    atomic_store_explicit(&state->reader_entered, 1U, memory_order_release);
    rc = llam_poll_fd((llam_fd_t)state->read_fd, POLLIN, 3000, &revents);
    if (rc <= 0 || (revents & POLLIN) == 0) {
        task_fail(state, "llam_poll_fd did not complete after dump thread release");
    }
}

int main(void) {
    io_dump_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *reader;
    pthread_t dump_thread;
    int fds[2];
    int thread_started = 0;
    int result = 1;

    memset(&state, 0, sizeof(state));
    state.read_fd = -1;
    state.write_fd = -1;

    if (pipe(fds) != 0) {
        return fail_now("pipe failed");
    }
    state.read_fd = fds[0];
    state.write_fd = fds[1];

    if (set_nonblocking(state.read_fd) != 0) {
        (void)close(state.read_fd);
        (void)close(state.write_fd);
        return fail_now("failed to make read fd nonblocking");
    }

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        (void)close(state.read_fd);
        (void)close(state.write_fd);
        return fail_now("runtime init failed");
    }

    reader = llam_spawn(pending_poll_task, &state, NULL);
    if (reader == NULL) {
        llam_runtime_shutdown();
        (void)close(state.read_fd);
        (void)close(state.write_fd);
        return fail_now("reader spawn failed");
    }

    if (pthread_create(&dump_thread, NULL, dump_thread_main, &state) == 0) {
        thread_started = 1;
    } else {
        task_fail(&state, "pthread_create failed");
        wake_reader_after_setup_failure(&state);
    }

    if (llam_run() != 0) {
        task_fail(&state, "llam_run failed");
    }
    if (llam_join(reader) != 0) {
        task_fail(&state, "reader join failed");
    }
    if (thread_started != 0 && pthread_join(dump_thread, NULL) != 0) {
        task_fail(&state, "pthread_join failed");
    }

    if (atomic_load_explicit(&state.failures, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state.dump_seen, memory_order_acquire) != 0U) {
        result = 0;
    } else if (state.first_failure[0] != '\0') {
        fprintf(stderr, "[test_runtime_io_dump] %s\n", state.first_failure);
    }

    llam_runtime_shutdown();
    (void)close(state.read_fd);
    (void)close(state.write_fd);
    return result;
}
#else
int main(void) {
    puts("test_runtime_io_dump: skipped on non-POSIX platform");
    return 0;
}
#endif
