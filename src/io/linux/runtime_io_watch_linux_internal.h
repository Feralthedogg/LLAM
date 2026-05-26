/**
 * @file src/io/linux/runtime_io_watch_linux_internal.h
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
#define LLAM_IO_SQPOLL_SPIN_NS 50000ULL
/** Maximum SQPOLL spin iterations before falling back to eventfd wait. */
#define LLAM_IO_SQPOLL_SPIN_ITERS 256U

/** @brief Temporary accepted-fd completion list node used outside watch locks. */
typedef struct llam_accept_watch_completion {
    llam_io_req_t *req;
    int result;
    struct llam_accept_watch_completion *next;
} llam_accept_watch_completion_t;

/** @brief Temporary poll completion list node containing detached waiters. */
typedef struct llam_poll_watch_completion {
    llam_io_req_t *waiters;
    int revents;
    struct llam_poll_watch_completion *next;
} llam_poll_watch_completion_t;

/** @brief Temporary receive completion list node with optional provided-buffer id. */
typedef struct llam_recv_watch_completion {
    llam_io_req_t *req;
    size_t size;
    unsigned short bid;
    unsigned node_index;
    bool has_buffer;
    struct llam_recv_watch_completion *next;
} llam_recv_watch_completion_t;

/* Backend setup, wake, and submission primitives. */
void llam_io_uring_sqe_set_buf_group_compat(struct io_uring_sqe *sqe, int buf_group);
uint64_t llam_hash_watch_identity_u64(uint64_t value);
void llam_node_lower_worker_priority(const llam_node_t *node);
bool llam_node_submit_needs_syscall(llam_node_t *node);
int llam_node_submit_ring(llam_node_t *node);
bool llam_node_wait_eventfd(llam_node_t *node, int timeout_ms);
bool llam_node_spin_for_cqe(llam_node_t *node);
void llam_shard_note_inflight_io_waiter(llam_runtime_t *rt, unsigned owner_shard, int delta);

/* Shared watch-list mutation helpers. */
llam_io_req_t *llam_take_node_submissions(llam_node_t *node);
llam_io_req_t *llam_poll_watch_take_waiters(llam_poll_watch_t *watch);
void llam_destroy_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch);
llam_io_req_t *llam_accept_watch_pop_waiter(llam_accept_watch_t *watch);
llam_io_req_t *llam_recv_watch_pop_waiter(llam_recv_watch_t *watch);
bool llam_accept_watch_push_ready_owned(llam_accept_watch_t *watch, int fd);
void llam_accept_watch_push_ready(llam_accept_watch_t *watch, int fd);
void llam_destroy_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch);
void llam_release_recv_ready(llam_runtime_t *rt, llam_node_t *fallback_node, llam_recv_ready_t *ready);
bool llam_recv_watch_push_ready(llam_recv_watch_t *watch,
                              size_t size,
                              unsigned short bid,
                              bool has_buffer,
                              unsigned node_index,
                              unsigned char *copy_data,
                              size_t copy_capacity);

bool llam_capture_recv_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino);
llam_poll_watch_t *llam_find_poll_watch_locked(llam_node_t *node, int fd, short events);
llam_accept_watch_t *llam_find_accept_watch_locked(llam_node_t *node, int fd);
llam_recv_watch_t *llam_find_recv_watch_locked(llam_node_t *node, int fd, dev_t st_dev, ino_t st_ino);
void llam_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch);
bool llam_accept_watch_completion_push(llam_accept_watch_completion_t **head,
                                     llam_accept_watch_completion_t **tail,
                                     llam_io_req_t *req,
                                     int result);
void llam_accept_watch_completion_drain(llam_node_t *node, llam_accept_watch_completion_t *head);
bool llam_poll_watch_completion_push(llam_poll_watch_completion_t **head,
                                   llam_poll_watch_completion_t **tail,
                                   llam_io_req_t *waiters,
                                   int revents);
void llam_poll_watch_completion_drain(llam_node_t *node, llam_poll_watch_completion_t *head);
bool llam_recv_watch_completion_push(llam_recv_watch_completion_t **head,
                                   llam_recv_watch_completion_t **tail,
                                   llam_io_req_t *req,
                                   size_t size,
                                   unsigned short bid,
                                   bool has_buffer,
                                   unsigned node_index);
void llam_recv_watch_completion_drain(llam_runtime_t *rt,
                                    llam_node_t *fallback_node,
                                    llam_recv_watch_completion_t *head);
bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target);

/* Backend arming and live watch forwarding/migration. */
bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node);
bool llam_arm_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node);
bool llam_arm_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node);
bool llam_forward_live_poll_watch_event(llam_node_t *source,
                                      int fd,
                                      short events,
                                      unsigned target_index,
                                      short revents);
bool llam_forward_live_accept_watch_ready(llam_node_t *source, int fd, unsigned target_index, int accepted_fd);
bool llam_forward_live_recv_watch_ready(llam_node_t *source,
                                      int fd,
                                      dev_t st_dev,
                                      ino_t st_ino,
                                      unsigned target_index,
                                      size_t size,
                                      unsigned short bid,
                                      bool has_buffer,
                                      unsigned ready_node_index);
bool llam_finalize_poll_watch_migration(llam_node_t *source,
                                      llam_poll_watch_t *watch,
                                      unsigned target_index,
                                      bool *kick_target);
bool llam_finalize_accept_watch_migration(llam_node_t *source,
                                        llam_accept_watch_t *watch,
                                        unsigned target_index,
                                        bool *kick_target);
bool llam_finalize_recv_watch_migration(llam_node_t *source,
                                      llam_recv_watch_t *watch,
                                      unsigned target_index,
                                      bool *kick_target);

unsigned llam_watch_migration_target_index(llam_runtime_t *rt,
                                         unsigned fallback_target_index,
                                         unsigned current_target_index,
                                         int fd);
bool llam_io_rehome_watch_state_filtered(llam_node_t *source, llam_node_t *target, bool only_marked);

/* Control queue, submission, and completion loop entry points. */
llam_io_control_op_t *llam_take_node_controls(llam_node_t *node);
void llam_io_complete_req(llam_node_t *node,
                        llam_io_req_t *req,
                        int res,
                        unsigned cqe_flags,
                        bool decrement_pending);
void llam_io_submit_control_op(llam_node_t *node, llam_io_control_op_t *op);
void llam_io_submit_one(llam_node_t *node, llam_io_req_t *req);
void llam_io_queue_shutdown_controls(llam_node_t *node);
void llam_io_submit_batch(llam_node_t *node);
void llam_io_handle_cqe(llam_node_t *node, struct io_uring_cqe *cqe);
void llam_io_drain_completions(llam_node_t *node);
bool llam_linux_wait_cqe_error_is_fatal(int err);

#endif
