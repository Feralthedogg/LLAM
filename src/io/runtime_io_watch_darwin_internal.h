/**
 * @file src/io/runtime_io_watch_darwin_internal.h
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
#define NM_DARWIN_KEVENT_BATCH 32U

/** @brief Temporary accept completion list node drained after releasing locks. */
typedef struct nm_darwin_accept_completion {
    nm_io_req_t *req;
    int result;
    struct nm_darwin_accept_completion *next;
} nm_darwin_accept_completion_t;

/** @brief Temporary poll completion list node containing detached waiters. */
typedef struct nm_darwin_poll_completion {
    nm_io_req_t *waiters;
    int revents;
    struct nm_darwin_poll_completion *next;
} nm_darwin_poll_completion_t;

/** @brief Temporary recv completion list node with copied payload ownership. */
typedef struct nm_darwin_recv_completion {
    nm_io_req_t *req;
    size_t size;
    unsigned char *copy_data;
    size_t copy_capacity;
    struct nm_darwin_recv_completion *next;
} nm_darwin_recv_completion_t;

/* Shared queue/watch helpers also used by Linux-side common code. */
uint64_t nm_hash_watch_identity_u64(uint64_t value);
void nm_shard_note_inflight_io_waiter(unsigned owner_shard, int delta);
void nm_queue_node_submit_locked(nm_node_t *node, nm_io_req_t *req);
bool nm_remove_node_submit_locked(nm_node_t *node, nm_io_req_t *req);
nm_io_req_t *nm_take_node_submissions(nm_node_t *node);
void nm_poll_watch_enqueue_waiter(nm_poll_watch_t *watch, nm_io_req_t *req);
bool nm_poll_watch_remove_waiter(nm_poll_watch_t *watch, nm_io_req_t *req);
nm_io_req_t *nm_poll_watch_take_waiters(nm_poll_watch_t *watch);
void nm_accept_watch_enqueue_waiter(nm_accept_watch_t *watch, nm_io_req_t *req);
int nm_accept_watch_pop_ready(nm_accept_watch_t *watch);
bool nm_accept_watch_remove_waiter(nm_accept_watch_t *watch, nm_io_req_t *req);
nm_io_req_t *nm_accept_watch_pop_waiter(nm_accept_watch_t *watch);
bool nm_accept_watch_push_ready_owned(nm_accept_watch_t *watch, int fd);
void nm_accept_watch_push_ready(nm_accept_watch_t *watch, int fd);
nm_io_req_t *nm_recv_watch_pop_waiter(nm_recv_watch_t *watch);
bool nm_recv_watch_push_ready_copy(nm_recv_watch_t *watch, const unsigned char *data, size_t size);
bool nm_capture_recv_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino);
nm_poll_watch_t *nm_find_poll_watch_locked(nm_node_t *node, int fd, short events);
nm_accept_watch_t *nm_find_accept_watch_locked(nm_node_t *node, int fd);
nm_recv_watch_t *nm_find_recv_watch_locked(nm_node_t *node, int fd, dev_t st_dev, ino_t st_ino);
nm_poll_watch_t *nm_get_or_create_poll_watch_locked(nm_node_t *node, int fd, short events);
nm_accept_watch_t *nm_get_or_create_accept_watch_locked(nm_node_t *node, int fd);
nm_recv_watch_t *nm_get_or_create_recv_watch_locked(nm_node_t *node, int fd);
void nm_destroy_poll_watch_locked(nm_node_t *node, nm_poll_watch_t *watch);
void nm_destroy_accept_watch_locked(nm_node_t *node, nm_accept_watch_t *watch);
void nm_destroy_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch);
void nm_maybe_destroy_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch);
bool nm_darwin_accept_completion_push(nm_darwin_accept_completion_t **head,
                                      nm_darwin_accept_completion_t **tail,
                                      nm_io_req_t *req,
                                      int result);
bool nm_darwin_poll_completion_push(nm_darwin_poll_completion_t **head,
                                    nm_darwin_poll_completion_t **tail,
                                    nm_io_req_t *waiters,
                                    int revents);
void nm_darwin_poll_completion_drain(nm_node_t *node, nm_darwin_poll_completion_t *head);
bool nm_darwin_recv_completion_push(nm_darwin_recv_completion_t **head,
                                    nm_darwin_recv_completion_t **tail,
                                    nm_io_req_t *req,
                                    size_t size,
                                    unsigned char *copy_data,
                                    size_t copy_capacity);
