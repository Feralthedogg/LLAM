/**
 * @file src/io/runtime_io_watch_rehome_template.inc
 * @brief Shared multishot watch rehome algorithm template.
 *
 * @details
 * Linux and Darwin use the same watch-list migration state machine. Backend
 * completion payloads differ, so platform files provide the completion types,
 * push helpers, and drain helpers through the macros checked below before
 * including this file. The accept/recv phases live in separate include
 * fragments to keep each template focused and auditable.
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

#ifndef LLAM_REHOME_POLL_COMPLETION_T
#error "LLAM_REHOME_POLL_COMPLETION_T must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_ACCEPT_COMPLETION_T
#error "LLAM_REHOME_ACCEPT_COMPLETION_T must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_RECV_COMPLETION_T
#error "LLAM_REHOME_RECV_COMPLETION_T must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_POLL_COMPLETION_PUSH
#error "LLAM_REHOME_POLL_COMPLETION_PUSH must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_ACCEPT_COMPLETION_PUSH
#error "LLAM_REHOME_ACCEPT_COMPLETION_PUSH must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY
#error "LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_POLL_COMPLETION_DRAIN
#error "LLAM_REHOME_POLL_COMPLETION_DRAIN must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_ACCEPT_COMPLETION_DRAIN
#error "LLAM_REHOME_ACCEPT_COMPLETION_DRAIN must be defined before including runtime_io_watch_rehome_template.inc"
#endif
#ifndef LLAM_REHOME_RECV_COMPLETION_DRAIN
#error "LLAM_REHOME_RECV_COMPLETION_DRAIN must be defined before including runtime_io_watch_rehome_template.inc"
#endif

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
        LLAM_REHOME_POLL_COMPLETION_T *poll_completions = NULL;
        LLAM_REHOME_POLL_COMPLETION_T *poll_completion_tail = NULL;
        LLAM_REHOME_ACCEPT_COMPLETION_T *accept_completions = NULL;
        LLAM_REHOME_ACCEPT_COMPLETION_T *accept_completion_tail = NULL;
        LLAM_REHOME_RECV_COMPLETION_T *recv_completions = NULL;
        LLAM_REHOME_RECV_COMPLETION_T *recv_completion_tail = NULL;
        bool kick_source = false;
        bool kick_target = false;
        llam_node_t *source_locked = NULL;
        llam_node_t *target_locked = NULL;
        bool ok = true;

        llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);

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
                    llam_poll_watch_note_waiter_migration(watch, desired_target_index, source_locked->index);
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
                    watch->migrate_target_node_index =
                        llam_watch_migration_target_or_none(desired_target_index, source_locked->index);
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

                            if (!LLAM_REHOME_POLL_COMPLETION_PUSH(&poll_completions, &poll_completion_tail, waiters, revents)) {
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
                    llam_poll_watch_mark_live_migration(watch, desired_target_index, source_locked->index);
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
#include "runtime_io_watch_rehome_accept_template.inc"
        }

        if (ok && deferred_kind == LLAM_DEFER_NONE) {
#include "runtime_io_watch_rehome_recv_template.inc"
        }

        llam_io_unlock_rehome_pair(source, target);

        if (kick_source) {
            llam_kick_node(source);
        }
        if (kick_target) {
            llam_kick_node(target);
        }
        LLAM_REHOME_POLL_COMPLETION_DRAIN(target, poll_completions);
        LLAM_REHOME_ACCEPT_COMPLETION_DRAIN(target, accept_completions);
        LLAM_REHOME_RECV_COMPLETION_DRAIN(target, recv_completions);

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
