/**
 * @file src/io/watch/watch_queue.c
 * @brief Shared I/O watch submit/control queue helpers.
 *
 * @details
 * Native backends use the same submit-list ownership transitions and control
 * queue allocation rules.  Keeping them here avoids drift between Linux,
 * Darwin, and Windows watch workers while preserving each backend's completion
 * policy in its platform directory.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#include "runtime_internal.h"

/*
 * Numeric descriptors are process-global and may be reused immediately after
 * close.  Serialize the short watch attach/migration critical sections with
 * the public close boundary so no backend watch can be created in the gap
 * between runtime-state invalidation and the platform close.
 */
#if LLAM_PLATFORM_WINDOWS

static INIT_ONCE g_llam_fd_watch_lifecycle_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_llam_fd_watch_lifecycle_lock;

static BOOL CALLBACK llam_fd_watch_lifecycle_init_once(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_llam_fd_watch_lifecycle_lock);
    return TRUE;
}

void llam_fd_watch_lifecycle_lock(void) {
    if (!InitOnceExecuteOnce(&g_llam_fd_watch_lifecycle_once,
                            llam_fd_watch_lifecycle_init_once,
                            NULL,
                            NULL)) {
        abort();
    }
    EnterCriticalSection(&g_llam_fd_watch_lifecycle_lock);
}

void llam_fd_watch_lifecycle_unlock(void) {
    LeaveCriticalSection(&g_llam_fd_watch_lifecycle_lock);
}

#else

static pthread_once_t g_llam_fd_watch_lifecycle_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_llam_fd_watch_lifecycle_lock;

static void llam_fd_watch_lifecycle_init_once(void) {
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&g_llam_fd_watch_lifecycle_lock, &attr) != 0) {
        abort();
    }
    (void)pthread_mutexattr_destroy(&attr);
}

void llam_fd_watch_lifecycle_lock(void) {
    (void)pthread_once(&g_llam_fd_watch_lifecycle_once, llam_fd_watch_lifecycle_init_once);
    int rc = pthread_mutex_lock(&g_llam_fd_watch_lifecycle_lock);

    if (rc != 0) {
        abort();
    }
}

void llam_fd_watch_lifecycle_unlock(void) {
    int rc = pthread_mutex_unlock(&g_llam_fd_watch_lifecycle_lock);

    if (rc != 0) {
        abort();
    }
}

#endif

void llam_io_control_op_destroy(llam_node_t *node, llam_io_control_op_t *op) {
    llam_runtime_t *rt = node != NULL ? node->runtime : NULL;
    llam_task_t *task_ref;
    llam_io_req_t *req;
    bool holds_task_ref;
    bool holds_request_ref;

    if (op == NULL) {
        return;
    }

    task_ref = op->task_ref;
    req = op->kind == LLAM_IO_CONTROL_REQ_CANCEL ? op->target : NULL;
    holds_task_ref = op->holds_task_ref;
    holds_request_ref = op->holds_request_ref;
    op->task_ref = NULL;
    op->holds_task_ref = false;
    op->holds_request_ref = false;

    /* The request put may publish/reset embedded storage; do not touch it later. */
    if (holds_request_ref && req != NULL) {
        (void)llam_io_req_lifetime_release(req);
    }
    if (holds_task_ref && task_ref != NULL && rt != NULL) {
        (void)llam_task_scan_ref_release(rt, task_ref);
    }
    free(op);
}

