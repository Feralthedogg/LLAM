/**
 * @file src/io/runtime_io_watch_migration_live_finalize_template.inc
 * @brief Shared live watch migration finalization template.
 *
 * @details
 * Source watch finalization is identical across Linux and Darwin except for
 * deferred completion payload ownership. Platform files provide the completion
 * adapters before including this template.
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

#ifndef LLAM_LIVE_POLL_COMPLETION_T
#error "LLAM_LIVE_POLL_COMPLETION_T must be defined"
#endif
#ifndef LLAM_LIVE_ACCEPT_COMPLETION_T
#error "LLAM_LIVE_ACCEPT_COMPLETION_T must be defined"
#endif
#ifndef LLAM_LIVE_RECV_COMPLETION_T
#error "LLAM_LIVE_RECV_COMPLETION_T must be defined"
#endif
#ifndef LLAM_LIVE_POLL_COMPLETION_PUSH
#error "LLAM_LIVE_POLL_COMPLETION_PUSH must be defined"
#endif
#ifndef LLAM_LIVE_ACCEPT_COMPLETION_PUSH
#error "LLAM_LIVE_ACCEPT_COMPLETION_PUSH must be defined"
#endif
#ifndef LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY
#error "LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY must be defined"
#endif
#ifndef LLAM_LIVE_RECV_READY_COMPLETION_OWNED
#error "LLAM_LIVE_RECV_READY_COMPLETION_OWNED must be defined"
#endif
#ifndef LLAM_LIVE_POLL_COMPLETION_DRAIN
#error "LLAM_LIVE_POLL_COMPLETION_DRAIN must be defined"
#endif
#ifndef LLAM_LIVE_ACCEPT_COMPLETION_DRAIN
#error "LLAM_LIVE_ACCEPT_COMPLETION_DRAIN must be defined"
#endif
#ifndef LLAM_LIVE_RECV_COMPLETION_DRAIN
#error "LLAM_LIVE_RECV_COMPLETION_DRAIN must be defined"
#endif

/**
 * @brief Finish poll watch migration after source deactivation completes.
 *
 * @param source       Source node.
 * @param watch        Source watch that is no longer active.
 * @param target_index Target node index.
 * @param kick_target  Set true when target worker should be woken.
 * @return true if migration completed.
 */
