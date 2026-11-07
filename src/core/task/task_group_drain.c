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
#include "task_group_internal.h"

void llam_task_group_storage_free(llam_task_group_t *group) {
    if (group->cancel_token != NULL && llam_cancel_token_destroy(group->cancel_token) != 0) abort();
    if (group->drain_event != NULL) {
        if (llam_channel_close(group->drain_event) != 0 || llam_channel_destroy(group->drain_event) != 0) abort();
    }
    if (group->lock_initialized) pthread_mutex_destroy(&group->lock);
    free(group->tasks);
    free(group);
}

void llam_task_group_storage_release(llam_task_group_t *group) {
    /* The final release imports both the public destroy and notification's last
     * accesses. No registry/group lock is held while releasing the private event. */
    size_t previous = atomic_fetch_sub_explicit(&group->lifetime_refs, 1U, memory_order_acq_rel);
    if (previous == 0U) abort();
    if (previous == 1U) llam_task_group_storage_free(group);
}

bool llam_task_group_signal_locked(llam_task_group_t *group) {
    if (!group->sealed || group->drain_signalled || group->pending_retirements != 0U ||
        group->active_spawns != 0U) return false;
    group->drain_signalled = true;
    /* A single notification may race a successful drain followed by destroy.
     * Its private reference keeps the channel and group alive through close. */
    if (atomic_fetch_add_explicit(&group->lifetime_refs, 1U, memory_order_relaxed) != 1U) abort();
    return true;
}

void llam_task_group_signal(llam_task_group_t *group, bool signal) {
    if (!signal) return;
    if (llam_channel_close(group->drain_event) != 0) abort();
    llam_task_group_storage_release(group);
}

void llam_task_group_note_retirement(llam_task_t *task) {
    if (!task->group_retirement_pending) return;
    llam_task_group_t *group = task->owning_group;
    task->group_retirement_pending = false;
    pthread_mutex_lock(&group->lock);
    if (group->pending_retirements == 0U) abort();
    group->pending_retirements -= 1U;
    bool signal = llam_task_group_signal_locked(group);
    pthread_mutex_unlock(&group->lock);
    llam_task_group_signal(group, signal);
    /* reclaim_ready is published after this hook returns. A consuming join
     * cannot finish before the retiring native M has made its last group access. */
}

static int llam_group_claim_drain(llam_task_group_t *group) {
    llam_task_t *self = g_llam_tls_task;
    if (self != NULL && self->owning_group == group) { errno = EDEADLK; return -1; }
    if (group->joining || llam_public_active_op_is_saturated(llam_public_active_op_count(&group->active_ops)) ||
        (self != NULL && atomic_load_explicit(&self->opaque_blocking_depth,
                                                               memory_order_acquire) != 0U)) {
        errno = EBUSY;
        return -1;
    }
    if (self == NULL && (group->pending_retirements != 0U || group->active_spawns != 0U) &&
        !atomic_load_explicit(&group->owner_runtime->exec_started, memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    group->joining = true;
    group->sealed = true;
    return 0;
}

int llam_task_group_drain(llam_task_group_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_task_group_t *group;
    if (llam_task_group_lock_live(handle, &group) != 0) return -1;
    llam_cleanup_wait_state_t mask = {0};
    int result = llam_borrowed_cleanup_enter(&mask);
    if (result == 0) result = llam_group_claim_drain(group);
    bool signal = result == 0 && llam_task_group_signal_locked(group);
    pthread_mutex_unlock(&group->lock);
    if (result == 0) {
        llam_task_group_signal(group, signal);
        result = llam_borrowed_wait_event(group->owner_runtime, group->drain_event);
        if (result == 0) result = llam_task_group_consume_children(group, false, 0U);
        else {
            int error = errno;
            pthread_mutex_lock(&group->lock);
            group->joining = false;
            pthread_mutex_unlock(&group->lock);
            errno = error;
        }
    }
    int error = errno;
    llam_borrowed_cleanup_leave(&mask);
    if (result != 0) errno = error;
    return result;
}
