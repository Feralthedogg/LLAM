/**
 * @file src/io/darwin/runtime_io_watch_darwin_migration_live.c
 * @brief Darwin live I/O watch migration checks and inflight ownership handling.
 *
 * @details
 * Darwin live migration mirrors the Linux watch migration model, but receive
 * readiness is copied payload data instead of provided-buffer ownership. Source
 * and target watch locks are always acquired by node index; completions are
 * queued and drained after both locks are released.
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

/** @brief Ensure a poll watch has an activation control queued if needed. */
bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    if (!watch->active && !watch->activating && watch->deactivate_queued) {
        // If deactivate has not reached the worker yet, remove it and reuse the
        // existing watch object.
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

/** @brief Pick the owner node for multishot watch state. */
unsigned llam_multishot_owner_node_index(llam_runtime_t *rt, unsigned fallback_node_index, int fd) {
    struct stat st;
    uint64_t hash;

    if (rt == NULL) {
        return 0U;
    }
    if (fallback_node_index >= rt->active_nodes) {
        fallback_node_index = rt->active_nodes > 0U ? 0U : fallback_node_index;
    }
    if (rt->experimental_shard_rings == 0U ||
        rt->experimental_shard_rings_multishot == 0U ||
        rt->active_nodes <= 1U ||
        fd < 0) {
        return fallback_node_index;
    }
    if (fstat(fd, &st) != 0) {
        return fallback_node_index;
    }

    hash = llam_hash_watch_identity_u64((uint64_t)st.st_dev);
    hash ^= llam_hash_watch_identity_u64((uint64_t)st.st_ino);
    hash ^= llam_hash_watch_identity_u64((uint64_t)(unsigned)fd);
    return (unsigned)(hash % rt->active_nodes);
}

/**
 * @brief Forward poll readiness from a source node to a migration target.
 *
 * @return true if the target consumed or buffered the readiness.
 */
bool llam_forward_live_poll_watch_event(llam_node_t *source,
                                             int fd,
                                             short events,
                                             unsigned target_index,
                                             short revents) {
    llam_darwin_poll_completion_t *poll_completions = NULL;
    llam_darwin_poll_completion_t *poll_completion_tail = NULL;
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
    // Lock in stable node order to avoid migration deadlocks.
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

        // Complete waiters after releasing watch locks. Restore the waiter list
        // if completion-node allocation fails.
        if (!llam_darwin_poll_completion_push(&poll_completions, &poll_completion_tail, waiters, merged_revents)) {
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
    llam_darwin_poll_completion_drain(target, poll_completions);
    return ok;
}

/**
 * @brief Forward an accepted fd from source to target during migration.
 *
 * @return true if ownership of @p accepted_fd transferred.
 */
bool llam_forward_live_accept_watch_ready(llam_node_t *source, int fd, unsigned target_index, int accepted_fd) {
    llam_darwin_accept_completion_t *accept_completions = NULL;
    llam_darwin_accept_completion_t *accept_completion_tail = NULL;
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

        if (waiter == NULL ||
            !llam_darwin_accept_completion_push(&accept_completions, &accept_completion_tail, waiter, accepted_fd)) {
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
    llam_darwin_complete_accept_completions(target, accept_completions);
    return ok;
}

/**
 * @brief Forward copied receive readiness from source to target during migration.
 *
 * @return true if target consumed or buffered the payload.
 */
bool llam_forward_live_recv_watch_ready(llam_node_t *source,
                                             int fd,
                                             dev_t st_dev,
                                             ino_t st_ino,
                                             unsigned target_index,
                                             const unsigned char *data,
                                             size_t size) {
    llam_darwin_recv_completion_t *recv_completions = NULL;
    llam_darwin_recv_completion_t *recv_completion_tail = NULL;
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

        // Darwin forwards a copied packet; ownership is transferred through the
        // deferred completion list.
        if (waiter == NULL ||
            !llam_darwin_recv_completion_push_copy(&recv_completions,
                                                 &recv_completion_tail,
                                                 waiter,
                                                 data,
                                                 size)) {
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

    if (!llam_recv_watch_push_ready_copy(target_watch, data, size)) {
        goto out;
    }
    ok = true;

out:
    pthread_mutex_unlock(&second->watch_lock);
    pthread_mutex_unlock(&first->watch_lock);
    llam_darwin_recv_completion_drain(target, recv_completions);
    return ok;
}

/** @brief Finish poll watch migration after source deactivation. */
bool llam_finalize_poll_watch_migration(llam_node_t *source,
                                             llam_poll_watch_t *watch,
                                             unsigned target_index,
                                             bool *kick_target) {
    llam_darwin_poll_completion_t *poll_completions = NULL;
    llam_darwin_poll_completion_t *poll_completion_tail = NULL;
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

                    if (!llam_darwin_poll_completion_push(&poll_completions, &poll_completion_tail, waiters, revents)) {
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
                // No sticky event remains; arm target so future readiness lands
                // on the new node.
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
    llam_darwin_poll_completion_drain(target, poll_completions);
    return ok;
}

/** @brief Finish accept watch migration after source deactivation. */
bool llam_finalize_accept_watch_migration(llam_node_t *source,
                                               llam_accept_watch_t *watch,
                                               unsigned target_index,
                                               bool *kick_target) {
    llam_darwin_accept_completion_t *accept_completions = NULL;
    llam_darwin_accept_completion_t *accept_completion_tail = NULL;
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
                // First deliver buffered accepted fds to target waiters, then
                // splice any leftovers into the target ready queue.
                while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                    llam_accept_ready_t *ready = watch->ready_head;
                    llam_io_req_t *waiter = llam_accept_watch_pop_waiter(target_watch);

                    if (waiter == NULL ||
                        !llam_darwin_accept_completion_push(&accept_completions,
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
    llam_darwin_complete_accept_completions(target, accept_completions);
    return ok;
}

/** @brief Finish receive watch migration after source deactivation. */
bool llam_finalize_recv_watch_migration(llam_node_t *source,
                                             llam_recv_watch_t *watch,
                                             unsigned target_index,
                                             bool *kick_target) {
    llam_darwin_recv_completion_t *recv_completions = NULL;
    llam_darwin_recv_completion_t *recv_completion_tail = NULL;
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
                // Move buffered copied payloads without another copy by
                // transferring copy_data ownership to deferred completions or
                // the target ready queue.
                while (watch->ready_head != NULL && target_watch->wait_head != NULL) {
                    llam_recv_ready_t *ready = watch->ready_head;
                    llam_io_req_t *waiter = llam_recv_watch_pop_waiter(target_watch);

                    if (waiter == NULL ||
                        !llam_darwin_recv_completion_push(&recv_completions,
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
    llam_darwin_recv_completion_drain(target, recv_completions);
    return ok;
}
