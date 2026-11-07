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
#include "structured_internal.h"

void llam_structured_notify(llam_runtime_t *owner, llam_channel_t *channel) {
    int saved_errno = errno;
    if (llam_channel_close(channel) != 0) {
        int error = errno;
        llam_runtime_t *pinned = NULL;
        /* Destructive shutdown already wakes waiters. Other unexpected signal
         * failures must not leave an unbounded waiter silently parked. */
        if (llam_runtime_begin_object_owner_op_if_live(owner, &pinned)) {
            llam_record_fatal_deferred(pinned, error);
            llam_runtime_end_public_op(pinned);
        }
    }
    errno = saved_errno;
}

static int llam_structured_host_wait(llam_runtime_t *runtime, llam_channel_t *done, uint64_t deadline) {
    for (;;) {
        void *unused;
        int result = llam_channel_try_recv_result(done, &unused);
        if (result != 0 && errno == EPIPE) return 0;
        if (result == 0 || errno != EAGAIN) { if (result == 0) errno = EINVAL; return -1; }
        if (deadline != UINT64_MAX && llam_now_ns() >= deadline) { errno = ETIMEDOUT; return -1; }
        int fatal = atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire);
        if (fatal != 0) { errno = fatal; return -1; }
        if (!atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) { errno = EBUSY; return -1; }
        /* The last task also requests ordinary run stop before native retirement.
         * That transition must not cancel a host waiting for its final result. */
        if (atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
            errno = ECANCELED;
            return -1;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
}

int llam_structured_wait(llam_runtime_t *owner, llam_channel_t *done, uint64_t deadline) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_begin_live_object_owner_op(owner, &runtime, ENOTSUP) != 0) return -1;
    int result;
    if (g_llam_tls_task != NULL) {
        void *unused;
        result = deadline == UINT64_MAX ? llam_channel_recv_result(done, &unused)
                                         : llam_channel_recv_until_result(done, deadline, &unused);
        if (result != 0 && errno == EPIPE) result = 0;
    } else result = llam_structured_host_wait(runtime, done, deadline);
    int error = errno;
    llam_runtime_end_public_op(runtime);
    if (result != 0) errno = error;
    return result;
}

static int llam_scope_cleanup_wait(llam_scope_t *scope, uint64_t deadline) {
    llam_task_t *task = g_llam_tls_task;
    unsigned previous = task != NULL ? atomic_load_explicit(&task->cleanup_wait_depth, memory_order_relaxed) : 0U;
    if (previous == UINT_MAX) { errno = EBUSY; return -1; }
    /* This is a G-local policy, not lifetime publication. Wait generations still
     * reject old token deliveries, and the tokens themselves remain cancelled. */
    if (task != NULL) atomic_store_explicit(&task->cleanup_wait_depth, previous + 1U, memory_order_relaxed);
    int result = llam_structured_wait(scope->object.owner, scope->done, deadline);
    int error = errno;
    if (task != NULL) atomic_store_explicit(&task->cleanup_wait_depth, previous, memory_order_relaxed);
    if (result != 0) errno = error;
    return result;
}

static int llam_scope_begin_drain(llam_scope_t *scope, bool *notify) {
    if (scope->draining) { errno = EBUSY; return -1; }
    if (g_llam_tls_task != NULL && g_llam_tls_task->scope_completion != NULL &&
        g_llam_tls_task->scope_completion->scope == scope) { errno = EDEADLK; return -1; }
    scope->draining = true;
    if (scope->report.state == LLAM_SCOPE_OPEN) scope->report.state = LLAM_SCOPE_CLOSING;
    *notify = llam_scope_finish_closing_locked(scope);
    return 0;
}

static int llam_scope_finish_drain(llam_scope_t *scope, int result, int error,
                                   llam_scope_report_t *out, size_t size) {
    pthread_mutex_lock(&g_llam_structured_lock);
    if (scope->report.state == LLAM_SCOPE_DRAINED) result = 0;
    else if (result == 0) { result = -1; error = EIO; }
    scope->draining = false;
    llam_scope_report_t report = scope->report;
    if (result != 0 && error == EBUSY) report.flags |= LLAM_SCOPE_REPORT_F_NEEDS_RUNNER;
    if (out != NULL) memcpy(out, &report, size < sizeof(report) ? size : sizeof(report));
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_structured_end(&scope->object);
    if (result != 0) errno = error;
    return result;
}

static int llam_scope_drain_impl(llam_scope_t *handle, uint64_t deadline,
                                 llam_scope_report_t *out, size_t size, bool cancel) {
    if (out != NULL && size == 0U) { errno = EINVAL; return -1; }
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, &object) != 0) return -1;
    llam_scope_t *scope = (llam_scope_t *)object;
    bool notify = false;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_scope_begin_drain(scope, &notify);
    bool ready = scope->report.state == LLAM_SCOPE_DRAINED;
    if (result != 0) llam_scope_copy_report_locked(scope, out, size);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (result != 0) {
        int error = errno;
        llam_structured_end(object);
        errno = error;
        return -1;
    }
    if (notify) llam_structured_notify(object->owner, scope->done);
    if (cancel) result = llam_scope_request_cancel(scope);
    if (result == 0 && !ready) result = llam_scope_cleanup_wait(scope, deadline);
    return llam_scope_finish_drain(scope, result, errno, out, size);
}

int llam_scope_drain_until(llam_scope_t *scope, uint64_t deadline,
                           llam_scope_report_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_scope_drain_impl(scope, deadline, out, size, false);
}

int llam_scope_cancel_and_drain_until(llam_scope_t *scope, uint64_t deadline,
                                      llam_scope_report_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_scope_drain_impl(scope, deadline, out, size, true);
}
