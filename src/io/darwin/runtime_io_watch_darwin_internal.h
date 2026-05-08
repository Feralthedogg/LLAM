/**
 * @file src/io/darwin/runtime_io_watch_darwin_internal.h
 * @brief Darwin/kqueue I/O watch backend private declarations.
 *
 * @details
 * The Darwin backend uses kqueue for request readiness and multishot-style watch
 * state. This header collects private contracts shared by the Darwin state,
 * control, worker, event, completion, and migration files.
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

#ifndef LLAM_RUNTIME_IO_WATCH_DARWIN_INTERNAL_H
#define LLAM_RUNTIME_IO_WATCH_DARWIN_INTERNAL_H

#include "runtime_internal.h"

#include <sys/event.h>

/** Maximum kevents submitted/read in one Darwin backend batch. */
#define LLAM_DARWIN_KEVENT_BATCH 32U

/** @brief Temporary accept completion list node drained after releasing locks. */
typedef struct llam_darwin_accept_completion {
    llam_io_req_t *req;
    int result;
    struct llam_darwin_accept_completion *next;
} llam_darwin_accept_completion_t;

/** @brief Temporary poll completion list node containing detached waiters. */
typedef struct llam_darwin_poll_completion {
    llam_io_req_t *waiters;
    int revents;
    struct llam_darwin_poll_completion *next;
} llam_darwin_poll_completion_t;

/** @brief Temporary recv completion list node with copied payload ownership. */
typedef struct llam_darwin_recv_completion {
    llam_io_req_t *req;
    size_t size;
    unsigned char *copy_data;
    size_t copy_capacity;
    struct llam_darwin_recv_completion *next;
} llam_darwin_recv_completion_t;

/* Shared queue/watch helpers also used by Linux-side common code. */
uint64_t llam_hash_watch_identity_u64(uint64_t value);
void llam_shard_note_inflight_io_waiter(unsigned owner_shard, int delta);
void llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req);
bool llam_remove_node_submit_locked(llam_node_t *node, llam_io_req_t *req);
llam_io_req_t *llam_take_node_submissions(llam_node_t *node);
void llam_poll_watch_enqueue_waiter(llam_poll_watch_t *watch, llam_io_req_t *req);
bool llam_poll_watch_remove_waiter(llam_poll_watch_t *watch, llam_io_req_t *req);
llam_io_req_t *llam_poll_watch_take_waiters(llam_poll_watch_t *watch);
void llam_accept_watch_enqueue_waiter(llam_accept_watch_t *watch, llam_io_req_t *req);
int llam_accept_watch_pop_ready(llam_accept_watch_t *watch);
bool llam_accept_watch_remove_waiter(llam_accept_watch_t *watch, llam_io_req_t *req);
llam_io_req_t *llam_accept_watch_pop_waiter(llam_accept_watch_t *watch);
bool llam_accept_watch_push_ready_owned(llam_accept_watch_t *watch, int fd);
void llam_accept_watch_push_ready(llam_accept_watch_t *watch, int fd);
llam_io_req_t *llam_recv_watch_pop_waiter(llam_recv_watch_t *watch);
bool llam_recv_watch_push_ready_copy(llam_recv_watch_t *watch, const unsigned char *data, size_t size);
bool llam_capture_recv_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino);
llam_poll_watch_t *llam_find_poll_watch_locked(llam_node_t *node, int fd, short events);
llam_accept_watch_t *llam_find_accept_watch_locked(llam_node_t *node, int fd);
llam_recv_watch_t *llam_find_recv_watch_locked(llam_node_t *node, int fd, dev_t st_dev, ino_t st_ino);
llam_poll_watch_t *llam_get_or_create_poll_watch_locked(llam_node_t *node, int fd, short events);
llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, int fd);
llam_recv_watch_t *llam_get_or_create_recv_watch_locked(llam_node_t *node, int fd);
void llam_destroy_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch);
void llam_destroy_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch);
void llam_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch);
void llam_maybe_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch);
bool llam_darwin_accept_completion_push(llam_darwin_accept_completion_t **head,
                                      llam_darwin_accept_completion_t **tail,
                                      llam_io_req_t *req,
                                      int result);
bool llam_darwin_poll_completion_push(llam_darwin_poll_completion_t **head,
                                    llam_darwin_poll_completion_t **tail,
                                    llam_io_req_t *waiters,
                                    int revents);
