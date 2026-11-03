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
#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_watch_linux_internal.h"
#elif LLAM_RUNTIME_BACKEND_KQUEUE
#include "io/darwin/runtime_io_watch_darwin_internal.h"
#endif
#if LLAM_PLATFORM_POSIX
#include <sched.h>
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "watch index check failed: %s at %d (errno=%d)\n", #condition, __LINE__, errno); \
            abort();                                                                                         \
        }                                                                                                    \
    } while (0)

typedef struct race {
    llam_runtime_t runtime;
    llam_node_t nodes[2];
    llam_poll_watch_t *source_watch;
    atomic_uint ready;
    int fd;
} race_t;

static void setup(race_t *state) {
    memset(state, 0, sizeof(*state));
    state->runtime.nodes = state->nodes;
    state->runtime.active_nodes = 2U;
    atomic_init(&state->ready, 0U);
    for (unsigned i = 0; i < 2; ++i) {
        state->nodes[i].runtime = &state->runtime;
        state->nodes[i].index = i;
        CHECK(pthread_mutex_init(&state->nodes[i].watch_lock, NULL) == 0);
        state->nodes[i].watch_lock_initialized = true;
    }
}
static void teardown(race_t *state) {
    for (unsigned i = 0; i < 2; ++i) {
        llam_node_t *node = &state->nodes[i];
        CHECK(node->poll_watches == NULL && node->accept_watches == NULL && node->recv_watches == NULL);
        CHECK(node->poll_watch_index.count == 0 && node->poll_watch_index.buckets == NULL);
        CHECK(node->accept_watch_index.count == 0 && node->accept_watch_index.buckets == NULL);
        CHECK(node->recv_watch_index.count == 0 && node->recv_watch_index.buckets == NULL);
        CHECK(pthread_mutex_destroy(&node->watch_lock) == 0);
    }
}
static void exercise_tables(void) {
    race_t *state = calloc(1, sizeof(*state));
    int fds[48][2];
    llam_poll_watch_t *polls[48];
    llam_accept_watch_t *accepts[48];
    llam_recv_watch_t *recvs[48];
    CHECK(state != NULL);
    setup(state);
    llam_node_t *node = &state->nodes[0];
    pthread_mutex_lock(&node->watch_lock);
    for (unsigned i = 0; i < 48; ++i) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
        if (i == 0)
            llam_watch_index_test_fail_next_allocation();
        errno = E2BIG;
        polls[i] = llam_get_or_create_poll_watch_locked(node, fds[i][0], POLLIN);
        CHECK(polls[i] != NULL && errno == E2BIG);
        if (i == 16)
            llam_watch_index_test_fail_next_allocation();
        accepts[i] = llam_get_or_create_accept_watch_locked(node, fds[i][0]);
        CHECK(accepts[i] != NULL);
        if (i == 16)
            CHECK(node->accept_watch_index.capacity == 16);
        recvs[i] = llam_get_or_create_recv_watch_locked(node, fds[i][0]);
        CHECK(recvs[i] != NULL);
        for (unsigned j = 0; j <= i; ++j) {
            CHECK(llam_find_poll_watch_locked(node, fds[j][0], POLLIN) == polls[j]);
            CHECK(llam_find_accept_watch_locked(node, fds[j][0]) == accepts[j]);
            CHECK(llam_find_recv_watch_locked(node, fds[j][0], recvs[j]->st_dev, recvs[j]->st_ino) ==
                  recvs[j]);
            CHECK(llam_find_poll_watch_locked(node, fds[j][0], POLLOUT) == NULL);
            CHECK(llam_find_recv_watch_locked(node, fds[j][0], recvs[j]->st_dev, recvs[j]->st_ino ^ 1U) ==
                  NULL);
        }
    }
    CHECK(node->poll_watch_index.disabled && node->poll_watch_index.capacity == 0);
    CHECK(node->accept_watch_index.capacity >= 48 && node->recv_watch_index.capacity >= 48);
    /* Remove in a non-list order to exercise index bucket predecessor handling. */
    for (unsigned step = 0; step < 48; ++step) {
        unsigned i = step * 13U % 48U;
        llam_destroy_poll_watch_locked(node, polls[i]);
        llam_destroy_accept_watch_locked(node, accepts[i]);
        llam_destroy_recv_watch_locked(node, recvs[i]);
        CHECK(llam_find_poll_watch_locked(node, fds[i][0], POLLIN) == NULL);
        CHECK(llam_find_accept_watch_locked(node, fds[i][0]) == NULL);
        CHECK(llam_find_recv_watch_locked(node, fds[i][0], 0, 0) == NULL);
    }
    CHECK(!node->poll_watch_index.disabled);
    llam_poll_watch_t *fresh = llam_get_or_create_poll_watch_locked(node, fds[0][0], POLLIN);
    CHECK(fresh != NULL && node->poll_watch_index.capacity == 16);
    llam_destroy_poll_watch_locked(node, fresh);
    pthread_mutex_unlock(&node->watch_lock);
    for (unsigned i = 0; i < 48; ++i) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    teardown(state);
    free(state);
}
static void migrate(race_t *state) {
    bool kick = false;
    (void)llam_finalize_poll_watch_migration(&state->nodes[0], state->source_watch, 1, &kick);
    CHECK(!kick); /* Sticky readiness transfers without arming a detached backend. */
}
static void close_watch(race_t *state) {
    llam_fd_watch_lifecycle_lock();
    CHECK(llam_forget_closed_fd_watch_state(&state->runtime, state->fd) == 0);
    CHECK(close(state->fd) == 0);
    llam_fd_watch_lifecycle_unlock();
}
static void await_peer(race_t *state) {
    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(&state->ready, memory_order_acquire) != 2U)
        sched_yield();
}
static void *migration_thread(void *arg) {
    await_peer(arg);
    migrate(arg);
    return NULL;
}
static void *close_thread(void *arg) {
    await_peer(arg);
    close_watch(arg);
    return NULL;
}
static void exercise_close_migrate_reuse(unsigned order) {
    race_t *state = calloc(1, sizeof(*state));
    int old[2], replacement[2];
    CHECK(state != NULL);
    setup(state);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, old) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    state->fd = old[0];
    llam_node_t *source = &state->nodes[0];
    pthread_mutex_lock(&source->watch_lock);
    state->source_watch = llam_get_or_create_poll_watch_locked(source, old[0], POLLIN);
    CHECK(state->source_watch != NULL);
    state->source_watch->sticky_revents = POLLIN;
    state->source_watch->migrate_target_node_index = 1U;
    llam_poll_watch_pin_locked(state->source_watch);
    pthread_mutex_unlock(&source->watch_lock);
    if (order == 0) {
        close_watch(state);
        migrate(state);
    } else if (order == 1) {
        migrate(state);
        close_watch(state);
    } else {
        pthread_t a, b;
        CHECK(pthread_create(&a, NULL, migration_thread, state) == 0);
        CHECK(pthread_create(&b, NULL, close_thread, state) == 0);
        CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    }
    CHECK(dup2(replacement[0], state->fd) == state->fd);
    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&source->watch_lock);
    CHECK(state->source_watch->destroy_pending && !state->source_watch->accepts_waiters);
    CHECK(llam_find_poll_watch_locked(source, state->fd, POLLIN) == NULL);
    llam_poll_watch_t *fresh = llam_get_or_create_poll_watch_locked(source, state->fd, POLLIN);
    CHECK(fresh != NULL && fresh != state->source_watch && fresh->sticky_revents == 0);
    CHECK(llam_find_poll_watch_locked(source, state->fd, POLLIN) == fresh);
    pthread_mutex_unlock(&source->watch_lock);
    llam_fd_watch_lifecycle_unlock();
    /* A late migration event for the consumed generation cannot forward via the new watch. */
    CHECK(!llam_forward_live_poll_watch_event(source, state->fd, POLLIN, 1U, POLLIN));
    pthread_mutex_lock(&state->nodes[1].watch_lock);
    CHECK(llam_find_poll_watch_locked(&state->nodes[1], state->fd, POLLIN) == NULL);
    pthread_mutex_unlock(&state->nodes[1].watch_lock);
    pthread_mutex_lock(&source->watch_lock);
    llam_poll_watch_unpin_locked(source, state->source_watch);
    CHECK(llam_find_poll_watch_locked(source, state->fd, POLLIN) == fresh);
    llam_destroy_poll_watch_locked(source, fresh);
    pthread_mutex_unlock(&source->watch_lock);
    close(state->fd);
    close(old[1]);
    close(replacement[0]);
    close(replacement[1]);
    teardown(state);
    free(state);
}
int main(void) {
    exercise_tables();
    exercise_close_migrate_reuse(0);
    exercise_close_migrate_reuse(1);
    for (unsigned i = 0; i < 500; ++i)
        exercise_close_migrate_reuse(2);
    puts("test_io_watch_index: growth/collisions/faults/identity and 502 close-migration-FD-reuse cases "
         "passed");
    return 0;
}
#else
int main(void) {
    return 77;
}
#endif
