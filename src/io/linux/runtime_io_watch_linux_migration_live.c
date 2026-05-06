/**
 * @file src/io/linux/runtime_io_watch_linux_migration_live.c
 * @brief Linux live I/O watch migration checks and inflight ownership handling.
 *
 * @details
 * Live migration handles events that arrive on a source node while watch state is
 * moving to a target node. To avoid deadlocks, source and target watch locks are
 * always acquired by ascending node index, and task completions are deferred
 * until after both locks are released.
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

/** @brief Ensure a poll watch has an activation control queued if needed. */
bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    if (!watch->active && !watch->activating && watch->deactivate_queued) {
        // Drop a not-yet-submitted deactivate when new demand arrives before the
        // worker consumes it.
        if (!llam_drop_node_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
            return false;
        }
        watch->deactivate_queued = false;
    }
    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_ACTIVATE, watch) != 0) {
            return false;
        }
        watch->activating = true;
        if (kick_node != NULL) {
            *kick_node = true;
        }
    }
    return true;
}

/** @brief Ensure an accept watch has an activation control queued if needed. */
bool llam_arm_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    if (!watch->active && !watch->activating && watch->deactivate_queued) {
        if (!llam_drop_node_control_locked(node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch)) {
            return false;
        }
        watch->deactivate_queued = false;
    }
    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_ACCEPT_ACTIVATE, watch) != 0) {
            return false;
        }
        watch->activating = true;
        if (kick_node != NULL) {
            *kick_node = true;
        }
    }
    return true;
}

/** @brief Ensure a receive watch has an activation control queued if needed. */
bool llam_arm_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    if (!watch->active && !watch->activating && watch->deactivate_queued) {
        if (!llam_drop_node_control_locked(node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch)) {
            return false;
        }
        watch->deactivate_queued = false;
    }
    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_RECV_ACTIVATE, watch) != 0) {
            return false;
        }
        watch->activating = true;
        if (kick_node != NULL) {
            *kick_node = true;
        }
    }
    return true;
}

/**
 * @brief Forward poll readiness from a source watch to a target node.
 *
 * @return true if the target consumed or buffered the event.
 */
