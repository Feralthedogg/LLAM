/**
 * @file src/io/runtime_io_watch_queue.c
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

void llam_shard_note_inflight_io_waiter(llam_runtime_t *rt, unsigned owner_shard, int delta) {
    if (rt == NULL || delta == 0 || owner_shard >= rt->active_shards) {
        return;
    }
    if (delta > 0) {
        atomic_fetch_add_explicit(&rt->shards[owner_shard].inflight_io_waiters,
                                  (unsigned)delta,
                                  memory_order_acq_rel);
    } else {
        atomic_fetch_sub_explicit(&rt->shards[owner_shard].inflight_io_waiters,
                                  (unsigned)(-delta),
                                  memory_order_acq_rel);
    }
}

static bool llam_node_complete_pending_op(llam_node_t *node, llam_runtime_t *rt) {
    unsigned pending;

    if (node == NULL) {
        return false;
    }

    pending = atomic_load_explicit(&node->pending_ops, memory_order_acquire);
    while (pending != 0U) {
        if (atomic_compare_exchange_weak_explicit(&node->pending_ops,
                                                  &pending,
                                                  pending - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }

    if (rt != NULL) {
        llam_record_fatal(rt, EINVAL);
    }
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
    if (decrement_pending && !llam_node_complete_pending_op(node, node_runtime)) {
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
            llam_record_fatal(node_runtime, EINVAL);
        }
        errno = EINVAL;
        return false;
    }
    if (LLAM_UNLIKELY(node_runtime == NULL || req->owner_runtime == NULL)) {
        if (node_runtime != NULL) {
            llam_record_fatal(node_runtime, EINVAL);
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
        llam_record_fatal(node_runtime, EXDEV);
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
    atomic_fetch_add(&node->pending_ops, 1U);
    if (llam_queue_node_submit_locked(node, req)) {
        pthread_mutex_unlock(&node->submit_lock);
        return true;
    }

    saved_errno = errno;
    atomic_fetch_sub(&node->pending_ops, 1U);
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
        atomic_store_explicit(&cursor->inflight_owner_shard,
                              cursor->owner_shard,
                              memory_order_release);
        atomic_store(&cursor->wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT);
        llam_shard_note_inflight_io_waiter(cursor->owner_runtime, cursor->owner_shard, 1);
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
    if (node->control_tail != NULL) {
        node->control_tail->next = op;
    } else {
        node->control_head = op;
    }
    node->control_tail = op;
    return 0;
}
