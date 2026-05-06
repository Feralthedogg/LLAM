/**
 * @file src/io/linux/runtime_io_watch_linux_migration_rehome.c
 * @brief Linux I/O watch rehoming between nodes for load and idle balancing.
 *
 * @details
 * Rehoming moves idle or marked multishot watch state from a source node to a
 * target node. The implementation handles three watch families (poll, accept,
 * recv), preserves buffered readiness, and defers migration when a watch is
 * still active or its ideal target is not the currently supplied target.
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

#include "runtime_io_watch_linux_internal.h"

/**
 * @brief Resolve the desired target for a watch migration.
 *
 * @param rt                    Runtime owning nodes.
 * @param fallback_target_index Target supplied by caller.
 * @param current_target_index  Existing marked target, or UINT_MAX.
 * @param fd                    File descriptor used for ownership hashing.
 * @return Node index that should receive the watch.
 */
unsigned llam_watch_migration_target_index(llam_runtime_t *rt,
                                                unsigned fallback_target_index,
                                                unsigned current_target_index,
                                                int fd) {
    if (current_target_index != UINT_MAX) {
        return current_target_index;
    }
    return llam_multishot_owner_node_index(rt, fallback_target_index, fd);
}

/**
 * @brief Move watch state from one Linux node to another.
 *
 * @param source      Source node.
 * @param target      Target node attempted by this pass.
 * @param only_marked If true, move only watches with migrate_target_node_index.
 * @return true if the pass completed without an unrecoverable allocation/state
 *         failure.
 */
