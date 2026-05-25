/**
 * @file src/io/runtime_io_watch_migration_live_forward_template.inc
 * @brief Shared live watch readiness-forwarding template.
 *
 * @details
 * Linux and Darwin forward readiness with the same migration locking and
 * waiter-restoration rules. Platform files provide completion-list types and
 * receive payload adapters before including this template.
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
#ifndef LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH
#error "LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH must be defined"
#endif
#ifndef LLAM_LIVE_RECV_FORWARD_READY_PUSH
#error "LLAM_LIVE_RECV_FORWARD_READY_PUSH must be defined"
#endif
#ifndef LLAM_LIVE_RECV_FORWARD_PARAMS
#error "LLAM_LIVE_RECV_FORWARD_PARAMS must be defined"
#endif
#ifndef LLAM_LIVE_RECV_FORWARD_ARGS
#error "LLAM_LIVE_RECV_FORWARD_ARGS must be defined"
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
 * @brief Forward poll readiness from a source watch to a target node.
 *
 * @return true if the target consumed or buffered the event.
 */
bool llam_forward_live_poll_watch_event(llam_node_t *source,
                                        int fd,
                                        short events,
                                        unsigned target_index,
                                        short revents) {
    LLAM_LIVE_POLL_COMPLETION_T *poll_completions = NULL;
    LLAM_LIVE_POLL_COMPLETION_T *poll_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
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
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
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

        if (!LLAM_LIVE_POLL_COMPLETION_PUSH(&poll_completions,
                                            &poll_completion_tail,
                                            waiters,
                                            merged_revents)) {
            llam_poll_watch_push_waiters_front(target_watch, waiters);
            goto out;
        }
        target_watch->sticky_revents = 0;
        ok = true;
        goto out;
    }

    target_watch->sticky_revents |= revents;
    ok = true;

out:
    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_POLL_COMPLETION_DRAIN(target, poll_completions);
    return ok;
}

/**
 * @brief Forward an accepted fd from a source watch to a target node.
 *
 * @return true if ownership of @p accepted_fd transferred.
 */
bool llam_forward_live_accept_watch_ready(llam_node_t *source, int fd, unsigned target_index, int accepted_fd) {
    LLAM_LIVE_ACCEPT_COMPLETION_T *accept_completions = NULL;
    LLAM_LIVE_ACCEPT_COMPLETION_T *accept_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
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
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
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
            !LLAM_LIVE_ACCEPT_COMPLETION_PUSH(&accept_completions,
                                              &accept_completion_tail,
                                              waiter,
                                              accepted_fd)) {
            llam_accept_watch_push_waiter_front(target_watch, waiter);
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
    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_ACCEPT_COMPLETION_DRAIN(target, accept_completions);
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
                                        LLAM_LIVE_RECV_FORWARD_PARAMS) {
    LLAM_LIVE_RECV_COMPLETION_T *recv_completions = NULL;
    LLAM_LIVE_RECV_COMPLETION_T *recv_completion_tail = NULL;
    llam_runtime_t *rt;
    llam_node_t *target;
    llam_node_t *source_locked = NULL;
    llam_node_t *target_locked = NULL;
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
    llam_io_lock_rehome_pair(source, target, &source_locked, &target_locked);
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

        if (waiter == NULL ||
            !LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH(&recv_completions,
                                                    &recv_completion_tail,
                                                    waiter,
                                                    LLAM_LIVE_RECV_FORWARD_ARGS)) {
            llam_recv_watch_push_waiter_front(target_watch, waiter);
            goto out;
        }
        ok = true;
        goto out;
    }

    if (!LLAM_LIVE_RECV_FORWARD_READY_CALL(target_watch)) {
        goto out;
    }
    ok = true;

out:
    llam_io_unlock_rehome_pair(source, target);
    LLAM_LIVE_RECV_COMPLETION_DRAIN(rt, target, recv_completions);
    return ok;
}
