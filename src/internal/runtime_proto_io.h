/**
 * @file src/internal/runtime_proto_io.h
 * @brief Internal prototypes for public and backend I/O operations.
 *
 * @details
 * I/O declarations are grouped by ownership: request lifecycle, backend node
 * setup, watch queues, multishot rehome, and user-data encoding. Completion
 * paths rely on the ownership fields documented here, so keep request/waiter
 * helpers close to the code that mutates those fields.
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

#ifndef LLAM_RUNTIME_PROTO_IO_H
#define LLAM_RUNTIME_PROTO_IO_H

#include "runtime_state.h"

/*
 * Request cancellation and parking ownership.
 */
bool llam_abort_io_wait(llam_task_t *task, llam_io_abort_reason_t reason);
llam_wait_reason_t llam_io_abort_wait_reason(llam_io_abort_reason_t reason);
void llam_io_set_abort_result(llam_io_req_t *req, llam_io_abort_reason_t reason);
bool llam_io_req_transfer_inflight_owner(llam_io_req_t *req, unsigned from_shard, unsigned to_shard);
uint64_t llam_hash_watch_identity_u64(uint64_t value);
llam_io_req_t *llam_task_active_io_req_load(const llam_task_t *task);
void llam_task_set_io_tracking(llam_task_t *task, llam_io_req_t *req, unsigned parked_shard);

static inline bool llam_io_req_abort_requested(const llam_io_req_t *req) {
    return req != NULL &&
           (atomic_load_explicit(&req->cancel_queued, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->abort_reason, memory_order_acquire) != LLAM_IO_ABORT_NONE);
}

/*
 * Watch-list waiter queues. The watch lock protects these lists.
 */
void llam_accept_watch_enqueue_waiter(llam_accept_watch_t *watch, llam_io_req_t *req);
int llam_accept_watch_pop_ready(llam_accept_watch_t *watch);
bool llam_accept_watch_remove_waiter(llam_accept_watch_t *watch, llam_io_req_t *req);
void llam_accept_watch_push_waiter_front(llam_accept_watch_t *watch, llam_io_req_t *req);
void llam_poll_watch_enqueue_waiter(llam_poll_watch_t *watch, llam_io_req_t *req);
bool llam_poll_watch_remove_waiter(llam_poll_watch_t *watch, llam_io_req_t *req);
llam_io_req_t *llam_poll_watch_take_waiters(llam_poll_watch_t *watch);
void llam_poll_watch_push_waiters_front(llam_poll_watch_t *watch, llam_io_req_t *waiters);
void llam_recv_watch_enqueue_waiter(llam_recv_watch_t *watch, llam_io_req_t *req);
bool llam_recv_watch_remove_waiter(llam_recv_watch_t *watch, llam_io_req_t *req);
void llam_recv_watch_push_waiter_front(llam_recv_watch_t *watch, llam_io_req_t *req);
bool llam_recv_watch_pop_ready_shared(llam_recv_watch_t *watch,
                                      size_t *size_out,
                                      unsigned short *bid_out,
                                      bool *has_buffer_out,
                                      unsigned *node_index_out,
                                      unsigned char **copy_data_out,
                                      size_t *copy_capacity_out);
bool llam_recv_watch_pop_ready(llam_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out);

/*
 * Watch lookup/creation. Callers must hold node->watch_lock for *_locked APIs.
 */
llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, llam_fd_t fd);
llam_poll_watch_t *llam_get_or_create_poll_watch_locked(llam_node_t *node, llam_fd_t fd, short events);
llam_recv_watch_t *llam_get_or_create_recv_watch_locked(llam_node_t *node, llam_fd_t fd);
void llam_destroy_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch);
void llam_destroy_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch);
void llam_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch);
void llam_maybe_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch);
void llam_forget_closed_fd_watch_state(llam_runtime_t *rt, llam_fd_t fd);
void llam_accept_watch_splice_ready(llam_accept_watch_t *target, llam_accept_watch_t *source);
void llam_recv_watch_splice_ready(llam_recv_watch_t *target, llam_recv_watch_t *source);

/*
 * Request allocation, capability checks, and submission queues.
 */
