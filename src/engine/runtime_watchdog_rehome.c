/**
 * @file src/engine/runtime_watchdog_rehome.c
 * @brief Watchdog-driven I/O watch rehoming and idle-node balancing helpers.
 *
 * @details
 * Rehoming helpers move parked wait ownership away from a shard that is being
 * offlined. They cover regular sync waits, in-flight I/O waits, submit-queue
 * waits, multishot watch waiters, and cross-node watch migration state.
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

#include "runtime_watchdog_internal.h"

/**
 * @brief Decide whether source shard timers can be merged.
 *
 * Timer heaps are not migrated yet; a shard with active timers, or expired
 * timers that have been popped but not fully resolved, must remain online until
 * the timer path drains.
 *
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output, always set to 0.
 *
 * @return @c true only when the source has no timers.
 */
bool llam_merge_shard_timers_locked(llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source == NULL || target == NULL || source == target) {
        return false;
    }
    // Timer nodes are heap-backed; do not migrate them with the old linked-list path.
    return source->timer_heap_len == 0U &&
           source->timers == NULL &&
           atomic_load_explicit(&source->timer_callbacks_active, memory_order_acquire) == 0U;
}

/**
 * @brief Check whether a parked non-I/O wait can move to another shard.
 *
 * @param source Source shard.
 * @param target Target shard.
 * @param task   Task to inspect.
 *
 * @return @c true when the task is parked on @p source and can be rehomed.
 */
static bool llam_task_can_rehome_parked_wait(const llam_shard_t *source, const llam_shard_t *target, const llam_task_t *task) {
    if (source == NULL || target == NULL || task == NULL) {
        return false;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) != 0U) {
        return false;
    }
    if (task->state != LLAM_TASK_STATE_PARKED || task->parked_shard != source->id) {
        return false;
    }
    if (llam_task_active_io_req_load(task) != NULL) {
        // I/O waits have backend-owned state and use separate paths.
        return false;
    }
    /*
     * Do not inspect task->active_timer here.  Timer expiry mutates it under
     * the source shard lock, while this scanner walks task lists under each
     * task-owner shard lock.  The offlining path already proves source timers
     * are quiescent via llam_merge_shard_timers_locked(), including popped but
     * unresolved callbacks.
     */
    return atomic_load_explicit(&task->active_wait_node, memory_order_acquire) != NULL ||
           atomic_load_explicit(&task->join_target, memory_order_acquire) != NULL ||
           llam_task_active_block_job_load(task) != NULL;
}

/**
 * @brief Rehome parked non-I/O waiters from source to target.
 *
 * @param rt           Runtime containing the task list.
 * @param source       Source shard being offlined.
 * @param target       Target shard receiving parked waiters.
 * @param migrated_out Optional migrated-count output.
 */
void llam_rehome_parked_waiters(llam_runtime_t *rt, llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    llam_task_t *task;
    unsigned migrated = 0U;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (rt == NULL || source == NULL || target == NULL || source == target) {
        return;
    }

    for (unsigned i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *owner = &rt->shards[i];

        pthread_mutex_lock(&owner->lock);
        for (task = owner->all_tasks; task != NULL; task = task->all_next) {
            // The wait structure keeps the same node; only the resume shard changes.
            if (!llam_task_can_rehome_parked_wait(source, target, task)) {
                continue;
            }
            task->parked_shard = target->id;
            llam_merge_rehome_task(source, target, task);
            migrated += 1U;
        }
        pthread_mutex_unlock(&owner->lock);
    }

    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    source->metrics.migrations += migrated;
}

/**
 * @brief Return the number of I/O requests currently owned in-flight by a shard.
 *
 * @param shard Shard to inspect.
 *
 * @return In-flight I/O waiter count.
 */
unsigned llam_shard_inflight_io_waiters(const llam_shard_t *shard) {
    if (shard == NULL) {
        return 0U;
    }
    return atomic_load_explicit(&((llam_shard_t *)shard)->inflight_io_waiters, memory_order_acquire);
}

