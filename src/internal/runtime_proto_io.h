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

#ifndef NM_RUNTIME_PROTO_IO_H
#define NM_RUNTIME_PROTO_IO_H

#include "runtime_state.h"

/*
 * Request cancellation and parking ownership.
 */
bool nm_abort_io_wait(nm_task_t *task, nm_io_abort_reason_t reason);
nm_wait_reason_t nm_io_abort_wait_reason(nm_io_abort_reason_t reason);
void nm_io_set_abort_result(nm_io_req_t *req, nm_io_abort_reason_t reason);
bool nm_io_req_transfer_inflight_owner(nm_io_req_t *req, unsigned from_shard, unsigned to_shard);
void nm_task_set_io_tracking(nm_task_t *task, nm_io_req_t *req, unsigned parked_shard);

/*
 * Watch-list waiter queues. The watch lock protects these lists.
 */
void nm_accept_watch_enqueue_waiter(nm_accept_watch_t *watch, nm_io_req_t *req);
int nm_accept_watch_pop_ready(nm_accept_watch_t *watch);
bool nm_accept_watch_remove_waiter(nm_accept_watch_t *watch, nm_io_req_t *req);
void nm_poll_watch_enqueue_waiter(nm_poll_watch_t *watch, nm_io_req_t *req);
bool nm_poll_watch_remove_waiter(nm_poll_watch_t *watch, nm_io_req_t *req);
void nm_recv_watch_enqueue_waiter(nm_recv_watch_t *watch, nm_io_req_t *req);
bool nm_recv_watch_remove_waiter(nm_recv_watch_t *watch, nm_io_req_t *req);
bool nm_recv_watch_pop_ready(nm_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out);

/*
 * Watch lookup/creation. Callers must hold node->watch_lock for *_locked APIs.
 */
nm_accept_watch_t *nm_get_or_create_accept_watch_locked(nm_node_t *node, nm_fd_t fd);
nm_poll_watch_t *nm_get_or_create_poll_watch_locked(nm_node_t *node, nm_fd_t fd, short events);
nm_recv_watch_t *nm_get_or_create_recv_watch_locked(nm_node_t *node, nm_fd_t fd);
void nm_maybe_destroy_recv_watch_locked(nm_node_t *node, nm_recv_watch_t *watch);

/*
 * Request allocation, capability checks, and submission queues.
 */
bool nm_io_capability_error(int error_code);
nm_io_req_t *nm_io_req_alloc(nm_shard_t *shard);
void nm_io_req_free(nm_shard_t *shard, nm_io_req_t *req);
int nm_io_req_node_index(const nm_io_req_t *req);
void nm_queue_node_submit_locked(nm_node_t *node, nm_io_req_t *req);
bool nm_remove_node_submit_locked(nm_node_t *node, nm_io_req_t *req);

/*
 * Live watch and waiter rehome support for dynamic shard/node movement.
 */
bool nm_io_rehome_idle_watch_state(nm_node_t *source, nm_node_t *target);
bool nm_io_rehome_marked_watch_state(nm_node_t *source, nm_node_t *target);
unsigned nm_multishot_owner_node_index(nm_runtime_t *rt, unsigned fallback_node_index, nm_fd_t fd);

/*
 * Backend node setup, probing, and worker lifecycle.
 */
bool nm_io_sqpoll_setup_error(int error_code);
void *nm_io_worker_main(void *arg);
void nm_node_destroy_recv_buf_ring(nm_node_t *node);
int nm_node_init_ring(nm_runtime_t *rt, nm_node_t *node);
int nm_node_recycle_recv_buffer(nm_node_t *node, unsigned short bid);
void nm_node_unregister_cq_eventfd(nm_node_t *node);
int nm_node_setup_recv_buf_ring(nm_node_t *node);
int nm_node_queue_control(nm_node_t *node, nm_io_control_kind_t kind, void *target);
int nm_node_queue_control_locked(nm_node_t *node, nm_io_control_kind_t kind, void *target);
bool nm_node_supports_kind(const nm_node_t *node, nm_io_kind_t kind);
void nm_probe_ring_support(nm_node_t *node);

/*
 * io_uring/kqueue user-data tagging. Pointers are encoded with small type tags
 * so completion dispatch can distinguish requests from watch/control CQEs.
 */
uint64_t nm_io_udata_encode(void *ptr, unsigned tag);
unsigned nm_io_udata_tag(uint64_t user_data);
void *nm_io_udata_ptr(uint64_t user_data);

#endif
