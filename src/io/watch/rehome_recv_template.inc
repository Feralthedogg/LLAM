/**
 * @file src/io/watch/rehome_recv_template.inc
 * @brief Receive-watch portion of the shared rehome template.
 *
 * @details
 * This file is included inside @c llam_io_rehome_watch_state_filtered after the
 * accept-watch phase. It uses the caller's local rehome state and backend
 * completion macros while preserving platform-specific receive payload
 * ownership in the including adapter.
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
        llam_recv_watch_note_waiter_migration(watch, desired_target_index, source_locked->index);
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
        watch->migrate_target_node_index =
            llam_watch_migration_target_or_none(desired_target_index, source_locked->index);
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
                    !LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY(&recv_completions,
                                                                 &recv_completion_tail,
                                                                 waiter,
                                                                 ready)) {
                    if (waiter != NULL) {
                        llam_recv_watch_push_waiter_front(target_watch, waiter);
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
        llam_recv_watch_mark_live_migration(watch, desired_target_index, source_locked->index);
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