void llam_shard_note_inflight_io_waiter(llam_runtime_t *rt, unsigned owner_shard, int delta) {
    atomic_uint *counter;
    unsigned amount;
    unsigned current;

    if (rt == NULL || delta == 0 || owner_shard >= rt->active_shards) {
        return;
    }
    counter = &rt->shards[owner_shard].inflight_io_waiters;
    if (delta > 0) {
        amount = (unsigned)delta;
        current = atomic_load_explicit(counter, memory_order_acquire);
        for (;;) {
            if (UINT_MAX - current < amount) {
                llam_record_fatal_deferred(rt, EOVERFLOW);
                return;
            }
            if (atomic_compare_exchange_weak_explicit(counter,
                                                      &current,
                                                      current + amount,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                return;
            }
        }
    }

    amount = delta == INT_MIN ? ((unsigned)INT_MAX + 1U) : (unsigned)(-delta);
    current = atomic_load_explicit(counter, memory_order_acquire);
    for (;;) {
        if (current < amount) {
            /*
             * In-flight waiter counters are diagnostics and scale guards, not
             * ownership themselves.  Clamp by refusing the stale decrement
             * instead of wrapping to UINT_MAX and poisoning later decisions.
             */
            llam_record_fatal_deferred(rt, EINVAL);
            return;
        }
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  current - amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
}

bool llam_node_note_pending_ops(llam_node_t *node, unsigned amount) {
    llam_runtime_t *rt = node != NULL ? node->runtime : NULL;
    unsigned pending;

    if (amount == 0U) {
        return true;
    }
    if (node == NULL) {
        errno = EINVAL;
        return false;
    }

    pending = atomic_load_explicit(&node->pending_ops, memory_order_acquire);
    for (;;) {
        if (UINT_MAX - pending < amount) {
            /*
             * pending_ops is both a worker wake/sleep gate and a shutdown
             * diagnostic.  Saturated counters must reject new ownership
             * instead of wrapping to zero while work remains queued.
             */
            if (rt != NULL) {
                llam_record_fatal_deferred(rt, EOVERFLOW);
            }
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&node->pending_ops,
                                                  &pending,
                                                  pending + amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

bool llam_node_complete_pending_ops(llam_node_t *node, unsigned amount) {
    llam_runtime_t *rt = node != NULL ? node->runtime : NULL;
    unsigned pending;

    if (amount == 0U) {
        return true;
    }
    if (node == NULL) {
        errno = EINVAL;
        return false;
    }

    pending = atomic_load_explicit(&node->pending_ops, memory_order_acquire);
    while (pending >= amount) {
        if (atomic_compare_exchange_weak_explicit(&node->pending_ops,
                                                  &pending,
                                                  pending - amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }

    if (rt != NULL) {
        llam_record_fatal_deferred(rt, EINVAL);
    }
    errno = EINVAL;
    return false;
}

bool llam_io_completion_begin(llam_node_t *node, llam_io_req_t *req, bool decrement_pending) {
    llam_runtime_t *node_runtime = node != NULL ? node->runtime : NULL;

    if (LLAM_UNLIKELY(node_runtime == NULL || req == NULL || req->owner_runtime == NULL)) {
        if (node_runtime != NULL) {
            llam_record_fatal(node_runtime, EINVAL);
        }
        return false;
    }

    if (LLAM_UNLIKELY(node_runtime != req->owner_runtime)) {
        /*
         * A completion packet/user-data record must never cross runtime
         * ownership domains.  Continuing would route a parked task through the
         * wrong scheduler and can turn stale backend state into cross-runtime
         * UAF, so fail closed on the runtime that observed the bad packet.
         */
        llam_record_fatal(node_runtime, EXDEV);
        return false;
    }
    if (decrement_pending && !llam_node_complete_pending_ops(node, 1U)) {
        return false;
    }
    return true;
}

bool llam_io_req_transfer_inflight_owner(llam_io_req_t *req, unsigned from_shard, unsigned to_shard) {
    unsigned expected;
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;

    if (req == NULL || rt == NULL || from_shard == to_shard ||
        from_shard >= rt->active_shards || to_shard >= rt->active_shards) {
        return false;
    }

    expected = from_shard;
    if (!atomic_compare_exchange_strong_explicit(&req->inflight_owner_shard,
                                                 &expected,
                                                 to_shard,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    llam_shard_note_inflight_io_waiter(rt, from_shard, -1);
    llam_shard_note_inflight_io_waiter(rt, to_shard, 1);
    return true;
}

bool llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    llam_runtime_t *node_runtime = node != NULL ? node->runtime : NULL;

    if (node == NULL || req == NULL) {
        if (node_runtime != NULL) {
            llam_record_fatal_deferred(node_runtime, EINVAL);
        }
        errno = EINVAL;
        return false;
    }
    if (LLAM_UNLIKELY(node_runtime == NULL || req->owner_runtime == NULL)) {
        if (node_runtime != NULL) {
            llam_record_fatal_deferred(node_runtime, EINVAL);
        }
        errno = EINVAL;
        return false;
    }
    if (LLAM_UNLIKELY(node_runtime != req->owner_runtime)) {
        /*
         * Submit queues are node-owned; accepting a foreign request would let a
         * backend issue and later complete work through the wrong runtime.  The
         * caller still owns the request because it has not been linked.
         */
        llam_record_fatal_deferred(node_runtime, EXDEV);
        errno = EXDEV;
        return false;
    }
    req->next = NULL;
    if (node->submit_tail != NULL) {
        node->submit_tail->next = req;
    } else {
        node->submit_head = req;
    }
    node->submit_tail = req;
    return true;
}

bool llam_node_submit_io_req(llam_node_t *node, llam_io_req_t *req) {
    int saved_errno;

    if (node == NULL || req == NULL) {
        errno = EINVAL;
        return false;
    }

    pthread_mutex_lock(&node->submit_lock);
    if (llam_io_req_abort_requested(req)) {
        /*
         * Wait tracking is published before this queue link. A stop/cancel
         * resolver that wins that window latches the abort while holding all
         * submit locks; reject publication so its one-shot delivery cannot be
         * lost behind a later enqueue.
         */
        pthread_mutex_unlock(&node->submit_lock);
        errno = ECANCELED;
        return false;
    }
    if (!llam_node_note_pending_ops(node, 1U)) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    if (llam_queue_node_submit_locked(node, req)) {
        pthread_mutex_unlock(&node->submit_lock);
        return true;
    }

    saved_errno = errno;
    (void)llam_node_complete_pending_ops(node, 1U);
    pthread_mutex_unlock(&node->submit_lock);
    errno = saved_errno;
    return false;
}

bool llam_remove_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur;

    if (node == NULL || req == NULL) {
        return false;
    }

    cur = node->submit_head;
    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                node->submit_head = cur->next;
            }
            if (node->submit_tail == cur) {
                node->submit_tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static llam_io_submit_detach_snapshot_hook_fn
    g_llam_io_submit_detach_snapshot_hook;

void llam_io_test_set_submit_detach_snapshot_hook(
    llam_io_submit_detach_snapshot_hook_fn hook) {
    g_llam_io_submit_detach_snapshot_hook = hook;
}

static void llam_io_test_submit_detach_snapshot(llam_io_req_t *req,
                                                unsigned node_index) {
    if (g_llam_io_submit_detach_snapshot_hook != NULL) {
        g_llam_io_submit_detach_snapshot_hook(req, node_index);
    }
}
#else
static void llam_io_test_submit_detach_snapshot(llam_io_req_t *req,
                                                unsigned node_index) {
    (void)req;
    (void)node_index;
}
#endif

/**
 * @brief Detach one exact request while excluding every cross-node evacuation.
 *
 * Every submit lock is acquired in node-index order, matching watchdog
 * evacuation's pair ordering. The exact queue is authoritative; the atomic
 * attachment is revalidated under those locks and repaired only after the
 * request itself has been found. Pending-op accounting is decremented on the
 * node that actually owned the request.
 */
llam_io_submit_detach_result_t llam_detach_submit_req_current(
    llam_io_req_t *req,
    unsigned *node_index_out) {
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;
    unsigned attached_node_index;
    unsigned found_node = UINT_MAX;
    unsigned locked_nodes = 0U;
    llam_io_submit_detach_result_t result =
        LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED;

    if (node_index_out != NULL) {
        *node_index_out = UINT_MAX;
    }
    if (req == NULL || rt == NULL || rt->nodes == NULL ||
        rt->active_nodes == 0U) {
        return LLAM_IO_SUBMIT_DETACH_NOT_FOUND;
    }

    attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                               memory_order_acquire);
    llam_io_test_submit_detach_snapshot(req, attached_node_index);
    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        pthread_mutex_lock(&rt->nodes[i].submit_lock);
        locked_nodes += 1U;
    }

    if (atomic_load_explicit(&req->wait_mode,
                             memory_order_acquire) !=
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        result = LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED;
        goto unlock;
    }
    attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                               memory_order_acquire);
    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        if (llam_remove_node_submit_locked(&rt->nodes[i], req)) {
            found_node = i;
            break;
        }
    }
    if (found_node == UINT_MAX) {
        result = LLAM_IO_SUBMIT_DETACH_NOT_FOUND;
        goto unlock;
    }
    if (attached_node_index != found_node) {
        /* Exact list ownership is safe to detach, but metadata drift is fatal. */
        llam_record_fatal_deferred(rt, EPROTO);
        atomic_store_explicit(&req->attached_node_index,
                              found_node,
                              memory_order_release);
    }
    atomic_store_explicit(&req->wait_mode,
                          LLAM_IO_WAIT_MODE_NONE,
                          memory_order_release);
    atomic_store_explicit(&req->inflight_owner_shard,
                          UINT_MAX,
                          memory_order_release);
    result = LLAM_IO_SUBMIT_DETACH_REMOVED;

unlock:
    while (locked_nodes > 0U) {
        locked_nodes -= 1U;
        pthread_mutex_unlock(&rt->nodes[locked_nodes].submit_lock);
    }
    if (result == LLAM_IO_SUBMIT_DETACH_REMOVED) {
        (void)llam_node_complete_pending_ops(&rt->nodes[found_node], 1U);
        if (node_index_out != NULL) {
            *node_index_out = found_node;
        }
    }
    return result;
}

llam_io_req_t *llam_take_node_submissions(llam_node_t *node) {
    llam_io_req_t *head;
    llam_io_req_t *cursor;

    if (node == NULL) {
        return NULL;
    }

    /*
     * Move the whole submit list out under submit_lock, then publish backend
     * ownership before releasing the list to the worker. Cancellation paths that
     * miss the submit queue must then see INFLIGHT and use backend cancel.
     */
    pthread_mutex_lock(&node->submit_lock);
    head = node->submit_head;
    node->submit_head = NULL;
    node->submit_tail = NULL;
    cursor = head;
    while (cursor != NULL) {
        unsigned owner_shard = atomic_load_explicit(&cursor->owner_shard,
                                                    memory_order_acquire);

        atomic_store_explicit(&cursor->inflight_owner_shard,
                              owner_shard,
                              memory_order_release);
        atomic_store(&cursor->wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT);
        llam_shard_note_inflight_io_waiter(cursor->owner_runtime,
                                          owner_shard,
                                          1);
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    llam_io_control_op_t *op;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }

    op = calloc(1, sizeof(*op));
    if (op == NULL) {
        return -1;
    }

    op->kind = kind;
    op->target = target;
    if (kind == LLAM_IO_CONTROL_REQ_CANCEL) {
        llam_io_req_t *req = target;
        llam_task_t *task;

        if (req == NULL || req->owner_runtime != node->runtime) {
            errno = EINVAL;
            free(op);
            return -1;
        }
        if (!llam_io_req_lifetime_try_acquire(req)) {
            free(op);
            return -1;
        }
        op->holds_request_ref = true;
        task = req->task;
        if (task != NULL && req == &task->embedded_io_req) {
            if (task->owner_runtime != node->runtime) {
                llam_io_control_op_destroy(node, op);
                errno = EXDEV;
                return -1;
            }
            if (!llam_task_scan_ref_try_acquire(node->runtime, task)) {
                int saved_errno = errno;

                llam_io_control_op_destroy(node, op);
                errno = saved_errno;
                return -1;
            }
            op->task_ref = task;
            op->holds_task_ref = true;
        }
    }
    if (node->control_tail != NULL) {
        node->control_tail->next = op;
    } else {
        node->control_head = op;
    }
    node->control_tail = op;
    return 0;
}