bool nm_darwin_recv_completion_push_copy(nm_darwin_recv_completion_t **head,
                                         nm_darwin_recv_completion_t **tail,
                                         nm_io_req_t *req,
                                         const unsigned char *data,
                                         size_t size);
void nm_darwin_recv_completion_drain(nm_node_t *node, nm_darwin_recv_completion_t *head);
bool nm_drop_node_control_locked(nm_node_t *node, nm_io_control_kind_t kind, const void *target);

/* Node control queue and watch waiter helpers. */
int nm_node_queue_control(nm_node_t *node, nm_io_control_kind_t kind, void *target);
int nm_node_queue_control_locked(nm_node_t *node, nm_io_control_kind_t kind, void *target);
void nm_recv_watch_enqueue_waiter(nm_recv_watch_t *watch, nm_io_req_t *req);
bool nm_recv_watch_remove_waiter(nm_recv_watch_t *watch, nm_io_req_t *req);
bool nm_recv_watch_pop_ready(nm_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out);
nm_io_control_op_t *nm_take_node_controls(nm_node_t *node);
int nm_darwin_kevent_apply(nm_node_t *node, struct kevent *changes, int change_count);
int nm_darwin_poll_watch_change(nm_node_t *node, nm_poll_watch_t *watch, uint16_t flags);
int nm_darwin_accept_watch_change(nm_node_t *node, nm_accept_watch_t *watch, uint16_t flags);
int nm_darwin_recv_watch_change(nm_node_t *node, nm_recv_watch_t *watch, uint16_t flags);
int nm_darwin_req_change_one(nm_node_t *node, nm_io_req_t *req, int16_t filter, uint16_t flags);
int nm_darwin_req_register(nm_node_t *node, nm_io_req_t *req);
void nm_darwin_req_delete(nm_node_t *node, nm_io_req_t *req);

/* Request completion, direct syscall, and kqueue event helpers. */
short nm_darwin_poll_revents(const struct kevent *event);
void nm_io_complete_req(nm_node_t *node, nm_io_req_t *req, int res, bool decrement_pending);
void nm_darwin_complete_accept_completions(nm_node_t *node, nm_darwin_accept_completion_t *head);
int nm_darwin_fd_set_nonblocking(int fd, int *saved_flags_out, bool *restore_out);
void nm_darwin_fd_restore(int fd, int saved_flags, bool restore);
bool nm_darwin_assign_owned_buffer(nm_io_req_t *req,
                                   const unsigned char *data,
                                   size_t size,
                                   unsigned char *owned_data,
                                   size_t owned_capacity);
int nm_darwin_try_req_syscall(nm_io_req_t *req, int *result_out);

void nm_darwin_process_control(nm_node_t *node, nm_io_control_op_t *op);
void nm_darwin_process_submissions(nm_node_t *node);
void nm_darwin_queue_shutdown_controls(nm_node_t *node);
void *nm_io_worker_main(void *arg);

/* Watch arming, live event forwarding, and rehome finalization. */
bool nm_arm_poll_watch_locked(nm_node_t *node, nm_poll_watch_t *watch, bool *kick_node);
bool nm_arm_accept_watch_locked(nm_node_t *node, nm_accept_watch_t *watch, bool *kick_node);
bool nm_arm_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch, bool *kick_node);
unsigned nm_multishot_owner_node_index(nm_runtime_t *rt, unsigned fallback_node_index, int fd);
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
                                      const unsigned char *data,
                                      size_t size);
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

void nm_darwin_handle_poll_watch_event(nm_node_t *node, nm_poll_watch_t *watch, short revents);
void nm_darwin_handle_accept_watch_event(nm_node_t *node, nm_accept_watch_t *watch);
void nm_darwin_handle_recv_watch_event(nm_node_t *node, nm_recv_watch_t *watch);
void nm_darwin_handle_req_event(nm_node_t *node, nm_io_req_t *req, const struct kevent *event);
void nm_darwin_submit_req(nm_node_t *node, nm_io_req_t *req);

unsigned nm_watch_migration_target_index(nm_runtime_t *rt,
                                         unsigned fallback_target_index,
                                         unsigned current_target_index,
                                         int fd);
bool nm_io_rehome_watch_state_filtered(nm_node_t *source, nm_node_t *target, bool only_marked);
bool nm_io_rehome_idle_watch_state(nm_node_t *source, nm_node_t *target);
bool nm_io_rehome_marked_watch_state(nm_node_t *source, nm_node_t *target);
bool nm_io_req_transfer_inflight_owner(nm_io_req_t *req, unsigned from_shard, unsigned to_shard);

#endif
