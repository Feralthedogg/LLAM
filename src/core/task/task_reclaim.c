/**
 * @file src/core/task/task_reclaim.c
 * @brief Task diagnostic-list linking and final task-object reclamation.
 *
 * @details
 * This translation unit owns task visibility for diagnostic/rehome scans and
 * the final reclaim paths for joined or detached tasks.  Keeping this separate
 * from stack mapping code makes the task lifetime boundary explicit: stack
 * mappings may be cached independently, while task objects are reclaimed only
 * after join/detach ownership rules have been satisfied.
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

/**
 * @brief Link a task into a shard-local diagnostic task list.
 *
 * @param shard Shard that owns the list; its lock must already be held.
 * @param task  Task to insert.
 */
void llam_add_task_to_list_locked(llam_shard_t *shard, llam_task_t *task) {
    if (shard == NULL || task == NULL) {
        return;
    }
    if (atomic_load_explicit(&task->task_listed, memory_order_acquire) != 0U) {
        return;
    }
    task->all_prev = NULL;
    task->all_next = shard->all_tasks;
    if (shard->all_tasks != NULL) {
        shard->all_tasks->all_prev = task;
    }
    shard->all_tasks = task;
    atomic_store_explicit(&task->task_listed, 1U, memory_order_release);
}

/**
 * @brief Link a task into its allocation-owner shard task list.
 *
 * @param rt   Runtime that owns the shards.
 * @param task Task to insert.
 */
void llam_add_task_to_list(llam_runtime_t *rt, llam_task_t *task) {
    llam_shard_t *owner;

    if (rt == NULL || task == NULL || task->alloc_owner_shard >= rt->active_shards) {
        return;
    }
    owner = &rt->shards[task->alloc_owner_shard];
    pthread_mutex_lock(&owner->lock);
    llam_add_task_to_list_locked(owner, task);
    pthread_mutex_unlock(&owner->lock);
}

/**
 * @brief Pin a task while a diagnostic/shutdown scan owns a raw task pointer.
 *
 * @details
 * Scan refs are intentionally short-lived references used after a task has
 * been found under a shard/token lock but before the scanner can cancel or
 * complete the wait outside that lock.  Saturation is a corrupted lifetime
 * state: wrapping to zero would let reclaim free the task while the scanner
 * still holds the pointer.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Task to pin for a scan.
 *
 * @return true on success, false with errno set on invalid input or overflow.
 */
bool llam_task_scan_ref_try_acquire(llam_runtime_t *rt, llam_task_t *task) {
    if (rt == NULL || task == NULL) {
        errno = EINVAL;
        if (rt != NULL) {
            llam_record_fatal(rt, EINVAL);
        }
        return false;
    }
    return llam_sync_note_inflight_waiter(rt, &task->scan_refs, 1U);
}

/**
 * @brief Release a short diagnostic/shutdown scan reference.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Task whose scan pin should be dropped.
 *
 * @return true on success, false with errno set on invalid input or underflow.
 */
bool llam_task_scan_ref_release(llam_runtime_t *rt, llam_task_t *task) {
    if (rt == NULL || task == NULL) {
        errno = EINVAL;
        if (rt != NULL) {
            llam_record_fatal(rt, EINVAL);
        }
        return false;
    }
    return llam_sync_complete_inflight_waiter(rt, &task->scan_refs, 1U);
}

/**
 * @brief Wait until all scan refs have drained without spinning on corruption.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Task whose scan refs must reach zero before final reclaim.
 *
 * @return 0 when quiescent, -1 with errno set when a saturated scan counter is
 *         observed.
 */