void llam_darwin_poll_completion_drain(llam_node_t *node, llam_darwin_poll_completion_t *head);
bool llam_darwin_recv_completion_push(llam_darwin_recv_completion_t **head,
                                    llam_darwin_recv_completion_t **tail,
                                    llam_io_req_t *req,
                                    size_t size,
                                    unsigned char *copy_data,
                                    size_t copy_capacity);
bool llam_darwin_recv_completion_push_copy(llam_darwin_recv_completion_t **head,
                                         llam_darwin_recv_completion_t **tail,
                                         llam_io_req_t *req,
                                         const unsigned char *data,
                                         size_t size);
void llam_darwin_recv_completion_drain(llam_node_t *node, llam_darwin_recv_completion_t *head);
bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target);

/* Node control queue and watch waiter helpers. */
int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target);
int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target);
void llam_recv_watch_enqueue_waiter(llam_recv_watch_t *watch, llam_io_req_t *req);
bool llam_recv_watch_remove_waiter(llam_recv_watch_t *watch, llam_io_req_t *req);
bool llam_recv_watch_pop_ready(llam_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out);
llam_io_control_op_t *llam_take_node_controls(llam_node_t *node);
int llam_darwin_kevent_apply(llam_node_t *node, struct kevent *changes, int change_count);
int llam_darwin_poll_watch_change(llam_node_t *node, llam_poll_watch_t *watch, uint16_t flags);
int llam_darwin_accept_watch_change(llam_node_t *node, llam_accept_watch_t *watch, uint16_t flags);
int llam_darwin_recv_watch_change(llam_node_t *node, llam_recv_watch_t *watch, uint16_t flags);
int llam_darwin_req_change_one(llam_node_t *node, llam_io_req_t *req, int16_t filter, uint16_t flags);
int llam_darwin_req_register(llam_node_t *node, llam_io_req_t *req);
void llam_darwin_req_delete(llam_node_t *node, llam_io_req_t *req);

/* Request completion, direct syscall, and kqueue event helpers. */
short llam_darwin_poll_revents(const struct kevent *event);
void llam_io_complete_req(llam_node_t *node, llam_io_req_t *req, int res, bool decrement_pending);
void llam_darwin_complete_accept_completions(llam_node_t *node, llam_darwin_accept_completion_t *head);
int llam_darwin_fd_set_nonblocking(int fd, int *saved_flags_out, bool *restore_out);
void llam_darwin_fd_restore(int fd, int saved_flags, bool restore);
int llam_socket_connect_error(llam_fd_t fd);
bool llam_darwin_assign_owned_buffer(llam_io_req_t *req,
                                   const unsigned char *data,
                                   size_t size,
                                   unsigned char *owned_data,
                                   size_t owned_capacity);
int llam_darwin_try_req_syscall(llam_io_req_t *req, int *result_out);

void llam_darwin_process_control(llam_node_t *node, llam_io_control_op_t *op);
void llam_darwin_process_submissions(llam_node_t *node);
void llam_darwin_queue_shutdown_controls(llam_node_t *node);
void *llam_io_worker_main(void *arg);

/* Watch arming, live event forwarding, and rehome finalization. */
bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node);
bool llam_arm_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node);
bool llam_arm_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node);
unsigned llam_multishot_owner_node_index(llam_runtime_t *rt, unsigned fallback_node_index, int fd);
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
                                      const unsigned char *data,
                                      size_t size);
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

void llam_darwin_handle_poll_watch_event(llam_node_t *node, llam_poll_watch_t *watch, short revents);
void llam_darwin_handle_accept_watch_event(llam_node_t *node, llam_accept_watch_t *watch);
void llam_darwin_handle_recv_watch_event(llam_node_t *node, llam_recv_watch_t *watch);
void llam_darwin_handle_req_event(llam_node_t *node, llam_io_req_t *req, const struct kevent *event);
void llam_darwin_submit_req(llam_node_t *node, llam_io_req_t *req);

unsigned llam_watch_migration_target_index(llam_runtime_t *rt,
                                         unsigned fallback_target_index,
                                         unsigned current_target_index,
                                         int fd);
bool llam_io_rehome_watch_state_filtered(llam_node_t *source, llam_node_t *target, bool only_marked);
bool llam_io_rehome_idle_watch_state(llam_node_t *source, llam_node_t *target);
bool llam_io_rehome_marked_watch_state(llam_node_t *source, llam_node_t *target);
bool llam_io_req_transfer_inflight_owner(llam_io_req_t *req, unsigned from_shard, unsigned to_shard);

#endif