/**
 * @brief Check whether a task/request pair can be rehomed for a given I/O wait mode.
 *
 * @param source        Source shard.
 * @param target        Target shard.
 * @param task          Parked task.
 * @param req           Active I/O request.
 * @param expected_mode Required request wait mode.
 *
 * @return @c true when all ownership fields match the source shard.
 */
static bool llam_task_can_rehome_io_wait(const llam_shard_t *source,
                                       const llam_shard_t *target,
                                       const llam_task_t *task,
                                       const llam_io_req_t *req,
                                       llam_io_wait_mode_t expected_mode) {
    if (source == NULL || target == NULL || task == NULL || req == NULL) {
        return false;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) != 0U) {
        return false;
    }
    if (task->state != LLAM_TASK_STATE_PARKED ||
        (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire) != LLAM_WAIT_IO ||
        task->parked_shard != source->id) {
        return false;
    }
    if (llam_task_active_io_req_load(task) != req || req->owner_shard != source->id) {
        return false;
    }
    // Wait mode guards against moving a request from the wrong backend list.
    return atomic_load_explicit(&req->wait_mode, memory_order_acquire) == (unsigned)expected_mode;
}

/**
 * @brief Rewrite task and request ownership to the target shard.
 *
 * @param source Source shard.
 * @param target Target shard.
 * @param req    Request whose task should be rehomed.
 */
static void llam_rehome_io_wait_req(llam_shard_t *source, llam_shard_t *target, llam_io_req_t *req) {
    llam_task_t *task;

    if (source == NULL || target == NULL || req == NULL || req->task == NULL) {
        return;
    }

    task = req->task;
    // Completion paths read both task->parked_shard and req->owner_shard.
    task->parked_shard = target->id;
    req->owner_shard = target->id;
    llam_merge_rehome_task(source, target, task);
}

/**
 * @brief Rehome in-flight I/O waiters owned by a source shard.
 *
 * In-flight requests also transfer their atomic in-flight owner so completion
 * dispatch reinjects the task on the new shard.
 *
 * @param rt           Runtime containing the task list.
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output.
 */
void llam_rehome_inflight_io_waiters(llam_runtime_t *rt, llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    llam_task_t *task;
    unsigned migrated = 0U;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (rt == NULL || source == NULL || target == NULL || source == target) {
        return;
    }

    for (unsigned i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *owner = &rt->shards[i];

        pthread_mutex_lock(&owner->lock);
        for (task = owner->all_tasks; task != NULL; task = task->all_next) {
            llam_io_req_t *req = llam_task_active_io_req_load(task);

            if (!llam_task_can_rehome_io_wait(source, target, task, req, LLAM_IO_WAIT_MODE_INFLIGHT)) {
                continue;
            }
            // In-flight completions race with rehome; transfer only if owner still matches.
            if (!llam_io_req_transfer_inflight_owner(req, source->id, target->id)) {
                continue;
            }
            task->parked_shard = target->id;
            req->owner_shard = target->id;
            llam_merge_rehome_task(source, target, task);
            migrated += 1U;
        }
        pthread_mutex_unlock(&owner->lock);
    }

    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    source->metrics.migrations += migrated;
}

/**
 * @brief Rehome every request in a linked I/O waiter list for one wait mode.
 *
 * The list is validated first; any non-rehomeable source-owned request aborts
 * the whole migration so ownership does not become partially inconsistent.
 *
 * @param source        Source shard.
 * @param target        Target shard.
 * @param head          Request-list head.
 * @param expected_mode Required wait mode.
 * @param migrated_out  Optional migrated-count output.
 *
 * @return @c true when the list was valid and migrated.
 */
static bool llam_rehome_io_waiter_list(llam_shard_t *source,
                                     llam_shard_t *target,
                                     llam_io_req_t *head,
                                     llam_io_wait_mode_t expected_mode,
                                     unsigned *migrated_out) {
    llam_io_req_t *req;
    unsigned migrated = 0U;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source == NULL || target == NULL) {
        return false;
    }

    // Validate the whole list first so a single bad waiter cannot leave a
    // partially rehomed submit/watch queue behind.
    for (req = head; req != NULL; req = req->next) {
        if (req->owner_shard != source->id) {
            continue;
        }
        if (!llam_task_can_rehome_io_wait(source, target, req->task, req, expected_mode)) {
            return false;
        }
    }

    for (req = head; req != NULL; req = req->next) {
        if (req->owner_shard != source->id) {
            continue;
        }
        llam_rehome_io_wait_req(source, target, req);
        migrated += 1U;
    }

    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    return true;
}