int llam_task_wait_scan_refs_quiescent(llam_runtime_t *rt, llam_task_t *task) {
    unsigned refs;

    if (rt == NULL || task == NULL) {
        errno = EINVAL;
        if (rt != NULL) {
            llam_record_fatal(rt, EINVAL);
        }
        return -1;
    }

    for (;;) {
        refs = atomic_load_explicit(&task->scan_refs, memory_order_acquire);
        if (refs == 0U) {
            return 0;
        }
        if (LLAM_UNLIKELY(refs == UINT_MAX)) {
            llam_record_fatal(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return -1;
        }
        if (g_llam_tls_task != NULL) {
            llam_yield();
        } else {
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = 100000L,
            };

            nanosleep(&ts, NULL);
        }
    }
}

/**
 * @brief Decide whether every spawned task should enter diagnostic lists.
 *
 * @details
 * Default mode is lazy: tasks are linked only once they park on a wait that
 * dynamic rehome or diagnostics must be able to find.  This removes diagnostic
 * list mutation from spawn-heavy workloads.  Debug-safe runs and explicit
 * @c LLAM_TASK_LIST_EAGER=1 keep the older eager behavior.
 *
 * @param rt Runtime whose profile controls the default policy.
 * @return true when spawn should eagerly link the task.
 */
bool llam_task_list_eager_enabled(const llam_runtime_t *rt) {
    return rt != NULL && rt->task_list_eager != 0U;
}

/**
 * @brief Lazily link a task into the owner shard task list.
 *
 * @details
 * Parked waiters and in-flight I/O are the only tasks that dynamic rehome must
 * scan.  Linking at first park keeps the common spawn/run/join fast path free of
 * diagnostic list pointer mutation while preserving rehome visibility before the
 * task publishes a parked state.
 *
 * @param task Task to make visible to rehome/diagnostic scans.
 */
void llam_task_ensure_listed(llam_task_t *task) {
    llam_runtime_t *rt;
    llam_shard_t *owner;

    rt = task != NULL ? task->owner_runtime : NULL;
    if (task == NULL ||
        rt == NULL ||
        atomic_load_explicit(&task->task_listed, memory_order_acquire) != 0U ||
        task->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[task->alloc_owner_shard];
    pthread_mutex_lock(&owner->lock);
    llam_add_task_to_list_locked(owner, task);
    pthread_mutex_unlock(&owner->lock);
}

/**
 * @brief Mark a joined task as safe for final reclamation.
 *
 * @param task Task whose join lifecycle reached reclaim-ready state.
 */
void llam_task_mark_reclaim_ready(llam_task_t *task) {
    if (task == NULL) {
        return;
    }
    atomic_store_explicit(&task->reclaim_ready, 1U, memory_order_release);
}

/**
 * @brief Remove a task from the runtime-wide diagnostic task list.
 *
 * @param rt   Runtime that owns the list.
 * @param task Task to unlink.
 * @return true if the task was present and removed.
 */
static bool llam_remove_task_from_list(llam_runtime_t *rt, llam_task_t *task) {
    llam_shard_t *owner;
    bool removed = false;

    if (rt == NULL || task == NULL ||
        atomic_load_explicit(&task->task_listed, memory_order_acquire) == 0U ||
        task->alloc_owner_shard >= rt->active_shards) {
        return false;
    }

    owner = &rt->shards[task->alloc_owner_shard];
    pthread_mutex_lock(&owner->lock);
    if (atomic_load_explicit(&task->task_listed, memory_order_acquire) != 0U &&
        (task->all_prev != NULL || owner->all_tasks == task)) {
        if (task->all_prev != NULL) {
            task->all_prev->all_next = task->all_next;
        } else {
            owner->all_tasks = task->all_next;
        }
        if (task->all_next != NULL) {
            task->all_next->all_prev = task->all_prev;
        }
        task->all_next = NULL;
        task->all_prev = NULL;
        atomic_store_explicit(&task->task_listed, 0U, memory_order_release);
        removed = true;
    }
    pthread_mutex_unlock(&owner->lock);
    return removed;
}

/**
 * @brief Finish reclamation after a caller has claimed task ownership.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Task whose reclaim_claimed flag is already set by the caller.
 */
void llam_reclaim_claimed_task(llam_runtime_t *rt, llam_task_t *task) {
    if (rt == NULL || task == NULL) {
        return;
    }
    llam_task_invalidate_public_handle(task);
    (void)llam_remove_task_from_list(rt, task);
    /*
     * Stop/fatal scans take short references while they cancel parked waiters
     * outside the shard-list lock.  Reclaim waits here so those scans cannot
     * race a listed task into use-after-free.
     */
    if (llam_task_wait_scan_refs_quiescent(rt, task) != 0) {
        /*
         * A saturated scan-ref sentinel means a scanner may still own a raw
         * task pointer.  The handle is invalidated and the task list entry is
         * gone, so leak instead of freeing under an untrusted lifetime state.
         */
        return;
    }
    if (llam_task_wait_public_ops_quiescent(task) != 0) {
        /*
         * A saturated active-op sentinel means the task cannot be proven
         * unreferenced.  The public handle has already been invalidated, so the
         * least dangerous outcome is a bounded leak instead of UAF or teardown
         * deadlock.
         */
        return;
    }
    llam_free_task(task);
}

/**
 * @brief Reclaim a joined task once join waiters have released it.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Joined task candidate.
 */
void llam_try_reclaim_joined_task(llam_runtime_t *rt, llam_task_t *task) {
    unsigned expected = 0U;

    if (rt == NULL || task == NULL) {
        return;
    }
    // The final join waiter marks reclaim_ready. A non-runtime thread sleeps
    // briefly; a fiber yields so another runnable can complete the handoff.
    while (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) == 0U) {
        if (g_llam_tls_task != NULL) {
            llam_yield();
        } else {
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = 100000L,
            };

            nanosleep(&ts, NULL);
        }
    }
    if (task->join_waiter_count_at_exit > 1U) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(&task->reclaim_claimed,
                                                 &expected,
                                                 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }
    llam_reclaim_claimed_task(rt, task);
}

