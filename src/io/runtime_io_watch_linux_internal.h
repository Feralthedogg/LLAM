/**
 * @file src/io/runtime_io_watch_linux_internal.h
 * @brief Linux/io_uring I/O watch backend private declarations.
 *
 * @details
 * The Linux watch backend is split across submit, completion, lookup, control,
 * worker, state, and migration files. This header keeps their private contracts
 * in one place: watch waiter queues, temporary completion lists, live watch
 * forwarding, rehome finalization, and io_uring submission/completion helpers.
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

#ifndef LLAM_RUNTIME_IO_WATCH_LINUX_INTERNAL_H
#define LLAM_RUNTIME_IO_WATCH_LINUX_INTERNAL_H

#include "runtime_internal.h"

/** Busy-spin budget for SQPOLL completion polling before sleeping. */
#define NM_IO_SQPOLL_SPIN_NS 50000ULL
/** Maximum SQPOLL spin iterations before falling back to eventfd wait. */
#define NM_IO_SQPOLL_SPIN_ITERS 256U

/** @brief Temporary accepted-fd completion list node used outside watch locks. */
typedef struct nm_accept_watch_completion {
    nm_io_req_t *req;
    int result;
    struct nm_accept_watch_completion *next;
} nm_accept_watch_completion_t;

/** @brief Temporary poll completion list node containing detached waiters. */
typedef struct nm_poll_watch_completion {
    nm_io_req_t *waiters;
    int revents;
    struct nm_poll_watch_completion *next;
} nm_poll_watch_completion_t;

/** @brief Temporary receive completion list node with optional provided-buffer id. */
typedef struct nm_recv_watch_completion {
    nm_io_req_t *req;
    size_t size;
    unsigned short bid;
    unsigned node_index;
    bool has_buffer;
    struct nm_recv_watch_completion *next;
} nm_recv_watch_completion_t;

/* Backend setup, wake, and submission primitives. */
void nm_io_uring_sqe_set_buf_group_compat(struct io_uring_sqe *sqe, int buf_group);
uint64_t nm_hash_watch_identity_u64(uint64_t value);
void nm_node_lower_worker_priority(const nm_node_t *node);
bool nm_node_submit_needs_syscall(nm_node_t *node);
int nm_node_submit_ring(nm_node_t *node);
bool nm_node_wait_eventfd(nm_node_t *node, int timeout_ms);
bool nm_node_spin_for_cqe(nm_node_t *node);
void nm_shard_note_inflight_io_waiter(unsigned owner_shard, int delta);

/* Shared watch-list mutation helpers. */
nm_io_req_t *nm_take_node_submissions(nm_node_t *node);
nm_io_req_t *nm_poll_watch_take_waiters(nm_poll_watch_t *watch);
void nm_destroy_poll_watch_locked(nm_node_t *node, nm_poll_watch_t *watch);
nm_io_req_t *nm_accept_watch_pop_waiter(nm_accept_watch_t *watch);
nm_io_req_t *nm_recv_watch_pop_waiter(nm_recv_watch_t *watch);
bool nm_accept_watch_push_ready_owned(nm_accept_watch_t *watch, int fd);
void nm_accept_watch_push_ready(nm_accept_watch_t *watch, int fd);
void nm_destroy_accept_watch_locked(nm_node_t *node, nm_accept_watch_t *watch);
void nm_release_recv_ready(nm_runtime_t *rt, nm_node_t *fallback_node, nm_recv_ready_t *ready);
bool nm_recv_watch_push_ready(nm_recv_watch_t *watch,
                              size_t size,
                              unsigned short bid,
                              bool has_buffer,
                              unsigned node_index,
                              unsigned char *copy_data,
                              size_t copy_capacity);