/**
 * @brief Check whether all waiters in a list are owned by one shard.
 *
 * @param head     Request-list head.
 * @param shard_id Expected owner shard id.
 *
 * @return @c true when the non-empty list is fully owned by @p shard_id.
 */
static bool llam_io_waiters_all_owned_by_shard(const llam_io_req_t *head, unsigned shard_id) {
    const llam_io_req_t *req;

    if (head == NULL) {
        return false;
    }
    for (req = head; req != NULL; req = req->next) {
        if (req->owner_shard != shard_id) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Rehome requests waiting in a node submit queue.
 *
 * @param node         I/O node containing the submit queue.
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true when submit waiters were safely rehomed.
 */
bool llam_rehome_node_submit_waiters(llam_node_t *node, llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    bool ok;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (node == NULL || source == NULL || target == NULL) {
        return false;
    }

    pthread_mutex_lock(&node->submit_lock);
    ok = llam_rehome_io_waiter_list(source, target, node->submit_head, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE, migrated_out);
    pthread_mutex_unlock(&node->submit_lock);
    if (ok && migrated_out != NULL && *migrated_out > 0U) {
        source->metrics.migrations += *migrated_out;
    }
    return ok;
}

/**
 * @brief Check whether a target I/O node can submit a request.
 *
 * @param node Candidate target node.
 * @param req  Request to inspect.
 *
 * @return @c true when the node supports the request kind.
 */
static bool llam_node_supports_submit_req(const llam_node_t *node, const llam_io_req_t *req) {
    if (node == NULL || req == NULL) {
        return false;
    }
    if (!node->ring_ready) {
        return false;
    }
    if (req->kind == LLAM_IO_KIND_READ && req->use_recv_op) {
        return node->supports_recv;
    }
    return llam_node_supports_kind(node, req->kind);
}

/**
 * @brief Move already-rehomed submit waiters to their target I/O node.
 *
 * Used when a shard merge crosses I/O-node boundaries. Requests already owned by
 * the target shard but still queued on the source node are removed and queued on
 * the target node.
 *
 * @param source_node  Node currently holding queued requests.
 * @param target_node  Node that should submit them after rehome.
 * @param source_shard Original source shard for metrics.
 * @param target_shard Target shard that now owns the requests.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true when evacuation completed safely.
 */
bool llam_evacuate_rehomed_submit_waiters(llam_node_t *source_node,
                                               llam_node_t *target_node,
                                               llam_shard_t *source_shard,
                                               llam_shard_t *target_shard,
                                               unsigned *migrated_out) {
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur;
    unsigned migrated = 0U;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source_node == NULL || target_node == NULL || source_shard == NULL || target_shard == NULL ||
        source_node == target_node) {
        return false;
    }

    first = source_node->index < target_node->index ? source_node : target_node;
    second = first == source_node ? target_node : source_node;
    pthread_mutex_lock(&first->submit_lock);
    pthread_mutex_lock(&second->submit_lock);
    source_locked = first == source_node ? first : second;
    target_locked = source_locked == source_node ? target_node : source_node;

    for (cur = source_locked->submit_head; cur != NULL; cur = cur->next) {
        llam_task_t *task = cur->task;

        if (cur->owner_shard != target_shard->id || cur->attached_node_index != source_locked->index) {
            continue;
        }
        if (task == NULL || task->state != LLAM_TASK_STATE_PARKED ||
            (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire) != LLAM_WAIT_IO ||
            task->parked_shard != target_shard->id || llam_task_active_io_req_load(task) != cur ||
            atomic_load_explicit(&cur->wait_mode, memory_order_acquire) != (unsigned)LLAM_IO_WAIT_MODE_SUBMIT_QUEUE ||
            !llam_node_supports_submit_req(target_locked, cur)) {
            pthread_mutex_unlock(&second->submit_lock);
            pthread_mutex_unlock(&first->submit_lock);
            return false;
        }
    }

    cur = source_locked->submit_head;
    while (cur != NULL) {
        llam_io_req_t *next = cur->next;

        if (cur->owner_shard != target_shard->id || cur->attached_node_index != source_locked->index) {
            prev = cur;
            cur = next;
            continue;
        }
        if (prev != NULL) {
            prev->next = next;
        } else {
            source_locked->submit_head = next;
        }
        if (source_locked->submit_tail == cur) {
            source_locked->submit_tail = prev;
        }
        cur->next = NULL;
        // The request is already target-shard-owned; now move node ownership too.
        cur->attached_node_index = target_locked->index;
        llam_queue_node_submit_locked(target_locked, cur);
        migrated += 1U;
        cur = next;
    }

    pthread_mutex_unlock(&second->submit_lock);
    pthread_mutex_unlock(&first->submit_lock);

    if (migrated > 0U) {
        if (!llam_node_complete_pending_ops(source_node, migrated) ||
            !llam_node_note_pending_ops(target_node, migrated)) {
            return false;
        }
        source_shard->metrics.migrations += migrated;
    }
    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    return true;
}

/**
 * @brief Check whether a node still owns watch/control state that blocks migration.
 *
 * @param node Node whose watch state is inspected. The watch lock must be held.
 *
 * @return @c true when non-transferred control or watch state remains.
 */
static bool llam_node_has_watch_state_locked(const llam_node_t *node) {
    const llam_io_control_op_t *control;
    const llam_poll_watch_t *poll_watch;
    const llam_accept_watch_t *accept_watch;
    const llam_recv_watch_t *recv_watch;

    if (node == NULL) {
        return false;
    }
    for (control = node->control_head; control != NULL; control = control->next) {
        bool transferred = false;

        switch (control->kind) {
        case LLAM_IO_CONTROL_POLL_ACTIVATE:
        case LLAM_IO_CONTROL_POLL_DEACTIVATE:
            transferred = control->target != NULL && ((const llam_poll_watch_t *)control->target)->live_transferred;
            break;
        case LLAM_IO_CONTROL_ACCEPT_ACTIVATE:
        case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE:
            transferred = control->target != NULL && ((const llam_accept_watch_t *)control->target)->live_transferred;
            break;
        case LLAM_IO_CONTROL_RECV_ACTIVATE:
        case LLAM_IO_CONTROL_RECV_DEACTIVATE:
            transferred = control->target != NULL && ((const llam_recv_watch_t *)control->target)->live_transferred;
            break;
        case LLAM_IO_CONTROL_REQ_CANCEL:
            transferred = true;
            break;
        default:
            transferred = false;
            break;
        }
        if (!transferred) {
            return true;
        }
    }

    for (poll_watch = node->poll_watches; poll_watch != NULL; poll_watch = poll_watch->next) {
        if (poll_watch->sticky_revents != 0 ||
            ((poll_watch->active || poll_watch->activating || poll_watch->deactivate_queued) &&
             poll_watch->wait_head == NULL &&
             !poll_watch->live_transferred)) {
            return true;
        }
    }
    for (accept_watch = node->accept_watches; accept_watch != NULL; accept_watch = accept_watch->next) {
        if (accept_watch->ready_head != NULL ||
            ((accept_watch->active || accept_watch->activating || accept_watch->deactivate_queued) &&
             accept_watch->wait_head == NULL &&
             !accept_watch->live_transferred)) {
            return true;
        }
    }
    for (recv_watch = node->recv_watches; recv_watch != NULL; recv_watch = recv_watch->next) {
        if (recv_watch->ready_head != NULL ||
            ((recv_watch->active || recv_watch->activating || recv_watch->deactivate_queued) &&
             recv_watch->wait_head == NULL &&
             !recv_watch->live_transferred)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Rehome waiters attached to every watch list on one node.
 *
 * Multishot watches whose waiters now all belong to the target shard may be
 * marked for live transfer to the target shard's owning I/O node.
 *
 * @param node         Node containing watch lists.
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true when all watch waiters were safely rehomed.
 */
static bool llam_rehome_node_watch_waiters(llam_node_t *node, llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    llam_poll_watch_t *poll_watch;
    llam_accept_watch_t *accept_watch;
    llam_recv_watch_t *recv_watch;
    unsigned migrated = 0U;
    unsigned target_node_index;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (node == NULL || source == NULL || target == NULL) {
        return false;
    }

    pthread_mutex_lock(&node->watch_lock);
    for (poll_watch = node->poll_watches; poll_watch != NULL; poll_watch = poll_watch->next) {
        bool target_owned = llam_io_waiters_all_owned_by_shard(poll_watch->wait_head, target->id);

        if (!llam_rehome_io_waiter_list(source, target, poll_watch->wait_head, LLAM_IO_WAIT_MODE_POLL_WATCH, &migrated)) {
            pthread_mutex_unlock(&node->watch_lock);
            return false;
        }
        target_node_index = llam_multishot_owner_node_index(node->runtime, target->io_node_index, poll_watch->fd);
        target_owned = llam_io_waiters_all_owned_by_shard(poll_watch->wait_head, target->id);
        if (poll_watch->wait_head != NULL &&
            node->index != target_node_index &&
            target_owned) {
            // The watch is live on this node but every waiter now belongs to the target shard.
            poll_watch->migrate_target_node_index = target_node_index;
            poll_watch->live_transferred = true;
        } else if (poll_watch->wait_head != NULL) {
            poll_watch->migrate_target_node_index = UINT_MAX;
            poll_watch->live_transferred = false;
        }
        if (migrated_out != NULL) {
            *migrated_out += migrated;
        }
        migrated = 0U;
    }
    for (accept_watch = node->accept_watches; accept_watch != NULL; accept_watch = accept_watch->next) {
        bool target_owned = llam_io_waiters_all_owned_by_shard(accept_watch->wait_head, target->id);

        if (!llam_rehome_io_waiter_list(source, target, accept_watch->wait_head, LLAM_IO_WAIT_MODE_ACCEPT_WATCH, &migrated)) {
            pthread_mutex_unlock(&node->watch_lock);
            return false;
        }
        target_node_index = llam_multishot_owner_node_index(node->runtime, target->io_node_index, accept_watch->fd);
        target_owned = llam_io_waiters_all_owned_by_shard(accept_watch->wait_head, target->id);
        if (accept_watch->wait_head != NULL &&
            node->index != target_node_index &&
            target_owned) {
            // Mark for backend-assisted live migration instead of moving state under this lock.
            accept_watch->migrate_target_node_index = target_node_index;
            accept_watch->live_transferred = true;
        } else if (accept_watch->wait_head != NULL) {
            accept_watch->migrate_target_node_index = UINT_MAX;
            accept_watch->live_transferred = false;
        }
        if (migrated_out != NULL) {
            *migrated_out += migrated;
        }
        migrated = 0U;
    }
    for (recv_watch = node->recv_watches; recv_watch != NULL; recv_watch = recv_watch->next) {
        bool target_owned = llam_io_waiters_all_owned_by_shard(recv_watch->wait_head, target->id);

        if (!llam_rehome_io_waiter_list(source, target, recv_watch->wait_head, LLAM_IO_WAIT_MODE_RECV_WATCH, &migrated)) {
            pthread_mutex_unlock(&node->watch_lock);
            return false;
        }
        target_node_index = llam_multishot_owner_node_index(node->runtime, target->io_node_index, recv_watch->fd);
        target_owned = llam_io_waiters_all_owned_by_shard(recv_watch->wait_head, target->id);
        if (recv_watch->wait_head != NULL &&
            node->index != target_node_index &&
            target_owned) {
            // Provided-buffer/multishot recv state migrates asynchronously.
            recv_watch->migrate_target_node_index = target_node_index;
            recv_watch->live_transferred = true;
        } else if (recv_watch->wait_head != NULL) {
            recv_watch->migrate_target_node_index = UINT_MAX;
            recv_watch->live_transferred = false;
        }
        if (migrated_out != NULL) {
            *migrated_out += migrated;
        }
        migrated = 0U;
    }
    pthread_mutex_unlock(&node->watch_lock);

    if (migrated_out != NULL && *migrated_out > 0U) {
        source->metrics.migrations += *migrated_out;
    }
    return true;
}

/**
 * @brief Rehome watch waiters across the runtime.
 *
 * Single-node configurations only inspect the source shard's I/O node. Multinode
 * multishot configurations inspect every node because live watches may already
 * have moved independently from shard ownership.
 *
 * @param rt           Runtime containing I/O nodes.
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true when all relevant watch waiters were rehomed.
 */
bool llam_rehome_runtime_watch_waiters(llam_runtime_t *rt,
                                            llam_shard_t *source,
                                            llam_shard_t *target,
                                            unsigned *migrated_out) {
    unsigned migrated = 0U;
    unsigned i;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (rt == NULL || source == NULL || target == NULL) {
        return false;
    }

    if (rt->experimental_shard_rings_multishot == 0U || rt->active_nodes <= 1U) {
        return llam_rehome_node_watch_waiters(&rt->nodes[source->io_node_index], source, target, migrated_out);
    }

    for (i = 0U; i < rt->active_nodes; ++i) {
        unsigned node_migrated = 0U;

        if (!llam_rehome_node_watch_waiters(&rt->nodes[i], source, target, &node_migrated)) {
            return false;
        }
        migrated += node_migrated;
    }

    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    return true;
}

/**
 * @brief Check whether a node still contains cross-node watch state.
 *
 * @param node Node to inspect.
 *
 * @return @c true when watch/control state remains.
 */
static bool llam_node_has_cross_io_state(llam_node_t *node) {
    bool has_watch_state;

    if (node == NULL) {
        return false;
    }

    pthread_mutex_lock(&node->watch_lock);
    has_watch_state = llam_node_has_watch_state_locked(node);
    pthread_mutex_unlock(&node->watch_lock);
    return has_watch_state;
}

/**
 * @brief Check whether a node can still make asynchronous watch-migration progress.
 *
 * @param node Node to inspect.
 *
 * @return @c true when controls or pending operations may still complete.
 */
static bool llam_node_has_cross_io_async_progress(llam_node_t *node) {
    bool has_controls = false;

    if (node == NULL) {
        return false;
    }

    pthread_mutex_lock(&node->watch_lock);
    has_controls = node->control_head != NULL;
    pthread_mutex_unlock(&node->watch_lock);
    if (has_controls) {
        return true;
    }
    return atomic_load_explicit(&node->pending_ops, memory_order_acquire) > 0U;
}

/**
 * @brief Wait for cross-I/O-node watch state to quiesce after rehome.
 *
 * The function repeatedly asks the backend to move idle watch state, kicks the
 * source node while async progress is possible, and stops at @p deadline_ns.
 *
 * @param source      Source I/O node.
 * @param target      Target I/O node.
 * @param deadline_ns Absolute monotonic deadline.
 *
 * @return @c true when no blocking watch state remains on @p source.
 */
bool llam_quiesce_cross_io_watch_state(llam_node_t *source, llam_node_t *target, uint64_t deadline_ns) {
    if (source == NULL || target == NULL || source == target) {
        return false;
    }

    for (;;) {
        if (!llam_io_rehome_idle_watch_state(source, target)) {
            return false;
        }
        if (!llam_node_has_cross_io_state(source)) {
            return true;
        }
        if (llam_now_ns() >= deadline_ns || !llam_node_has_cross_io_async_progress(source)) {
            break;
        }
        // Give the source I/O worker a chance to process queued migration controls.
        llam_kick_node(source);
        llam_watchdog_pause_briefly();
    }

    if (!llam_io_rehome_idle_watch_state(source, target)) {
        return false;
    }
    return !llam_node_has_cross_io_state(source);
}
