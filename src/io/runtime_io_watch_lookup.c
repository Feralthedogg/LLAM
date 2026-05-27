/**
 * @file src/io/runtime_io_watch_lookup.c
 * @brief Shared watch lookup, creation, and common destruction helpers.
 *
 * @details
 * Linux and Darwin watch backends both protect fd reuse by recording descriptor
 * device/inode identity when a watch is created.  Keeping that lookup policy in
 * one file prevents the two native backends from drifting apart while leaving
 * backend-specific readiness release logic in the platform files.
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

/**
 * @brief Capture stable identity fields for an fd.
 *
 * @param fd     File descriptor to inspect.
 * @param st_dev Optional device id output.
 * @param st_ino Optional inode id output.
 * @return true on successful @c fstat.
 */
bool llam_capture_fd_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino) {
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return false;
    }
    if (st_dev != NULL) {
        *st_dev = st.st_dev;
    }
    if (st_ino != NULL) {
        *st_ino = st.st_ino;
    }
    return true;
}

static bool llam_capture_accept_watch_identity(int fd,
                                               dev_t *st_dev,
                                               ino_t *st_ino,
                                               struct sockaddr_storage *local_addr,
                                               socklen_t *local_addrlen,
                                               bool *has_local_addr) {
    socklen_t len;

    if (!llam_capture_fd_watch_identity(fd, st_dev, st_ino)) {
        return false;
    }
    if (local_addr == NULL || local_addrlen == NULL || has_local_addr == NULL) {
        return true;
    }

    memset(local_addr, 0, sizeof(*local_addr));
    len = (socklen_t)sizeof(*local_addr);
    if (getsockname(fd, (struct sockaddr *)(void *)local_addr, &len) == 0 &&
        len <= (socklen_t)sizeof(*local_addr)) {
        *local_addrlen = len;
        *has_local_addr = true;
    } else {
        *local_addrlen = 0U;
        *has_local_addr = false;
    }
    return true;
}

static bool llam_accept_watch_identity_matches(const llam_accept_watch_t *watch,
                                               int fd,
                                               dev_t st_dev,
                                               ino_t st_ino,
                                               const struct sockaddr_storage *local_addr,
                                               socklen_t local_addrlen,
                                               bool has_local_addr) {
    if (watch == NULL ||
        watch->fd != fd ||
        watch->st_dev != st_dev ||
        watch->st_ino != st_ino ||
        watch->has_local_addr != has_local_addr) {
        return false;
    }
    if (!has_local_addr) {
        return true;
    }
    return watch->local_addrlen == local_addrlen &&
           memcmp(&watch->local_addr, local_addr, (size_t)local_addrlen) == 0;
}

/**
 * @brief Find an existing poll watch while watch_lock is held.
 *
 * @param node   Node whose poll watch table is searched.
 * @param fd     File descriptor.
 * @param events Requested poll event mask.
 * @return Matching watch, or NULL.
 */
