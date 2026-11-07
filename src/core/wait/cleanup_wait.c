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

int llam_borrowed_cleanup_enter(llam_cleanup_wait_state_t *mask) {
    mask->task = g_llam_tls_task;
    if (mask->task == NULL) return 0;
    mask->cleanup_depth = atomic_load_explicit(&mask->task->cleanup_wait_depth, memory_order_relaxed);
    mask->drain_depth = atomic_load_explicit(&mask->task->borrowed_drain_depth, memory_order_relaxed);
    if (mask->cleanup_depth == UINT_MAX || mask->drain_depth == UINT_MAX) { errno = EBUSY; return -1; }
    atomic_store_explicit(&mask->task->cleanup_wait_depth, mask->cleanup_depth + 1U, memory_order_relaxed);
    /* Publish before a new wait generation; stop/cancel scanners recheck the
     * mask after acquiring that generation before taking any wake ownership. */
    atomic_store_explicit(&mask->task->borrowed_drain_depth, mask->drain_depth + 1U, memory_order_release);
    return 0;
}

void llam_borrowed_cleanup_leave(const llam_cleanup_wait_state_t *mask) {
    if (mask->task == NULL) return;
    atomic_store_explicit(&mask->task->borrowed_drain_depth, mask->drain_depth, memory_order_release);
    atomic_store_explicit(&mask->task->cleanup_wait_depth, mask->cleanup_depth, memory_order_relaxed);
}

static int llam_borrowed_host_wait(llam_runtime_t *owner, llam_channel_t *event) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_begin_public_op(owner, &runtime) != 0) return -1;
    int result = -1, error = 0;
    for (;;) {
        void *unused;
        int status = llam_channel_try_recv_result(event, &unused);
        if (status != 0 && errno == EPIPE) { result = 0; break; }
        if (status == 0 || errno != EAGAIN) { error = status == 0 ? EIO : errno; break; }
        error = atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire);
        if (error != 0) break;
        if (!atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) { error = EBUSY; break; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
    llam_runtime_end_public_op(runtime);
    if (result != 0) errno = error;
    return result;
}

int llam_borrowed_wait_event(llam_runtime_t *owner, llam_channel_t *event) {
    void *unused;
    int result = llam_channel_try_recv_result(event, &unused);
    if (result != 0 && errno == EPIPE) return 0;
    if (result == 0 || errno != EAGAIN) { if (result == 0) errno = EIO; return -1; }
    if (g_llam_tls_task == NULL) return llam_borrowed_host_wait(owner, event);
    result = llam_channel_recv_result(event, &unused);
    if (result != 0 && errno == EPIPE) return 0;
    int fatal = atomic_load_explicit(&owner->fatal_errno, memory_order_acquire);
    if (fatal != 0) errno = fatal;
    else if (result == 0) errno = EIO;
    return -1;
}