bool llam_io_capability_error(int error_code);
void llam_io_req_reset(llam_io_req_t *req, llam_runtime_t *owner_runtime, unsigned owner_shard, unsigned alloc_owner_shard);
llam_io_req_t *llam_io_req_alloc(llam_shard_t *shard);
void llam_io_req_free(llam_shard_t *shard, llam_io_req_t *req);
int llam_io_req_node_index(const llam_io_req_t *req);
void llam_shard_note_inflight_io_waiter(llam_runtime_t *rt, unsigned owner_shard, int delta);
bool llam_node_note_pending_ops(llam_node_t *node, unsigned amount);
bool llam_node_complete_pending_ops(llam_node_t *node, unsigned amount);
bool llam_io_completion_begin(llam_node_t *node, llam_io_req_t *req, bool decrement_pending);
bool llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req);
bool llam_node_submit_io_req(llam_node_t *node, llam_io_req_t *req);
bool llam_remove_node_submit_locked(llam_node_t *node, llam_io_req_t *req);
llam_io_req_t *llam_take_node_submissions(llam_node_t *node);

/*
 * Live watch and waiter rehome support for dynamic shard/node movement.
 */
bool llam_io_rehome_idle_watch_state(llam_node_t *source, llam_node_t *target);
bool llam_io_rehome_marked_watch_state(llam_node_t *source, llam_node_t *target);
unsigned llam_multishot_owner_node_index(llam_runtime_t *rt, unsigned fallback_node_index, llam_fd_t fd);
bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node);
bool llam_arm_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node);
bool llam_arm_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node);
bool llam_arm_watch_locked_common(llam_node_t *node,
                                  bool *active,
                                  bool *activating,
                                  bool *deactivate_queued,
                                  llam_io_control_kind_t deactivate_kind,
                                  llam_io_control_kind_t activate_kind,
                                  void *target,
                                  bool *kick_node);
void llam_io_lock_rehome_pair(llam_node_t *source,
                              llam_node_t *target,
                              llam_node_t **source_locked,
                              llam_node_t **target_locked);
void llam_io_unlock_rehome_pair(llam_node_t *source, llam_node_t *target);
unsigned llam_watch_migration_target_index(llam_runtime_t *rt,
                                           unsigned fallback_target_index,
                                           unsigned current_target_index,
                                           llam_fd_t fd);
unsigned llam_watch_migration_target_or_none(unsigned desired_target_index, unsigned source_index);
void llam_poll_watch_note_waiter_migration(llam_poll_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index);
void llam_accept_watch_note_waiter_migration(llam_accept_watch_t *watch,
                                             unsigned desired_target_index,
                                             unsigned source_index);
void llam_recv_watch_note_waiter_migration(llam_recv_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index);
void llam_poll_watch_mark_live_migration(llam_poll_watch_t *watch,
                                         unsigned desired_target_index,
                                         unsigned source_index);
void llam_accept_watch_mark_live_migration(llam_accept_watch_t *watch,
                                           unsigned desired_target_index,
                                           unsigned source_index);
void llam_recv_watch_mark_live_migration(llam_recv_watch_t *watch,
                                         unsigned desired_target_index,
                                         unsigned source_index);

/*
 * Backend node setup, probing, and worker lifecycle.
 */
bool llam_io_sqpoll_setup_error(int error_code);
void *llam_io_worker_main(void *arg);
void llam_node_destroy_recv_buf_ring(llam_node_t *node);
int llam_node_init_ring(llam_runtime_t *rt, llam_node_t *node);
int llam_node_recycle_recv_buffer(llam_node_t *node, unsigned short bid);
void llam_node_unregister_cq_eventfd(llam_node_t *node);
int llam_node_setup_recv_buf_ring(llam_node_t *node);
int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target);
int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target);
bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target);
bool llam_node_supports_kind(const llam_node_t *node, llam_io_kind_t kind);
void llam_io_queue_shutdown_controls_common(llam_node_t *node);
void llam_probe_ring_support(llam_node_t *node);
void llam_io_buffer_public_detach_runtime_storage(llam_runtime_t *rt);

/*
 * io_uring/kqueue user-data tagging. Pointers are encoded with small type tags
 * so completion dispatch can distinguish requests from watch/control CQEs.
 */
uint64_t llam_io_udata_encode(void *ptr, unsigned tag);
unsigned llam_io_udata_tag(uint64_t user_data);
void *llam_io_udata_ptr(uint64_t user_data);

#endif