llam_poll_watch_t *llam_find_poll_watch_locked(llam_node_t *node, int fd, short events) {
    llam_poll_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (node == NULL || !llam_capture_fd_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }

    watch = node->poll_watches;
    while (watch != NULL) {
        if (watch->fd == fd && watch->events == events && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/**
 * @brief Find an existing accept watch while watch_lock is held.
 *
 * @param node Node whose accept watch table is searched.
 * @param fd   Listener descriptor.
 * @return Matching watch, or NULL.
 */
llam_accept_watch_t *llam_find_accept_watch_locked(llam_node_t *node, int fd) {
    llam_accept_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = 0U;
    bool has_local_addr = false;

    if (node == NULL ||
        !llam_capture_accept_watch_identity(fd,
                                            &st_dev,
                                            &st_ino,
                                            &local_addr,
                                            &local_addrlen,
                                            &has_local_addr)) {
        return NULL;
    }

    watch = node->accept_watches;
    while (watch != NULL) {
        if (llam_accept_watch_identity_matches(watch,
                                               fd,
                                               st_dev,
                                               st_ino,
                                               &local_addr,
                                               local_addrlen,
                                               has_local_addr)) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/**
 * @brief Find an existing receive watch while watch_lock is held.
 *
 * @param node   Node whose receive watch table is searched.
 * @param fd     File descriptor.
 * @param st_dev Captured device id.
 * @param st_ino Captured inode id.
 * @return Matching watch, or NULL.
 */
llam_recv_watch_t *llam_find_recv_watch_locked(llam_node_t *node, int fd, dev_t st_dev, ino_t st_ino) {
    llam_recv_watch_t *watch;

    if (node == NULL) {
        return NULL;
    }

    watch = node->recv_watches;
    while (watch != NULL) {
        if (watch->fd == fd && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/**
 * @brief Find or create a poll watch while watch_lock is held.
 *
 * @param node   Node that will own the watch.
 * @param fd     File descriptor.
 * @param events Poll event mask.
 * @return Watch on success, NULL on fstat/allocation failure.
 */
llam_poll_watch_t *llam_get_or_create_poll_watch_locked(llam_node_t *node, int fd, short events) {
    llam_poll_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (node == NULL || !llam_capture_fd_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }

    watch = node->poll_watches;
    while (watch != NULL) {
        if (watch->fd == fd && watch->events == events && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }

    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->st_dev = st_dev;
    watch->st_ino = st_ino;
    watch->events = events;
    watch->migrate_target_node_index = UINT_MAX;
    /*
     * A migrating source watch sets this once the target owns all user-facing
     * state; final backend events still arriving at the source are then
     * forwarded instead of waking local waiters.
     */
    watch->live_transferred = false;
    watch->next = node->poll_watches;
    node->poll_watches = watch;
    return watch;
}

/**
 * @brief Find or create an accept watch while watch_lock is held.
 *
 * @param node Node that will own the watch.
 * @param fd   Listener descriptor.
 * @return Watch on success, NULL on fstat/allocation failure.
 */
llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, int fd) {
    llam_accept_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = 0U;
    bool has_local_addr = false;

    if (node == NULL ||
        !llam_capture_accept_watch_identity(fd,
                                            &st_dev,
                                            &st_ino,
                                            &local_addr,
                                            &local_addrlen,
                                            &has_local_addr)) {
        return NULL;
    }

    watch = node->accept_watches;
    while (watch != NULL) {
        if (llam_accept_watch_identity_matches(watch,
                                               fd,
                                               st_dev,
                                               st_ino,
                                               &local_addr,
                                               local_addrlen,
                                               has_local_addr)) {
            return watch;
        }
        watch = watch->next;
    }
    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->st_dev = st_dev;
    watch->st_ino = st_ino;
    if (has_local_addr) {
        memcpy(&watch->local_addr, &local_addr, sizeof(watch->local_addr));
    }
    watch->local_addrlen = local_addrlen;
    watch->has_local_addr = has_local_addr;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    watch->next = node->accept_watches;
    node->accept_watches = watch;
    return watch;
}

/**
 * @brief Find or create a receive watch while watch_lock is held.
 *
 * @param node Node that will own the watch.
 * @param fd   File descriptor.
 * @return Watch on success, NULL on fstat/allocation failure.
 */
llam_recv_watch_t *llam_get_or_create_recv_watch_locked(llam_node_t *node, int fd) {
    llam_recv_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (node == NULL || !llam_capture_fd_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }

    watch = llam_find_recv_watch_locked(node, fd, st_dev, st_ino);
    if (watch != NULL) {
        return watch;
    }

    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->st_dev = st_dev;
    watch->st_ino = st_ino;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    watch->next = node->recv_watches;
    node->recv_watches = watch;
    return watch;
}

/**
 * @brief Destroy and unlink a poll watch while watch_lock is held.
 *
 * @param node  Node owning the watch.
 * @param watch Watch to destroy.
 */
void llam_destroy_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch) {
    llam_poll_watch_t **cursor;

    if (node == NULL || watch == NULL) {
        return;
    }

    cursor = &node->poll_watches;
    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/**
 * @brief Destroy and unlink an accept watch while closing buffered accepted fds.
 *
 * @param node  Node owning the watch.
 * @param watch Watch to destroy.
 */
void llam_destroy_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch) {
    llam_accept_watch_t **cursor;

    if (node == NULL || watch == NULL) {
        return;
    }

    cursor = &node->accept_watches;
    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            while (watch->ready_head != NULL) {
                llam_accept_ready_t *next = watch->ready_head->next;

                close(watch->ready_head->fd);
                free(watch->ready_head);
                watch->ready_head = next;
            }
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/**
 * @brief Destroy a receive watch only when it has no active backend/user state.
 *
 * @param node  Node owning the watch.
 * @param watch Candidate watch.
 */
void llam_maybe_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch) {
    if (watch == NULL) {
        return;
    }
    if (watch->active || watch->activating || watch->deactivate_queued) {
        return;
    }
    if (watch->wait_head != NULL || watch->ready_head != NULL) {
        return;
    }
    llam_destroy_recv_watch_locked(node, watch);
}

#endif
