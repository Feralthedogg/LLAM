/**
 * @file src/io/watch/migration.c
 * @brief Shared I/O watch migration and shutdown-control helpers.
 *
 * @details
 * Dynamic node rehome can be requested from watchdog and backend worker paths.
 * This file centralizes the lock ordering and live-transfer bookkeeping used by
 * both Linux and Darwin watch backends.
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

void llam_io_lock_rehome_pair(llam_node_t *source,
                              llam_node_t *target,
                              llam_node_t **source_locked,
                              llam_node_t **target_locked) {
    llam_node_t *first;
    llam_node_t *second;

    if (source == NULL || target == NULL || source_locked == NULL || target_locked == NULL) {
        return;
    }

    /*
     * Rehome can be initiated by watchdog and worker paths in opposite
     * directions. Locking by stable node index is the shared deadlock rule for
     * all platform backends; callers still receive source/target in semantic
     * order after both locks are held.
     */
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    *source_locked = source;
    *target_locked = target;
}

void llam_io_unlock_rehome_pair(llam_node_t *source, llam_node_t *target) {
    llam_node_t *first;
    llam_node_t *second;

    if (source == NULL || target == NULL) {
        return;
    }

    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
}

unsigned llam_watch_migration_target_index(llam_runtime_t *rt,
                                           unsigned fallback_target_index,
                                           unsigned current_target_index,
                                           llam_fd_t fd) {
    if (current_target_index != UINT_MAX) {
        return current_target_index;
    }
    return llam_multishot_owner_node_index(rt, fallback_target_index, fd);
}

unsigned llam_watch_migration_target_or_none(unsigned desired_target_index, unsigned source_index) {
    return desired_target_index != source_index ? desired_target_index : UINT_MAX;
}

static void llam_watch_note_waiter_migration_common(unsigned *target_slot,
                                                    bool *live_transferred,
                                                    unsigned desired_target_index,
                                                    unsigned source_index) {
    if (target_slot == NULL || live_transferred == NULL) {
        return;
    }

    /*
     * A watch with live waiters cannot be structurally moved. If a migration is
     * already requested, refresh the destination and let backend completions
     * forward future readiness; otherwise keep ownership on the current node.
     */
    if (*target_slot != UINT_MAX) {
        *target_slot = desired_target_index;
        *live_transferred = desired_target_index != source_index;
    } else {
        *live_transferred = false;
    }
}

static void llam_watch_mark_live_migration_common(unsigned *target_slot,
                                                  bool *live_transferred,
                                                  unsigned desired_target_index,
                                                  unsigned source_index) {
    if (target_slot == NULL || live_transferred == NULL) {
        return;
    }

    *target_slot = llam_watch_migration_target_or_none(desired_target_index, source_index);
    *live_transferred = *target_slot != UINT_MAX;
}

void llam_poll_watch_note_waiter_migration(llam_poll_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index) {
    if (watch != NULL) {
        llam_watch_note_waiter_migration_common(&watch->migrate_target_node_index,
                                                &watch->live_transferred,
                                                desired_target_index,
                                                source_index);
    }
}

void llam_accept_watch_note_waiter_migration(llam_accept_watch_t *watch,
                                             unsigned desired_target_index,
                                             unsigned source_index) {
    if (watch != NULL) {
        llam_watch_note_waiter_migration_common(&watch->migrate_target_node_index,
                                                &watch->live_transferred,
                                                desired_target_index,
                                                source_index);
    }
}

void llam_recv_watch_note_waiter_migration(llam_recv_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index) {
    if (watch != NULL) {
        llam_watch_note_waiter_migration_common(&watch->migrate_target_node_index,
                                                &watch->live_transferred,
                                                desired_target_index,
                                                source_index);
    }
}

void llam_poll_watch_mark_live_migration(llam_poll_watch_t *watch,
                                         unsigned desired_target_index,
                                         unsigned source_index) {
    if (watch != NULL) {
        llam_watch_mark_live_migration_common(&watch->migrate_target_node_index,
                                              &watch->live_transferred,
                                              desired_target_index,
                                              source_index);
    }
}

void llam_accept_watch_mark_live_migration(llam_accept_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index) {
    if (watch != NULL) {
        llam_watch_mark_live_migration_common(&watch->migrate_target_node_index,
                                              &watch->live_transferred,
                                              desired_target_index,
                                              source_index);
    }
}

void llam_recv_watch_mark_live_migration(llam_recv_watch_t *watch,
                                         unsigned desired_target_index,
                                         unsigned source_index) {
    if (watch != NULL) {
        llam_watch_mark_live_migration_common(&watch->migrate_target_node_index,
                                              &watch->live_transferred,
                                              desired_target_index,
                                              source_index);
    }
}

void llam_io_queue_shutdown_controls_common(llam_node_t *node) {
    bool kicked = false;

    if (node == NULL) {
        return;
    }

    pthread_mutex_lock(&node->watch_lock);
#define LLAM_QUEUE_DEACTIVATE_WATCHES(head_, type_, kind_)                       \
    do {                                                                        \
        for (type_ *watch_ = (head_); watch_ != NULL; watch_ = watch_->next) {   \
            if (watch_->active && !watch_->deactivate_queued) {                 \
                watch_->deactivate_queued = true;                               \
                if (llam_node_queue_control_locked(node, (kind_), watch_) == 0) {\
                    kicked = true;                                              \
                } else {                                                        \
                    /* Keep shutdown retryable if control allocation fails. */   \
                    watch_->deactivate_queued = false;                          \
                }                                                               \
            }                                                                   \
        }                                                                       \
    } while (0)

    LLAM_QUEUE_DEACTIVATE_WATCHES(node->poll_watches, llam_poll_watch_t, LLAM_IO_CONTROL_POLL_DEACTIVATE);
    LLAM_QUEUE_DEACTIVATE_WATCHES(node->accept_watches, llam_accept_watch_t, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE);
    LLAM_QUEUE_DEACTIVATE_WATCHES(node->recv_watches, llam_recv_watch_t, LLAM_IO_CONTROL_RECV_DEACTIVATE);

#undef LLAM_QUEUE_DEACTIVATE_WATCHES
    pthread_mutex_unlock(&node->watch_lock);
    if (kicked) {
        llam_kick_node(node);
    }
}
