/**
 * @file src/internal/runtime_proto_sync.h
 * @brief Internal prototypes for mutex, condition variable, channel, and wait-object operations.
 *
 * @details
 * Synchronization code shares wait-node allocation and FIFO queue operations.
 * Public sync primitives build on these helpers after validating that the caller
 * is running inside a managed task context.
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

#ifndef NM_RUNTIME_PROTO_SYNC_H
#define NM_RUNTIME_PROTO_SYNC_H

#include "runtime_state.h"

/*
 * Managed-task context validation.
 */
int nm_require_task_context(void);

/*
 * Capacity-one channel cache. This keeps ping-pong style channels from
 * repeatedly allocating and freeing the same small object shape.
 */
nm_channel_t *nm_channel_cache_acquire(void);
bool nm_channel_cache_release(nm_channel_t *channel);

/*
 * Mutex internal entry point shared by normal, timed, and condvar reacquire
 * paths. Condvar waits pass register_cancel=false while reacquiring the mutex
 * after wakeup so cancellation is reported by the condition wait itself.
 */
int nm_mutex_lock_impl(nm_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns, bool register_cancel);

/*
 * Wait-node allocation and task tracking.
 *
 * The current task's embedded wait node is preferred; heap nodes are only used
 * when the embedded slot is already occupied.
 */
nm_wait_node_t *nm_sync_wait_node_acquire(nm_shard_t *shard);
void nm_sync_wait_node_release(nm_shard_t *shard, nm_wait_node_t *node);
void nm_task_set_wait_node_tracking(nm_task_t *task,
                                    nm_wait_node_t *node,
                                    nm_wait_queue_t *queue,
                                    pthread_mutex_t *queue_lock,
                                    unsigned parked_shard);
nm_wait_node_t *nm_wait_node_alloc(nm_shard_t *shard);
void nm_wait_node_free(nm_shard_t *shard, nm_wait_node_t *node);

/*
 * FIFO wait-queue primitives and wake dispatch.
 */
nm_wait_node_t *nm_wait_queue_pop_head(nm_wait_queue_t *queue);
void nm_wait_queue_push_tail(nm_wait_queue_t *queue, nm_wait_node_t *node);
bool nm_wait_queue_remove(nm_wait_queue_t *queue, nm_wait_node_t *node);
void nm_wake_wait_node(nm_wait_node_t *node, bool hot, nm_wait_reason_t reason);
void nm_wake_wait_queue_all(nm_wait_queue_t *queue, int error_code, nm_wait_reason_t reason);

#endif