bool llam_forward_live_poll_watch_event(llam_node_t *source,
                                             int fd,
                                             short events,
                                             unsigned target_index,
                                             short revents) {
    llam_poll_watch_completion_t *poll_completions = NULL;
    llam_poll_watch_completion_t *poll_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
    llam_poll_watch_t *source_watch;
    llam_poll_watch_t *target_watch;
    bool ok = false;

    if (source == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    // Lock order by node index is required because migrations can be initiated
    // from either side.
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

    source_watch = llam_find_poll_watch_locked(source_locked, fd, events);
    if (source_watch != NULL && source_watch->migrate_target_node_index != target_index) {
        goto out;
    }
    target_watch = llam_get_or_create_poll_watch_locked(target_locked, fd, events);
    if (target_watch == NULL) {
        goto out;
    }
    if (target_watch->wait_head != NULL) {
        llam_io_req_t *waiters = llam_poll_watch_take_waiters(target_watch);
        int merged_revents = revents | target_watch->sticky_revents;

        // Complete outside the watch locks. If allocation fails, restore the
        // waiter list so no parked task is lost.
        if (!llam_poll_watch_completion_push(&poll_completions, &poll_completion_tail, waiters, merged_revents)) {
            if (waiters != NULL) {
                target_watch->wait_head = waiters;
                target_watch->wait_tail = waiters;
                while (target_watch->wait_tail->next != NULL) {
                    target_watch->wait_tail = target_watch->wait_tail->next;
                }
            }
            goto out;
        }
        target_watch->sticky_revents = 0;
        ok = true;
        goto out;
    }

    target_watch->sticky_revents |= revents;
    ok = true;

out:
    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_poll_watch_completion_drain(target, poll_completions);
    return ok;
}

/**
 * @brief Forward an accepted fd from a source watch to a target node.
 *
 * @return true if ownership of @p accepted_fd transferred.
 */
bool llam_forward_live_accept_watch_ready(llam_node_t *source, int fd, unsigned target_index, int accepted_fd) {
    llam_accept_watch_completion_t *accept_completions = NULL;
    llam_accept_watch_completion_t *accept_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
    llam_accept_watch_t *source_watch;
    llam_accept_watch_t *target_watch;
    bool ok = false;

    if (source == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

    source_watch = llam_find_accept_watch_locked(source_locked, fd);
    if (source_watch != NULL && source_watch->migrate_target_node_index != target_index) {
        goto out;
    }
    target_watch = llam_get_or_create_accept_watch_locked(target_locked, fd);
    if (target_watch == NULL) {
        goto out;
    }
    if (target_watch->wait_head != NULL) {
        llam_io_req_t *waiter = llam_accept_watch_pop_waiter(target_watch);

        // Completion list ownership transfers accepted_fd to the waiter after
        // locks are released.
        if (waiter == NULL ||
            !llam_accept_watch_completion_push(&accept_completions, &accept_completion_tail, waiter, accepted_fd)) {
            if (waiter != NULL) {
                waiter->next = target_watch->wait_head;
                target_watch->wait_head = waiter;
                if (target_watch->wait_tail == NULL) {
                    target_watch->wait_tail = waiter;
                }
            }
            goto out;
        }
        ok = true;
        goto out;
    }

    if (!llam_accept_watch_push_ready_owned(target_watch, accepted_fd)) {
        goto out;
    }
    ok = true;

out:
    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_accept_watch_completion_drain(target, accept_completions);
    return ok;
}

/**
 * @brief Forward receive readiness from a source watch to a target node.
 *
 * @return true if the target consumed or buffered the readiness entry.
 */
bool llam_forward_live_recv_watch_ready(llam_node_t *source,
                                             int fd,
                                             dev_t st_dev,
                                             ino_t st_ino,
                                             unsigned target_index,
                                             size_t size,
                                             unsigned short bid,
                                             bool has_buffer,
                                             unsigned ready_node_index) {
    llam_recv_watch_completion_t *recv_completions = NULL;
    llam_recv_watch_completion_t *recv_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
    llam_recv_watch_t *source_watch;
    llam_recv_watch_t *target_watch;
    bool ok = false;

    if (source == NULL || source->runtime == NULL) {
        return false;
    }
    rt = source->runtime;
    if (target_index >= rt->active_nodes || target_index == source->index) {
        return false;
    }

    target = &rt->nodes[target_index];
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

    source_watch = llam_find_recv_watch_locked(source_locked, fd, st_dev, st_ino);
    if (source_watch != NULL && source_watch->migrate_target_node_index != target_index) {
        goto out;
    }
    target_watch = llam_get_or_create_recv_watch_locked(target_locked, fd);
    if (target_watch == NULL) {
        goto out;
    }
    if (target_watch->wait_head != NULL) {
        llam_io_req_t *waiter = llam_recv_watch_pop_waiter(target_watch);

        // Preserve provided-buffer ownership through the deferred completion so
        // the normal completion path attaches/recycles it correctly.
        if (waiter == NULL ||
            !llam_recv_watch_completion_push(&recv_completions,
                                           &recv_completion_tail,
                                           waiter,
                                           size,
                                           bid,
                                           has_buffer,
                                           ready_node_index)) {
            if (waiter != NULL) {
                waiter->next = target_watch->wait_head;
                target_watch->wait_head = waiter;
                if (target_watch->wait_tail == NULL) {
                    target_watch->wait_tail = waiter;
                }
            }
            goto out;
        }
        ok = true;
        goto out;
    }

    if (!llam_recv_watch_push_ready(target_watch, size, bid, has_buffer, ready_node_index, NULL, 0U)) {
        goto out;
    }
    ok = true;

out:
    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_recv_watch_completion_drain(rt, target, recv_completions);
    return ok;
}

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
    llam_poll_watch_completion_t *poll_completions = NULL;
    llam_poll_watch_completion_t *poll_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
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
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

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

                    if (!llam_poll_watch_completion_push(&poll_completions, &poll_completion_tail, waiters, revents)) {
                        if (waiters != NULL) {
                            target_watch->wait_head = waiters;
                            target_watch->wait_tail = waiters;
                            while (target_watch->wait_tail->next != NULL) {
                                target_watch->wait_tail = target_watch->wait_tail->next;
                            }
                        }
                    } else {
                        target_watch->sticky_revents = 0;
                        ok = true;
                    }
                } else {
                    target_watch->sticky_revents |= watch->sticky_revents;
                    ok = true;
                }
            } else {
                // No sticky event to transfer; arm the target watch so future
                // readiness arrives on the target node.
                ok = llam_arm_poll_watch_locked(target_locked, target_watch, kick_target);
            }
        }
        if (ok) {
            watch->migrate_target_node_index = UINT_MAX;
            llam_destroy_poll_watch_locked(source_locked, watch);
        }
    }

    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_poll_watch_completion_drain(target, poll_completions);
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
    llam_accept_watch_completion_t *accept_completions = NULL;
    llam_accept_watch_completion_t *accept_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
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
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

    if (watch->wait_head == NULL &&
        !watch->active &&
        !watch->activating &&
        !watch->deactivate_queued) {
        llam_accept_watch_t *target_watch = llam_get_or_create_accept_watch_locked(target_locked, watch->fd);

        if (target_watch != NULL) {
            if (watch->ready_head != NULL) {
                // First satisfy target waiters from source ready queue, then
                // splice remaining accepted fds into target's ready queue.
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
                        break;
                    }
                    watch->ready_head = ready->next;
                    if (watch->ready_head == NULL) {
                        watch->ready_tail = NULL;
                    }
                    watch->ready_depth -= 1U;
                    free(ready);
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

    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_accept_watch_completion_drain(target, accept_completions);
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
    llam_recv_watch_completion_t *recv_completions = NULL;
    llam_recv_watch_completion_t *recv_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *first;
    llam_node_t *second;
    llam_node_t *source_locked;
    llam_node_t *target_locked;
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
    first = source->index < target->index ? source : target;
    second = first == source ? target : source;
    pthread_mutex_lock(&first->watch_lock);
    pthread_mutex_lock(&second->watch_lock);
    source_locked = first == source ? first : second;
    target_locked = source_locked == source ? target : source;

    if (watch->wait_head == NULL &&
        !watch->active &&
        !watch->activating &&
        !watch->deactivate_queued) {
        llam_recv_watch_t *target_watch = llam_get_or_create_recv_watch_locked(target_locked, watch->fd);

        if (target_watch != NULL) {
            if (watch->ready_head != NULL) {
                // Move ready entries without copying. Provided-buffer ownership
                // remains encoded in each ready entry's node_index/bid pair.
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
                        break;
                    }
                    watch->ready_head = ready->next;
                    if (watch->ready_head == NULL) {
                        watch->ready_tail = NULL;
                    }
                    watch->ready_depth -= 1U;
                    free(ready);
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

    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_recv_watch_completion_drain(rt, target, recv_completions);
    return ok;
}