bool nm_capture_recv_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino);
nm_poll_watch_t *nm_find_poll_watch_locked(nm_node_t *node, int fd, short events);
nm_accept_watch_t *nm_find_accept_watch_locked(nm_node_t *node, int fd);
nm_recv_watch_t *nm_find_recv_watch_locked(nm_node_t *node, int fd, dev_t st_dev, ino_t st_ino);
void nm_destroy_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch);
bool nm_accept_watch_completion_push(nm_accept_watch_completion_t **head,
                                     nm_accept_watch_completion_t **tail,
                                     nm_io_req_t *req,
                                     int result);
void nm_accept_watch_completion_drain(nm_node_t *node, nm_accept_watch_completion_t *head);
bool nm_poll_watch_completion_push(nm_poll_watch_completion_t **head,
                                   nm_poll_watch_completion_t **tail,
                                   nm_io_req_t *waiters,
                                   int revents);
void nm_poll_watch_completion_drain(nm_node_t *node, nm_poll_watch_completion_t *head);
bool nm_recv_watch_completion_push(nm_recv_watch_completion_t **head,
                                   nm_recv_watch_completion_t **tail,
                                   nm_io_req_t *req,
                                   size_t size,
                                   unsigned short bid,
                                   bool has_buffer,
                                   unsigned node_index);
void nm_recv_watch_completion_drain(nm_runtime_t *rt,
                                    nm_node_t *fallback_node,
                                    nm_recv_watch_completion_t *head);
bool nm_drop_node_control_locked(nm_node_t *node, nm_io_control_kind_t kind, const void *target);

/* Backend arming and live watch forwarding/migration. */
bool nm_arm_poll_watch_locked(nm_node_t *node, nm_poll_watch_t *watch, bool *kick_node);
bool nm_arm_accept_watch_locked(nm_node_t *node, nm_accept_watch_t *watch, bool *kick_node);
bool nm_arm_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch, bool *kick_node);
bool nm_forward_live_poll_watch_event(nm_node_t *source,
                                      int fd,
                                      short events,
                                      unsigned target_index,
                                      short revents);
bool nm_forward_live_accept_watch_ready(nm_node_t *source, int fd, unsigned target_index, int accepted_fd);
bool nm_forward_live_recv_watch_ready(nm_node_t *source,
                                      int fd,
                                      dev_t st_dev,
                                      ino_t st_ino,
                                      unsigned target_index,
                                      size_t size,
                                      unsigned short bid,
                                      bool has_buffer,
                                      unsigned ready_node_index);
bool nm_finalize_poll_watch_migration(nm_node_t *source,
                                      nm_poll_watch_t *watch,
                                      unsigned target_index,
                                      bool *kick_target);
bool nm_finalize_accept_watch_migration(nm_node_t *source,
                                        nm_accept_watch_t *watch,
                                        unsigned target_index,
                                        bool *kick_target);
bool nm_finalize_recv_watch_migration(nm_node_t *source,
                                      nm_recv_watch_t *watch,
                                      unsigned target_index,
                                      bool *kick_target);

unsigned nm_watch_migration_target_index(nm_runtime_t *rt,
                                         unsigned fallback_target_index,
                                         unsigned current_target_index,
                                         int fd);
bool nm_io_rehome_watch_state_filtered(nm_node_t *source, nm_node_t *target, bool only_marked);

/* Control queue, submission, and completion loop entry points. */
nm_io_control_op_t *nm_take_node_controls(nm_node_t *node);
void nm_io_complete_req(nm_node_t *node,
                        nm_io_req_t *req,
                        int res,
                        unsigned cqe_flags,
                        bool decrement_pending);
void nm_io_submit_control_op(nm_node_t *node, nm_io_control_op_t *op);
void nm_io_submit_one(nm_node_t *node, nm_io_req_t *req);
void nm_io_queue_shutdown_controls(nm_node_t *node);
void nm_io_submit_batch(nm_node_t *node);
void nm_io_handle_cqe(nm_node_t *node, struct io_uring_cqe *cqe);
void nm_io_drain_completions(nm_node_t *node);

#endif
