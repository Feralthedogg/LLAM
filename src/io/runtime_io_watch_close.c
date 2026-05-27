/**
 * @file src/io/runtime_io_watch_close.c
 * @brief Descriptor-close cleanup for idle multishot watch state.
 *
 * @details
 * Public close is the descriptor lifetime boundary.  This file keeps that
 * policy separate from watch lookup/creation so fd-reuse hardening remains
 * easy to audit.
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

#if !LLAM_PLATFORM_WINDOWS

static bool llam_poll_watch_close_purgeable(const llam_poll_watch_t *watch, llam_fd_t fd) {
    return watch != NULL &&
           watch->fd == fd &&
           !watch->active &&
           !watch->activating &&
           !watch->deactivate_queued &&
           watch->wait_head == NULL;
}

static bool llam_accept_watch_close_purgeable(const llam_accept_watch_t *watch, llam_fd_t fd) {
    return watch != NULL &&
           watch->fd == fd &&
           !watch->active &&
           !watch->activating &&
           !watch->deactivate_queued &&
           watch->wait_head == NULL;
}

static bool llam_recv_watch_close_purgeable(const llam_recv_watch_t *watch, llam_fd_t fd) {
    return watch != NULL &&
           watch->fd == fd &&
           !watch->active &&
           !watch->activating &&
           !watch->deactivate_queued &&
           watch->wait_head == NULL;
}

static void llam_close_accept_ready_list(llam_accept_watch_t *watch) {
    while (watch->ready_head != NULL) {
        llam_accept_ready_t *next = watch->ready_head->next;

        close(watch->ready_head->fd);
        free(watch->ready_head);
        watch->ready_head = next;
    }
}

static void llam_purge_closed_fd_watches_locked(llam_node_t *node, llam_fd_t fd) {
    {
        llam_poll_watch_t **cursor = &node->poll_watches;

        while (*cursor != NULL) {
            llam_poll_watch_t *watch = *cursor;

            if (llam_poll_watch_close_purgeable(watch, fd)) {
                *cursor = watch->next;
                free(watch);
                continue;
            }
            cursor = &watch->next;
        }
    }
    {
        llam_accept_watch_t **cursor = &node->accept_watches;

        while (*cursor != NULL) {
            llam_accept_watch_t *watch = *cursor;

            if (llam_accept_watch_close_purgeable(watch, fd)) {
                *cursor = watch->next;
                llam_close_accept_ready_list(watch);
                free(watch);
                continue;
            }
            cursor = &watch->next;
        }
    }
    {
        llam_recv_watch_t *watch = node->recv_watches;

        while (watch != NULL) {
            llam_recv_watch_t *next = watch->next;

            if (llam_recv_watch_close_purgeable(watch, fd)) {
                llam_destroy_recv_watch_locked(node, watch);
            }
            watch = next;
        }
    }
}

void llam_forget_closed_fd_watch_state(llam_runtime_t *rt, llam_fd_t fd) {
    if (rt == NULL || rt->nodes == NULL || LLAM_FD_IS_INVALID(fd)) {
        return;
    }

    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        llam_node_t *node = &rt->nodes[i];

        if (!node->watch_lock_initialized) {
            continue;
        }
        pthread_mutex_lock(&node->watch_lock);
        /*
         * Only idle watches are freed here. Active backend watches still have
         * kernel completions carrying the watch pointer, so their existing
         * deactivate/completion path owns final teardown.
         */
        llam_purge_closed_fd_watches_locked(node, fd);
        pthread_mutex_unlock(&node->watch_lock);
    }
}

#endif
