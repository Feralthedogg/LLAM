/**
 * @file src/core/wait/wait_deadline.c
 * @brief Task deadline and cancellation-token wait tracking.
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

static llam_shard_t *llam_task_deadline_shard(llam_task_t *task) {
    llam_runtime_t *rt = llam_wait_task_runtime(task);

    if (task == NULL || rt == NULL || rt->active_shards == 0U) {
        return NULL;
    }

    if (task->parked_shard < rt->active_shards) {
        return &rt->shards[task->parked_shard];
    }

    /*
     * Teardown, rehome, and partially failed setup paths can leave parked_shard
     * defensive rather than authoritative.  last_shard still maps the task to a
     * valid shard for timer cleanup.
     */
    return &rt->shards[atomic_load_explicit(&task->last_shard, memory_order_relaxed) % rt->active_shards];
}
/**
 * @brief Arm a deadline timer for a parked task.
 *
 * Timed waits all funnel through the same timer node so timeout and cancellation
 * share one wake path.
 *
 * @param task        Task being parked.
 * @param shard       Timer owner shard.
 * @param deadline_ns Absolute wake deadline.
 * @return 0 on success, -1 on invalid args or timer allocation failure.
 */
int llam_arm_task_wait_deadline(llam_task_t *task, llam_shard_t *shard, uint64_t deadline_ns) {
    bool inserted;
    int insert_errno;

    if (task == NULL || shard == NULL) {
        errno = EINVAL;
        return -1;
    }

    task->deadline_ns = deadline_ns;
    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, task);
    insert_errno = errno;
    /*
     * Decide allocation success while the timer heap lock is still held.  A
     * very short deadline can be popped by the watchdog immediately after this
     * lock is released; that also clears active_timer, but it is completion, not
     * allocation failure.
     */
    inserted = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    if (!inserted) {
        task->deadline_ns = 0U;
        errno = insert_errno != 0 ? insert_errno : ENOMEM;
        return -1;
    }
    return 0;
}

/**
 * @brief Check whether a task still owns an active deadline timer.
 *
 * @details
 * Timer expiry clears @c active_timer under the shard timer lock, including
 * from watchdog/helper pthreads. Readers must use the same lock; otherwise a
 * producer wake can race with timer expiry while deciding whether a direct
 * handoff is safe.
 *
 * @param task Task whose deadline ownership should be checked.
 * @return true if the task currently has an active timer node.
 */
bool llam_task_wait_deadline_active(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);
    bool active = false;

    if (shard == NULL) {
        return false;
    }

    pthread_mutex_lock(&shard->lock);
    active = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    return active;
}

/**
 * @brief Remove a task's active wait deadline, if one exists.
 *
 * @param task Task whose timer should be removed.
 */
void llam_disarm_task_wait_deadline(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);

    if (shard == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    if (task->active_timer != NULL) {
        (void)llam_timer_remove_locked(shard, task);
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Register a task as a cancellation-token waiter.
 *
 * @param task Task with an optional cancel token.
 * @return 0 on success/no token, -1 with ECANCELED if already canceled.
 */
int llam_cancel_token_register_task(llam_task_t *task) {
    llam_cancel_token_t *token;

    if (task == NULL || task->cancel_token == NULL) {
        return 0;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (token->cancelled) {
        pthread_mutex_unlock(&token->lock);
        errno = ECANCELED;
        return -1;
    }

    if (!task->cancel_registered) {
        // Intrusive task links avoid allocating separate cancel wait nodes.
        task->cancel_prev = NULL;
        task->cancel_next = token->waiters;
        if (token->waiters != NULL) {
            token->waiters->cancel_prev = task;
        }
        token->waiters = task;
        task->cancel_registered = true;
    }
    pthread_mutex_unlock(&token->lock);
    return 0;
}

/**
 * @brief Remove a task from its cancellation-token waiter list.
 *
 * @param task Task to unregister.
 */
void llam_cancel_token_unregister_task(llam_task_t *task) {
    llam_cancel_token_t *token;
    llam_task_t *cur;
    bool linked = false;

    if (task == NULL || task->cancel_token == NULL) {
        return;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (task->cancel_registered) {
        for (cur = token->waiters; cur != NULL; cur = cur->cancel_next) {
            if (cur == task) {
                linked = true;
                break;
            }
        }
        if (linked) {
            if (task->cancel_prev != NULL) {
                task->cancel_prev->cancel_next = task->cancel_next;
            } else {
                token->waiters = task->cancel_next;
            }
            if (task->cancel_next != NULL) {
                task->cancel_next->cancel_prev = task->cancel_prev;
            }
        }
        task->cancel_prev = NULL;
        task->cancel_next = NULL;
        task->cancel_registered = false;
    }
    pthread_mutex_unlock(&token->lock);
}