bool llam_io_rehome_watch_state_filtered(llam_node_t *source, llam_node_t *target, bool only_marked) {
    if (source == NULL || target == NULL || source == target) {
        return false;
    }
    for (;;) {
        enum {
            LLAM_DEFER_NONE = 0,
            LLAM_DEFER_POLL = 1,
            LLAM_DEFER_ACCEPT = 2,
            LLAM_DEFER_RECV = 3,
        } deferred_kind = LLAM_DEFER_NONE;
        void *deferred_watch = NULL;
        unsigned deferred_target_index = UINT_MAX;
        llam_poll_watch_completion_t *poll_completions = NULL;
        llam_poll_watch_completion_t *poll_completion_tail = NULL;
        llam_accept_watch_completion_t *accept_completions = NULL;
        llam_accept_watch_completion_t *accept_completion_tail = NULL;
        llam_recv_watch_completion_t *recv_completions = NULL;
        llam_recv_watch_completion_t *recv_completion_tail = NULL;
        bool kick_source = false;
        bool kick_target = false;
        llam_node_t *first = source->index < target->index ? source : target;
        llam_node_t *second = first == source ? target : source;
        llam_node_t *source_locked;
        llam_node_t *target_locked;
        bool ok = true;

        pthread_mutex_lock(&first->watch_lock);
        pthread_mutex_lock(&second->watch_lock);
        // Source/target locks are always taken by node index so concurrent
        // rehome attempts cannot deadlock.
        source_locked = first == source ? first : second;
        target_locked = source_locked == source ? target : source;

        {
            llam_poll_watch_t **cursor = &source_locked->poll_watches;

            while (*cursor != NULL) {
                llam_poll_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = llam_watch_migration_target_index(source_locked->runtime,
                                                                       target_locked->index,
                                                                       watch->migrate_target_node_index,
                                                                       watch->fd);
                if (watch->wait_head != NULL) {
                    // A watch with active waiters cannot be moved structurally.
                    // Mark it so live completions can forward future readiness.
                    if (watch->migrate_target_node_index != UINT_MAX) {
                        watch->migrate_target_node_index = desired_target_index;
                        watch->live_transferred = desired_target_index != source_locked->index;
                    } else {
                        watch->live_transferred = false;
                    }
                    cursor = &watch->next;
                    continue;
                }
                if (!watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued &&
                    watch->sticky_revents == 0) {
                    // Fully idle watch: either destroy it, defer to the true
                    // hashed target, or create/arm equivalent state on target.
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_poll_watch_t *target_watch = llam_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL || !llam_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->sticky_revents == 0 &&
                    llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_POLL_ACTIVATE, watch)) {
                    // Activation has not reached the backend yet, so it can be
                    // canceled locally and recreated on the target.
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_poll_watch_t *target_watch = llam_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL || !llam_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->sticky_revents == 0) {
                    if (llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
                        // Deactivation was still queued, so the watch is already
                        // backend-idle and can be moved immediately.
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_poll_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = LLAM_DEFER_POLL;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            llam_poll_watch_t *target_watch = llam_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                            if (target_watch == NULL || !llam_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_poll_watch_locked(source_locked, watch);
                            continue;
                        }
                    }
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    cursor = &watch->next;
                    continue;
                }
                if (watch->sticky_revents != 0 &&
                    !watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued) {
                    // Sticky readiness must be preserved. Deliver it to target
                    // waiters if possible, otherwise merge it into target state.
                    if (desired_target_index == source_locked->index) {
                        ok = false;
                        break;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_poll_watch_t *target_watch =
                            llam_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        if (target_watch->wait_head != NULL) {
                            llam_io_req_t *waiters = llam_poll_watch_take_waiters(target_watch);
                            int revents = watch->sticky_revents | target_watch->sticky_revents;

                            if (!llam_poll_watch_completion_push(&poll_completions, &poll_completion_tail, waiters, revents)) {
                                if (waiters != NULL) {
                                    target_watch->wait_head = waiters;
                                    target_watch->wait_tail = waiters;
                                    while (target_watch->wait_tail->next != NULL) {
                                        target_watch->wait_tail = target_watch->wait_tail->next;
                                    }
                                }
                                ok = false;
                                break;
                            }
                            target_watch->sticky_revents = 0;
                        } else {
                            target_watch->sticky_revents |= watch->sticky_revents;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->sticky_revents == 0) {
                    // Active backend state must first be deactivated on source.
                    // Finalization happens from the control completion path.
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (llam_node_queue_control_locked(source_locked, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch) != 0) {
                            watch->deactivate_queued = false;
                            watch->migrate_target_node_index = UINT_MAX;
                            watch->live_transferred = false;
                            ok = false;
                            break;
                        }
                        kick_source = true;
                    }
                }
                cursor = &watch->next;
            }
        }

        if (ok && deferred_kind == LLAM_DEFER_NONE) {
            llam_accept_watch_t **cursor = &source_locked->accept_watches;

            while (*cursor != NULL) {
                llam_accept_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = llam_watch_migration_target_index(source_locked->runtime,
                                                                       target_locked->index,
                                                                       watch->migrate_target_node_index,
                                                                       watch->fd);
                if (watch->wait_head != NULL) {
                    // Accept watch still has waiters; mark live transfer instead
                    // of moving the object under them.
                    if (watch->migrate_target_node_index != UINT_MAX) {
                        watch->migrate_target_node_index = desired_target_index;
                        watch->live_transferred = desired_target_index != source_locked->index;
                    } else {
                        watch->live_transferred = false;
                    }
                    cursor = &watch->next;
                    continue;
                }
                if (!watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    // Idle accept watches with no buffered fds can be recreated
                    // on the target without moving user-visible state.
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !llam_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL &&
                    llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_ACCEPT_ACTIVATE, watch)) {
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !llam_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    if (llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch)) {
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_accept_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = LLAM_DEFER_ACCEPT;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

                            if (target_watch == NULL || !llam_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_accept_watch_locked(source_locked, watch);
                            continue;
                        }
                    }
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    cursor = &watch->next;
                    continue;
                }
                if (watch->ready_head != NULL &&
                    !watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued) {
                    if (desired_target_index == source_locked->index) {
                        ok = false;
                        break;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                            llam_accept_ready_t *ready = watch->ready_head;
                            llam_io_req_t *waiter = llam_accept_watch_pop_waiter(target_watch);

                            if (waiter == NULL ||
                                !llam_accept_watch_completion_push(&accept_completions, &accept_completion_tail, waiter, ready->fd)) {
                                if (waiter != NULL) {
                                    waiter->next = target_watch->wait_head;
                                    target_watch->wait_head = waiter;
                                    if (target_watch->wait_tail == NULL) {
                                        target_watch->wait_tail = waiter;
                                    }
                                }
                                ok = false;
                                break;
                            }
                            watch->ready_head = ready->next;
                            if (watch->ready_head == NULL) {
                                watch->ready_tail = NULL;
                            }
                            watch->ready_depth -= 1U;
                            free(ready);
                        }
                        if (!ok) {
                            break;
                        }
                        if (watch->ready_head != NULL) {
                            if (target_watch->ready_tail != NULL) {
                                target_watch->ready_tail->next = watch->ready_head;
                            } else {
                                target_watch->ready_head = watch->ready_head;
                            }
                            target_watch->ready_tail = watch->ready_tail;
                            target_watch->ready_depth += watch->ready_depth;
                            watch->ready_head = NULL;
                            watch->ready_tail = NULL;
                            watch->ready_depth = 0U;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->ready_head == NULL) {
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (llam_node_queue_control_locked(source_locked, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch) != 0) {
                            watch->deactivate_queued = false;
                            watch->migrate_target_node_index = UINT_MAX;
                            watch->live_transferred = false;
                            ok = false;
                            break;
                        }
                        kick_source = true;
                    }
                }
                cursor = &watch->next;
            }
        }

        if (ok && deferred_kind == LLAM_DEFER_NONE) {
            llam_recv_watch_t **cursor = &source_locked->recv_watches;

            while (*cursor != NULL) {
                llam_recv_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = llam_watch_migration_target_index(source_locked->runtime,
                                                                       target_locked->index,
                                                                       watch->migrate_target_node_index,
                                                                       watch->fd);
                if (watch->wait_head != NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX) {
                        watch->migrate_target_node_index = desired_target_index;
                        watch->live_transferred = desired_target_index != source_locked->index;
                    } else {
                        watch->live_transferred = false;
                    }
                    cursor = &watch->next;
                    continue;
                }
                if (!watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !llam_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL &&
                    llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_RECV_ACTIVATE, watch)) {
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !llam_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    if (llam_drop_node_control_locked(source_locked, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch)) {
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_recv_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = LLAM_DEFER_RECV;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

                            if (target_watch == NULL || !llam_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            llam_destroy_recv_watch_locked(source_locked, watch);
                            continue;
                        }
                    }
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    cursor = &watch->next;
                    continue;
                }
                if (watch->ready_head != NULL &&
                    !watch->active &&
                    !watch->activating &&
                    !watch->deactivate_queued) {
                    if (desired_target_index == source_locked->index) {
                        ok = false;
                        break;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = LLAM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                            llam_recv_ready_t *ready = watch->ready_head;
                            llam_io_req_t *waiter = llam_recv_watch_pop_waiter(target_watch);

                            if (waiter == NULL ||
                                !llam_recv_watch_completion_push(&recv_completions,
                                                               &recv_completion_tail,
                                                               waiter,
                                                               ready->size,
                                                               ready->bid,
                                                               ready->has_buffer,
                                                               ready->node_index)) {
                                if (waiter != NULL) {
                                    waiter->next = target_watch->wait_head;
                                    target_watch->wait_head = waiter;
                                    if (target_watch->wait_tail == NULL) {
                                        target_watch->wait_tail = waiter;
                                    }
                                }
                                ok = false;
                                break;
                            }
                            watch->ready_head = ready->next;
                            if (watch->ready_head == NULL) {
                                watch->ready_tail = NULL;
                            }
                            watch->ready_depth -= 1U;
                            free(ready);
                        }
                        if (!ok) {
                            break;
                        }
                        if (watch->ready_head != NULL) {
                            if (target_watch->ready_tail != NULL) {
                                target_watch->ready_tail->next = watch->ready_head;
                            } else {
                                target_watch->ready_head = watch->ready_head;
                            }
                            target_watch->ready_tail = watch->ready_tail;
                            target_watch->ready_depth += watch->ready_depth;
                            watch->ready_head = NULL;
                            watch->ready_tail = NULL;
                            watch->ready_depth = 0U;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        llam_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->ready_head == NULL) {
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (llam_node_queue_control_locked(source_locked, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch) != 0) {
                            watch->deactivate_queued = false;
                            watch->migrate_target_node_index = UINT_MAX;
                            watch->live_transferred = false;
                            ok = false;
                            break;
                        }
                        kick_source = true;
                    }
                }
                cursor = &watch->next;
            }
        }

        pthread_mutex_unlock(&second->watch_lock);
        pthread_mutex_unlock(&first->watch_lock);

        if (kick_source) {
            llam_kick_node(source);
        }
        if (kick_target) {
            llam_kick_node(target);
        }
        llam_poll_watch_completion_drain(target, poll_completions);
        llam_accept_watch_completion_drain(target, accept_completions);
        llam_recv_watch_completion_drain(target->runtime, target, recv_completions);

        if (!ok) {
            return false;
        }
        if (deferred_kind == LLAM_DEFER_NONE) {
            return true;
        }

        {
            bool deferred_kick = false;

            switch (deferred_kind) {
            case LLAM_DEFER_POLL:
                ok = llam_finalize_poll_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            case LLAM_DEFER_ACCEPT:
                ok = llam_finalize_accept_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            case LLAM_DEFER_RECV:
                ok = llam_finalize_recv_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                return false;
            }
            if (deferred_kick && deferred_target_index < source->runtime->active_nodes) {
                llam_kick_node(&source->runtime->nodes[deferred_target_index]);
            }
        }
    }
}

bool llam_io_rehome_idle_watch_state(llam_node_t *source, llam_node_t *target) {
    return llam_io_rehome_watch_state_filtered(source, target, false);
}

bool llam_io_rehome_marked_watch_state(llam_node_t *source, llam_node_t *target) {
    return llam_io_rehome_watch_state_filtered(source, target, true);
}
