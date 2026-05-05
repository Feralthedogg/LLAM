/**
 * @file src/io/runtime_io_watch_darwin_migration_rehome.c
 * @brief Darwin I/O watch rehoming between nodes for load and idle balancing.
 *
 * @details
 * Rehome passes migrate idle or explicitly marked kqueue watch state between I/O
 * nodes. Active watches cannot be moved in place; they are marked for live
 * transfer, deactivated on the source, and finalized once the delete control
 * completes.
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

#include "runtime_io_watch_darwin_internal.h"

/** @brief Resolve the desired target node for a watch migration. */
unsigned nm_watch_migration_target_index(nm_runtime_t *rt,
                                                unsigned fallback_target_index,
                                                unsigned current_target_index,
                                                int fd) {
    if (current_target_index != UINT_MAX) {
        return current_target_index;
    }
    return nm_multishot_owner_node_index(rt, fallback_target_index, fd);
}

/**
 * @brief Move watch state from one Darwin node to another.
 *
 * @param source      Source node.
 * @param target      Target node attempted by this pass.
 * @param only_marked If true, move only watches already marked for migration.
 * @return true if the pass completed without unrecoverable error.
 */
bool nm_io_rehome_watch_state_filtered(nm_node_t *source, nm_node_t *target, bool only_marked) {
    if (source == NULL || target == NULL || source == target) {
        return false;
    }
    for (;;) {
        enum {
            NM_DEFER_NONE = 0,
            NM_DEFER_POLL = 1,
            NM_DEFER_ACCEPT = 2,
            NM_DEFER_RECV = 3,
        } deferred_kind = NM_DEFER_NONE;
        void *deferred_watch = NULL;
        unsigned deferred_target_index = UINT_MAX;
        nm_darwin_poll_completion_t *poll_completions = NULL;
        nm_darwin_poll_completion_t *poll_completion_tail = NULL;
        nm_darwin_accept_completion_t *accept_completions = NULL;
        nm_darwin_accept_completion_t *accept_completion_tail = NULL;
        nm_darwin_recv_completion_t *recv_completions = NULL;
        nm_darwin_recv_completion_t *recv_completion_tail = NULL;
        bool kick_source = false;
        bool kick_target = false;
        nm_node_t *first = source->index < target->index ? source : target;
        nm_node_t *second = first == source ? target : source;
        nm_node_t *source_locked;
        nm_node_t *target_locked;
        bool ok = true;

        pthread_mutex_lock(&first->watch_lock);
        pthread_mutex_lock(&second->watch_lock);
        // Always lock by node index because watchdog and worker paths may
        // initiate migration concurrently in opposite directions.
        source_locked = first == source ? first : second;
        target_locked = source_locked == source ? target : source;

        {
            nm_poll_watch_t **cursor = &source_locked->poll_watches;

            while (*cursor != NULL) {
                nm_poll_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = nm_watch_migration_target_index(source_locked->runtime,
                                                                       target_locked->index,
                                                                       watch->migrate_target_node_index,
                                                                       watch->fd);
                if (watch->wait_head != NULL) {
                    // Active waiters keep the watch on source for now. Mark live
                    // transfer so future events can be forwarded.
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
                    // Fully idle poll watch: destroy locally, defer to the true
                    // hash target, or recreate/arm on this target.
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_poll_watch_t *target_watch = nm_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL || !nm_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->sticky_revents == 0 &&
                    nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_POLL_ACTIVATE, watch)) {
                    // Activation has not reached the worker yet, so it can be
                    // canceled and recreated on the target.
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_poll_watch_t *target_watch = nm_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL || !nm_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->sticky_revents == 0) {
                    if (nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
                        // Deactivation was still queued; removing it makes the
                        // watch backend-idle immediately.
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_poll_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = NM_DEFER_POLL;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            nm_poll_watch_t *target_watch = nm_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                            if (target_watch == NULL || !nm_arm_poll_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_poll_watch_locked(source_locked, watch);
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
                    // Preserve sticky readiness by delivering to target waiters
                    // or merging into target sticky state.
                    if (desired_target_index == source_locked->index) {
                        ok = false;
                        break;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_POLL;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_poll_watch_t *target_watch =
                            nm_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        if (target_watch->wait_head != NULL) {
                            nm_io_req_t *waiters = nm_poll_watch_take_waiters(target_watch);
                            int revents = watch->sticky_revents | target_watch->sticky_revents;

                            if (!nm_darwin_poll_completion_push(&poll_completions, &poll_completion_tail, waiters, revents)) {
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
                        nm_destroy_poll_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->sticky_revents == 0) {
                    // Active backend state must be deactivated before final
                    // structural move.
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (nm_node_queue_control_locked(source_locked, NM_IO_CONTROL_POLL_DEACTIVATE, watch) != 0) {
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

        if (ok && deferred_kind == NM_DEFER_NONE) {
            nm_accept_watch_t **cursor = &source_locked->accept_watches;

            while (*cursor != NULL) {
                nm_accept_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = nm_watch_migration_target_index(source_locked->runtime,
                                                                       target_locked->index,
                                                                       watch->migrate_target_node_index,
                                                                       watch->fd);
                if (watch->wait_head != NULL) {
                    // Waiters remain on source until a future event/control
                    // completion can safely forward or finalize.
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
                        nm_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_accept_watch_t *target_watch = nm_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !nm_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL &&
                    nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_ACCEPT_ACTIVATE, watch)) {
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_accept_watch_t *target_watch = nm_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !nm_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    if (nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_ACCEPT_DEACTIVATE, watch)) {
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_accept_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = NM_DEFER_ACCEPT;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            nm_accept_watch_t *target_watch = nm_get_or_create_accept_watch_locked(target_locked, watch->fd);

                            if (target_watch == NULL || !nm_arm_accept_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_accept_watch_locked(source_locked, watch);
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
                        deferred_kind = NM_DEFER_ACCEPT;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_accept_watch_t *target_watch = nm_get_or_create_accept_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                            nm_accept_ready_t *ready = watch->ready_head;
                            nm_io_req_t *waiter = nm_accept_watch_pop_waiter(target_watch);

                            if (waiter == NULL ||
                                !nm_darwin_accept_completion_push(&accept_completions,
                                                                  &accept_completion_tail,
                                                                  waiter,
                                                                  ready->fd)) {
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
                        nm_destroy_accept_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->ready_head == NULL) {
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (nm_node_queue_control_locked(source_locked, NM_IO_CONTROL_ACCEPT_DEACTIVATE, watch) != 0) {
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

        if (ok && deferred_kind == NM_DEFER_NONE) {
            nm_recv_watch_t **cursor = &source_locked->recv_watches;

            while (*cursor != NULL) {
                nm_recv_watch_t *watch = *cursor;
                unsigned desired_target_index;

                if (only_marked && watch->migrate_target_node_index == UINT_MAX) {
                    cursor = &watch->next;
                    continue;
                }
                desired_target_index = nm_watch_migration_target_index(source_locked->runtime,
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
                        nm_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_recv_watch_t *target_watch = nm_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !nm_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    watch->activating &&
                    !watch->deactivate_queued &&
                    watch->ready_head == NULL &&
                    nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_RECV_ACTIVATE, watch)) {
                    watch->activating = false;
                    if (desired_target_index == source_locked->index) {
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                    if (desired_target_index != target_locked->index) {
                        watch->migrate_target_node_index = desired_target_index;
                        deferred_kind = NM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_recv_watch_t *target_watch = nm_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL || !nm_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                            ok = false;
                            break;
                        }
                        watch->migrate_target_node_index = UINT_MAX;
                        nm_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if (!watch->active &&
                    !watch->activating &&
                    watch->deactivate_queued &&
                    watch->ready_head == NULL) {
                    if (nm_drop_node_control_locked(source_locked, NM_IO_CONTROL_RECV_DEACTIVATE, watch)) {
                        watch->deactivate_queued = false;
                        if (desired_target_index == source_locked->index) {
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_recv_watch_locked(source_locked, watch);
                            continue;
                        }
                        if (desired_target_index != target_locked->index) {
                            watch->migrate_target_node_index = desired_target_index;
                            deferred_kind = NM_DEFER_RECV;
                            deferred_watch = watch;
                            deferred_target_index = desired_target_index;
                            break;
                        }
                        {
                            nm_recv_watch_t *target_watch = nm_get_or_create_recv_watch_locked(target_locked, watch->fd);

                            if (target_watch == NULL || !nm_arm_recv_watch_locked(target_locked, target_watch, &kick_target)) {
                                ok = false;
                                break;
                            }
                            watch->migrate_target_node_index = UINT_MAX;
                            nm_destroy_recv_watch_locked(source_locked, watch);
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
                        deferred_kind = NM_DEFER_RECV;
                        deferred_watch = watch;
                        deferred_target_index = desired_target_index;
                        break;
                    }
                    {
                        nm_recv_watch_t *target_watch = nm_get_or_create_recv_watch_locked(target_locked, watch->fd);

                        if (target_watch == NULL) {
                            ok = false;
                            break;
                        }
                        while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                            nm_recv_ready_t *ready = watch->ready_head;
                            nm_io_req_t *waiter = nm_recv_watch_pop_waiter(target_watch);

                            if (waiter == NULL ||
                                !nm_darwin_recv_completion_push(&recv_completions,
                                                                &recv_completion_tail,
                                                                waiter,
                                                                ready->size,
                                                                ready->copy_data,
                                                                ready->copy_capacity)) {
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
                            ready->copy_data = NULL;
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
                        nm_destroy_recv_watch_locked(source_locked, watch);
                        continue;
                    }
                }
                if ((watch->active || watch->deactivate_queued) && watch->ready_head == NULL) {
                    watch->migrate_target_node_index = desired_target_index != source_locked->index ? desired_target_index : UINT_MAX;
                    watch->live_transferred = watch->migrate_target_node_index != UINT_MAX;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (nm_node_queue_control_locked(source_locked, NM_IO_CONTROL_RECV_DEACTIVATE, watch) != 0) {
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
            nm_kick_node(source);
        }
        if (kick_target) {
            nm_kick_node(target);
        }
        nm_darwin_poll_completion_drain(target, poll_completions);
        nm_darwin_complete_accept_completions(target, accept_completions);
        nm_darwin_recv_completion_drain(target, recv_completions);

        if (!ok) {
            return false;
        }
        if (deferred_kind == NM_DEFER_NONE) {
            return true;
        }

        {
            bool deferred_kick = false;

            switch (deferred_kind) {
            case NM_DEFER_POLL:
                ok = nm_finalize_poll_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            case NM_DEFER_ACCEPT:
                ok = nm_finalize_accept_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            case NM_DEFER_RECV:
                ok = nm_finalize_recv_watch_migration(source, deferred_watch, deferred_target_index, &deferred_kick);
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                return false;
            }
            if (deferred_kick && deferred_target_index < source->runtime->active_nodes) {
                nm_kick_node(&source->runtime->nodes[deferred_target_index]);
            }
        }
    }
}

bool nm_io_rehome_idle_watch_state(nm_node_t *source, nm_node_t *target) {
    return nm_io_rehome_watch_state_filtered(source, target, false);
}

bool nm_io_rehome_marked_watch_state(nm_node_t *source, nm_node_t *target) {
    return nm_io_rehome_watch_state_filtered(source, target, true);
}

bool nm_io_req_transfer_inflight_owner(nm_io_req_t *req, unsigned from_shard, unsigned to_shard) {
    unsigned expected;

    if (req == NULL || from_shard == to_shard || from_shard >= g_nm_runtime.active_shards || to_shard >= g_nm_runtime.active_shards) {
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
    nm_shard_note_inflight_io_waiter(from_shard, -1);
    nm_shard_note_inflight_io_waiter(to_shard, 1);
    return true;
}