/**
 * @brief Reclaim a detached task after it has fully returned to the scheduler.
 *
 * @param rt   Runtime that owns @p task.
 * @param task Detached task candidate.
 */
void llam_try_reclaim_detached_task(llam_runtime_t *rt, llam_task_t *task) {
    unsigned expected = 0U;

    if (rt == NULL || task == NULL) {
        return;
    }
    if (atomic_load_explicit(&task->detached, memory_order_acquire) == 0U ||
        atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) == 0U) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(&task->reclaim_claimed,
                                                 &expected,
                                                 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }
    llam_reclaim_claimed_task(rt, task);
}

/**
 * @brief Fully destroy a task object and return it to the task allocator.
 *
 * @param task Task to free.
 */
void llam_free_task(llam_task_t *task) {
    if (task == NULL) {
        return;
    }
    if (llam_task_wait_public_ops_quiescent(task) != 0) {
        return;
    }

    if (task->cancel_token != NULL) {
        // A task may still be registered as a cancellation waiter when freed
        // through error paths. Detach it before dropping the token reference.
        pthread_mutex_lock(&task->cancel_token->lock);
        if (task->cancel_registered) {
            if (task->cancel_prev != NULL) {
                task->cancel_prev->cancel_next = task->cancel_next;
            } else {
                task->cancel_token->waiters = task->cancel_next;
            }
            if (task->cancel_next != NULL) {
                task->cancel_next->cancel_prev = task->cancel_prev;
            }
            task->cancel_prev = NULL;
            task->cancel_next = NULL;
            task->cancel_registered = false;
        }
        pthread_mutex_unlock(&task->cancel_token->lock);
        llam_cancel_token_release_task_ref(task->cancel_token);
    }
    llam_ctx_destroy_fp_state(&task->ctx);
    if (task->stack_mapping != NULL && task->mapping_size != 0U) {
        (void)munmap(task->stack_mapping, task->mapping_size);
    }
    llam_task_allocator_free(task);
}