bool llam_finalize_poll_watch_migration(llam_node_t *source,
                                        llam_poll_watch_t *watch,
                                        unsigned target_index,
                                        bool *kick_target) {
    LLAM_LIVE_POLL_COMPLETION_T *poll_completions = NULL;
    LLAM_LIVE_POLL_COMPLETION_T *poll_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
    bool ok = false;

    if (kick_target != NULL) {
        *kick_target = false;
    }
    if (source == NULL || watch == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
    if (watch->wait_head == NULL &&
        !watch->active &&
        !watch->activating &&
        !watch->deactivate_queued) {
        llam_poll_watch_t *target_watch = llam_get_or_create_poll_watch_locked(target_locked, watch->fd, watch->events);

        if (target_watch != NULL) {
            if (watch->sticky_revents != 0) {
                if (target_watch->wait_head != NULL) {
                    llam_io_req_t *waiters = llam_poll_watch_take_waiters(target_watch);
                    int revents = watch->sticky_revents | target_watch->sticky_revents;

                    if (!LLAM_LIVE_POLL_COMPLETION_PUSH(&poll_completions, &poll_completion_tail, waiters, revents)) {
                        llam_poll_watch_push_waiters_front(target_watch, waiters);
                    } else {
                        target_watch->sticky_revents = 0;
                        ok = true;
                    }
                } else {
                    target_watch->sticky_revents |= watch->sticky_revents;
                    ok = true;
                }
            } else {
                ok = llam_arm_poll_watch_locked(target_locked, target_watch, kick_target);
            }
        }
        if (ok) {
            watch->migrate_target_node_index = UINT_MAX;
            llam_destroy_poll_watch_locked(source_locked, watch);
        }
    }

    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_POLL_COMPLETION_DRAIN(target, poll_completions);
    return ok;
}

/**
 * @brief Finish accept watch migration after source deactivation completes.
 *
 * @return true if migration completed.
 */
bool llam_finalize_accept_watch_migration(llam_node_t *source,
                                          llam_accept_watch_t *watch,
                                          unsigned target_index,
                                          bool *kick_target) {
    LLAM_LIVE_ACCEPT_COMPLETION_T *accept_completions = NULL;
    LLAM_LIVE_ACCEPT_COMPLETION_T *accept_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
    bool ok = false;

    if (kick_target != NULL) {
        *kick_target = false;
    }
    if (source == NULL || watch == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
    if (watch->wait_head == NULL &&
        !watch->active &&
        !watch->activating &&
        !watch->deactivate_queued) {
        llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

        if (target_watch != NULL) {
            if (watch->ready_head != NULL) {
                while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                    llam_accept_ready_t *ready = watch->ready_head;
                    llam_io_req_t *waiter = llam_accept_watch_pop_waiter(target_watch);

                    if (waiter == NULL ||
                        !LLAM_LIVE_ACCEPT_COMPLETION_PUSH(&accept_completions,
                                                          &accept_completion_tail,
                                                          waiter,
                                                          ready->fd)) {
                        llam_accept_watch_push_waiter_front(target_watch, waiter);
                        break;
                    }
                    watch->ready_head = ready->next;
                    if (watch->ready_head == NULL) {
                        watch->ready_tail = NULL;
                    }
                    watch->ready_depth -= 1U;
                    free(ready);
                }
                llam_accept_watch_splice_ready(target_watch, watch);
                ok = true;
            } else {
                ok = llam_arm_accept_watch_locked(target_locked, target_watch, kick_target);
            }
        }
        if (ok) {
            watch->migrate_target_node_index = UINT_MAX;
            llam_destroy_accept_watch_locked(source_locked, watch);
        }
    }

    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_ACCEPT_COMPLETION_DRAIN(target, accept_completions);
    return ok;
}

/**
 * @brief Finish receive watch migration after source deactivation completes.
 *
 * @return true if migration completed.
 */
bool llam_finalize_recv_watch_migration(llam_node_t *source,
                                        llam_recv_watch_t *watch,
                                        unsigned target_index,
                                        bool *kick_target) {
    LLAM_LIVE_RECV_COMPLETION_T *recv_completions = NULL;
    LLAM_LIVE_RECV_COMPLETION_T *recv_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
    bool ok = false;

    if (kick_target != NULL) {
        *kick_target = false;
    }
    if (source == NULL || watch == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
    if (watch->wait_head == NULL &&
        !watch->active &&
        !watch->activating &&
        !watch->deactivate_queued) {
        llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

        if (target_watch != NULL) {
            if (watch->ready_head != NULL) {
                while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                    llam_recv_ready_t *ready = watch->ready_head;
                    llam_io_req_t *waiter = llam_recv_watch_pop_waiter(target_watch);

                    if (waiter == NULL ||
                        !LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY(&recv_completions,
                                                                   &recv_completion_tail,
                                                                   waiter,
                                                                   ready)) {
                        llam_recv_watch_push_waiter_front(target_watch, waiter);
                        break;
                    }
                    LLAM_LIVE_RECV_READY_COMPLETION_OWNED(ready);
                    watch->ready_head = ready->next;
                    if (watch->ready_head == NULL) {
                        watch->ready_tail = NULL;
                    }
                    watch->ready_depth -= 1U;
                    free(ready);
                }
                llam_recv_watch_splice_ready(target_watch, watch);
                ok = true;
            } else {
                ok = llam_arm_recv_watch_locked(target_locked, target_watch, kick_target);
            }
        }
        if (ok) {
            watch->migrate_target_node_index = UINT_MAX;
            llam_destroy_recv_watch_locked(source_locked, watch);
        }
    }

    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_RECV_COMPLETION_DRAIN(rt, target, recv_completions);
    return ok;
}
