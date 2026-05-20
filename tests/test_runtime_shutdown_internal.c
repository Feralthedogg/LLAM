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
#endif

#include <errno.h>
#include <limits.h>
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

    if (pthread_mutex_lock(&g_llam_runtime.nodes[0].watch_lock) != 0) {
        free(payload);
        free(ready);
        free(watch);
        llam_runtime_shutdown();
        return fail_errno("watch lock failed");
    }
    watch->next = g_llam_runtime.nodes[0].recv_watches;
    g_llam_runtime.nodes[0].recv_watches = watch;
    if (pthread_mutex_unlock(&g_llam_runtime.nodes[0].watch_lock) != 0) {
        llam_runtime_shutdown();
        return fail_errno("watch unlock failed");
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

int main(void) {
    if (exercise_recv_ready_copy_payload_shutdown() != 0) {
        return 1;
    }
    if (exercise_recv_ready_pop_without_transfer() != 0) {
        return 1;
    }
    printf("test_runtime_shutdown_internal ok\n");
    return 0;
}
