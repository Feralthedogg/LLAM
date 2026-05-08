/**
 * @file src/io/windows/runtime_io_watch_windows_state.c
 * @brief Windows IOCP submission/control queue state helpers.
 *
 * @details
 * This file owns backend-neutral queue mutation for the Windows IOCP watcher:
 * submit queue insertion/removal, control queue insertion/removal, and
 * in-flight owner accounting used by dynamic rehome logic.
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

#include "../runtime_io_api_internal.h"

void llam_shard_note_inflight_io_waiter(unsigned owner_shard, int delta) {
    llam_runtime_t *rt = &g_llam_runtime;

    if (delta == 0 || owner_shard >= rt->active_shards) {
        return;
    }
    if (delta > 0) {
        atomic_fetch_add_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)delta, memory_order_acq_rel);
    } else {
        atomic_fetch_sub_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)(-delta), memory_order_acq_rel);
    }
}

bool llam_io_req_transfer_inflight_owner(llam_io_req_t *req, unsigned from_shard, unsigned to_shard) {
    unsigned expected;

    if (req == NULL || from_shard == to_shard || from_shard >= g_llam_runtime.active_shards ||
        to_shard >= g_llam_runtime.active_shards) {
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

    llam_shard_note_inflight_io_waiter(from_shard, -1);
    llam_shard_note_inflight_io_waiter(to_shard, 1);
    return true;
}

void llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    if (node == NULL || req == NULL) {
        return;
    }
    req->next = NULL;
    if (node->submit_tail != NULL) {
        node->submit_tail->next = req;
    } else {
        node->submit_head = req;
    }
    node->submit_tail = req;
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

int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    llam_io_control_op_t *op;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    op = calloc(1, sizeof(*op));
    if (op == NULL) {
        errno = ENOMEM;
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

int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    int rc;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&node->watch_lock);
    rc = llam_node_queue_control_locked(node, kind, target);
    pthread_mutex_unlock(&node->watch_lock);
    if (rc == 0) {
        llam_kick_node(node);
    }
    return rc;
}

bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target) {
    llam_io_control_op_t *prev = NULL;
    llam_io_control_op_t *cur;

    if (node == NULL) {
        return false;
    }
    cur = node->control_head;
    while (cur != NULL) {
        if (cur->kind == kind && cur->target == target) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                node->control_head = cur->next;
            }
            if (node->control_tail == cur) {
                node->control_tail = prev;
            }
            free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}
