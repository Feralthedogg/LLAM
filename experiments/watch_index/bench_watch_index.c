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
#include <inttypes.h>
#if LLAM_PLATFORM_POSIX
#include <sys/resource.h>
#include <sched.h>

typedef struct state state_t;
typedef struct connection {
    state_t *state;
    int fd[2];
    llam_task_t *task;
    llam_poll_watch_t *watch;
} connection_t;
struct state {
    llam_runtime_t *runtime;
    connection_t *connections;
    unsigned count;
    unsigned rounds;
    uint64_t *latencies;
    uint64_t elapsed;
    unsigned initial_watches;
};
static void die(const char *message) {
    fprintf(stderr, "bench_watch_index: %s (errno=%d)\n", message, errno);
    exit(1);
}
static unsigned number(const char *text, unsigned max) {
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *text < '0' || *text > '9' || *end || value == 0 || value > max) {
        die("invalid numeric argument");
    }
    return (unsigned)value;
}
static void server(void *arg) {
    connection_t *connection = arg;
    for (unsigned round = 0; round < connection->state->rounds; ++round) {
        short events = 0;
        unsigned char byte = 0;
        if (llam_poll_fd(connection->fd[0], POLLIN, -1, &events) != 1 || !(events & POLLIN) ||
            llam_read(connection->fd[0], &byte, 1) != 1 || byte != round % 251U ||
            llam_write(connection->fd[0], &byte, 1) != 1) {
            die("poll/read/echo or exact delivery failed");
        }
    }
}
static void *client(void *arg) {
    state_t *state = arg;
    llam_node_t *node = &state->runtime->nodes[0];
    uint64_t deadline = llam_now_ns() + UINT64_C(10000000000);
    for (;;) {
        unsigned count = 0;
        pthread_mutex_lock(&node->watch_lock);
        for (llam_poll_watch_t *watch = node->poll_watches; watch; watch = watch->next) {
            if (watch->wait_head != NULL)
                ++count;
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (count == state->count) {
            state->initial_watches = count;
            break;
        }
        if (llam_now_ns() >= deadline)
            die("not all real poll waits became active");
        sched_yield();
    }
    uint64_t begin = llam_now_ns();
    size_t sample = 0;
    for (unsigned round = 0; round < state->rounds; ++round) {
        /* Coprime odd stride visits every fd for the power-of-two connection counts. */
        for (unsigned step = 0; step < state->count; ++step) {
            unsigned i = (step * 4051U + round) % state->count;
            int fd = state->connections[i].fd[1];
            unsigned char sent = (unsigned char)(round % 251U), received = 0;
            struct pollfd wait = {fd, POLLIN, 0};
            uint64_t start = llam_now_ns();
            if (write(fd, &sent, 1) != 1 || poll(&wait, 1, 5000) != 1 || read(fd, &received, 1) != 1 ||
                received != sent)
                die("client exact echo failed");
            state->latencies[sample++] = llam_now_ns() - start;
        }
    }
    state->elapsed = llam_now_ns() - begin;
    return NULL;
}
static int compare(const void *lhs, const void *rhs) {
    uint64_t a = *(const uint64_t *)lhs, b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}
int main(int argc, char **argv) {
    state_t state = {0};
    llam_runtime_opts_t options;
    struct rlimit limit;
    bool io;
    if (argc != 4 || (strcmp(argv[1], "lookup") && strcmp(argv[1], "io"))) {
        fprintf(stderr, "usage: %s lookup|io connections operations\n", argv[0]);
        return 2;
    }
    io = strcmp(argv[1], "io") == 0;
    state.count = number(argv[2], 4096);
    if ((state.count & (state.count - 1U)) != 0U)
        die("connections must be a power of two");
    unsigned requested = number(argv[3], 1048576);
    state.rounds = (requested + state.count - 1U) / state.count;
    size_t operations = (size_t)state.rounds * state.count;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        die("getrlimit");
    rlim_t needed = (rlim_t)state.count * 2U + 256U;
    if (limit.rlim_cur < needed) {
        if (needed > limit.rlim_max)
            die("fd hard limit too low");
        limit.rlim_cur = needed;
        if (setrlimit(RLIMIT_NOFILE, &limit))
            die("setrlimit in benchmark process");
    }
    state.connections = calloc(state.count, sizeof(*state.connections));
    state.latencies = calloc(operations, sizeof(*state.latencies));
    if (!state.connections || !state.latencies)
        die("allocation");
    if (llam_runtime_opts_init(&options, sizeof(options)))
        die("options");
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_create_compat(&options, sizeof(options), &state.runtime))
        die("runtime");
    if (state.runtime->active_nodes != 1U || !state.runtime->nodes[0].supports_multishot_poll) {
        die("benchmark requires one native multishot poll node");
    }
    for (unsigned i = 0; i < state.count; ++i) {
        connection_t *connection = &state.connections[i];
        connection->state = &state;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, connection->fd))
            die("socketpair");
        int flags = fcntl(connection->fd[0], F_GETFL, 0);
        if (flags == -1 || fcntl(connection->fd[0], F_SETFL, flags | O_NONBLOCK))
            die("nonblocking fd");
    }
    if (io) {
        pthread_t thread;
        for (unsigned i = 0; i < state.count; ++i) {
            state.connections[i].task =
                llam_runtime_spawn_ex(state.runtime, server, &state.connections[i], NULL, 0);
            if (!state.connections[i].task)
                die("spawn");
        }
        if (pthread_create(&thread, NULL, client, &state))
            die("client thread");
        if (llam_runtime_run_handle(state.runtime))
            die("runtime run");
        if (pthread_join(thread, NULL))
            die("client join");
        for (unsigned i = 0; i < state.count; ++i) {
            if (llam_join(state.connections[i].task))
                die("task join");
        }
    } else {
        llam_node_t *node = &state.runtime->nodes[0];
        pthread_mutex_lock(&node->watch_lock);
        for (unsigned i = 0; i < state.count; ++i) {
            state.connections[i].watch =
                llam_get_or_create_poll_watch_locked(node, state.connections[i].fd[0], POLLIN);
            if (!state.connections[i].watch)
                die("watch setup");
        }
        pthread_mutex_unlock(&node->watch_lock);
        uint64_t begin = llam_now_ns();
        for (size_t step = 0; step < operations; ++step) {
            unsigned i = (unsigned)((step * 4051U + step / state.count) % state.count);
            uint64_t start = llam_now_ns();
            pthread_mutex_lock(&node->watch_lock);
            llam_poll_watch_t *watch =
                llam_get_or_create_poll_watch_locked(node, state.connections[i].fd[0], POLLIN);
            pthread_mutex_unlock(&node->watch_lock);
            if (watch != state.connections[i].watch)
                die("watch identity mismatch");
            state.latencies[step] = llam_now_ns() - start;
        }
        state.elapsed = llam_now_ns() - begin;
        state.initial_watches = state.count;
    }
    qsort(state.latencies, operations, sizeof(*state.latencies), compare);
    printf("{\"mode\":\"%s\",\"connections\":%u,\"operations\":%zu,\"workers\":%u,"
           "\"initial_watches\":%u,\"elapsed_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64 ",\"latencies_ns\":[",
           argv[1], state.count, operations, state.runtime->active_shards, state.initial_watches,
           state.elapsed, state.latencies[(operations * 99U + 99U) / 100U - 1U]);
    for (size_t i = 0; i < operations; ++i)
        printf("%s%" PRIu64, i ? "," : "", state.latencies[i]);
    puts("]}");
    llam_runtime_destroy(state.runtime);
    for (unsigned i = 0; i < state.count; ++i) {
        close(state.connections[i].fd[0]);
        close(state.connections[i].fd[1]);
    }
    free(state.connections);
    free(state.latencies);
    return 0;
}
#else
int main(void) {
    return 77;
}
#endif
