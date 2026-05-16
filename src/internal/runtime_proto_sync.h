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

#ifndef LLAM_RUNTIME_PROTO_SYNC_H
#define LLAM_RUNTIME_PROTO_SYNC_H

#include "runtime_state.h"

/*
 * Managed-task context validation.
 */
int llam_require_task_context(void);

/*
 * Capacity-one channel cache. This keeps ping-pong style channels from
 * repeatedly allocating and freeing the same small object shape.
 */
llam_channel_t *llam_channel_cache_acquire(void);
bool llam_channel_cache_release(llam_channel_t *channel);

/*
 * Mutex internal entry point shared by normal, timed, and condvar reacquire
 * paths. Condvar waits pass register_cancel=false while reacquiring the mutex
 * after wakeup so cancellation is reported by the condition wait itself.
 */
int llam_mutex_lock_impl(llam_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns, bool register_cancel);

/*
 * Wait-node allocation and task tracking.
 *
 * The current task's embedded wait node is preferred; heap nodes are only used
 * when the embedded slot is already occupied.
 */
llam_wait_node_t *llam_sync_wait_node_acquire(llam_shard_t *shard);
void llam_sync_wait_node_release(llam_shard_t *shard, llam_wait_node_t *node);
void llam_task_set_wait_node_tracking(llam_task_t *task,
                                    llam_wait_node_t *node,
                                    llam_wait_queue_t *queue,
                                    pthread_mutex_t *queue_lock,
                                    unsigned parked_shard);
llam_wait_node_t *llam_wait_node_alloc(llam_shard_t *shard);
void llam_wait_node_free(llam_shard_t *shard, llam_wait_node_t *node);
bool llam_wait_node_prepare_wake(llam_wait_node_t *node);
bool llam_wait_node_should_park(llam_wait_node_t *node);

/*
 * FIFO wait-queue primitives and wake dispatch.
 */
llam_wait_node_t *llam_wait_queue_pop_head(llam_wait_queue_t *queue);
void llam_wait_queue_push_tail(llam_wait_queue_t *queue, llam_wait_node_t *node);
bool llam_wait_queue_remove(llam_wait_queue_t *queue, llam_wait_node_t *node);
void llam_wake_wait_node(llam_wait_node_t *node, bool hot, llam_wait_reason_t reason);
void llam_wake_wait_queue_all(llam_wait_queue_t *queue, int error_code, llam_wait_reason_t reason);
bool llam_channel_select_complete_node(llam_wait_node_t *node, void *value, int error_code);
bool llam_channel_select_node_should_wake(llam_wait_node_t *node);
bool llam_channel_select_abort_task_wait(llam_task_t *task, int error_code, llam_wait_reason_t reason);

#endif
