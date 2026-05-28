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
 * Public mutex/cond handle encoding.
 *
 * These objects can be cached or reallocated after destroy. Public handles use
 * a registry slot plus family-tagged generation so stale handles cannot alias a
 * replacement object or a different sync object family using the same slot.
 */
#if UINTPTR_MAX <= UINT32_MAX
#error "LLAM sync public handles require uintptr_t wider than 32 bits"
#define LLAM_SYNC_PUBLIC_HANDLE_SHIFT 0U
#else
#define LLAM_SYNC_PUBLIC_HANDLE_SHIFT 32U
#endif

#define LLAM_CHANNEL_SELECT_INLINE_OPS 8U
#define LLAM_SELECT_PENDING 0U
#define LLAM_SELECT_COMPLETING 1U
#define LLAM_SELECT_COMPLETED_INLINE 2U
#define LLAM_SELECT_COMPLETED_QUEUED 3U
#define LLAM_SELECT_TRY_NOT_READY 0
#define LLAM_SELECT_TRY_SELECTED 1
#define LLAM_SELECT_TRY_FALLBACK 2

llam_mutex_t *llam_mutex_resolve_public_handle(const llam_mutex_t *handle);
void llam_mutex_end_public_op(llam_mutex_t *mutex);
llam_cond_t *llam_cond_resolve_public_handle(const llam_cond_t *handle);
void llam_cond_end_public_op(llam_cond_t *cond);

static inline llam_mutex_t *llam_mutex_public_handle(llam_mutex_t *mutex) {
    if (mutex == NULL) {
        return NULL;
    }
    return (llam_mutex_t *)llam_public_slot_encode_handle(mutex->public_handle_slot,
                                                          mutex->public_handle_generation,
                                                          LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
}

static inline llam_cond_t *llam_cond_public_handle(llam_cond_t *cond) {
    if (cond == NULL) {
        return NULL;
    }
    return (llam_cond_t *)llam_public_slot_encode_handle(cond->public_handle_slot,
                                                         cond->public_handle_generation,
                                                         LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
}

/*
 * Capacity-one channel cache. This keeps ping-pong style channels from
 * repeatedly allocating and freeing the same small object shape.
 */
llam_channel_t *llam_channel_cache_acquire(void);
bool llam_channel_cache_release(llam_channel_t *channel);
int llam_channel_register_live(llam_channel_t *channel);
void llam_channel_tls_cache_drain(void);
void llam_channel_global_cache_drain(void);
void llam_channel_hot_safepoint(void);
int llam_channel_try_send_buffered_fast(llam_channel_t *handle, void *value);

/*
 * Public channel-handle encoding.
 *
 * Capacity-one channels are intentionally recycled through TLS/global caches.
 * Public handles therefore use a registry slot plus family-tagged generation
 * instead of a raw pointer. That rejects stale consumed handles and wrong-family
 * sync handles even after many cache reuses of the same channel object address.
 */
llam_channel_t *llam_channel_resolve_public_handle(const llam_channel_t *handle);
void llam_channel_end_public_op(llam_channel_t *channel);
void llam_channel_public_registry_lock(void);
void llam_channel_public_registry_unlock(void);
llam_channel_t *llam_channel_resolve_public_handle_locked_unpinned(const llam_channel_t *handle);
int llam_channel_resolve_public_handles_for_select(llam_select_op_t *ops,
                                                   size_t op_count,
                                                   llam_channel_t **out_channels);
void llam_channel_end_public_select_ops(llam_channel_t **channels, size_t op_count);

static inline llam_channel_t *llam_channel_public_handle(llam_channel_t *channel) {
    if (channel == NULL) {
        return NULL;
    }
    return (llam_channel_t *)llam_public_slot_encode_handle(channel->public_handle_slot,
                                                            channel->public_handle_generation,
                                                            LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
}

/*
 * Mutex public-handle and already-resolved entry points.  Condvar waits keep a
 * public-op pin on the associated mutex while parked, then use the resolved
 * entry point to avoid treating the raw mutex address as an opaque public
 * slot/generation handle during reacquire.
 */
int llam_mutex_lock_impl(llam_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns, bool register_cancel);
int llam_mutex_lock_resolved_impl(llam_mutex_t *mutex,
                                  bool has_deadline,
                                  uint64_t deadline_ns,
                                  bool register_cancel);

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
void llam_wait_node_reset(llam_wait_node_t *node, llam_runtime_t *owner_runtime, unsigned owner_shard);
bool llam_sync_note_inflight_waiter(llam_runtime_t *rt, atomic_uint *counter, unsigned amount);
bool llam_sync_complete_inflight_waiter(llam_runtime_t *rt, atomic_uint *counter, unsigned amount);
bool llam_wait_node_prepare_wake(llam_wait_node_t *node);
bool llam_wait_node_completed(const llam_wait_node_t *node);
bool llam_wait_node_should_park(llam_wait_node_t *node);
void llam_channel_waiter_consumed(llam_channel_t *channel);
int llam_channel_select_validate_op(const llam_select_op_t *op);
int llam_channel_select_try_ready_large(llam_select_op_t *ops,
                                        size_t op_count,
                                        size_t start,
                                        size_t *selected_index);
int llam_channel_select_try_ready_batch(llam_select_op_t *ops,
                                        size_t op_count,
                                        size_t start,
                                        size_t *selected_index);
int llam_channel_select_try_one_fast(llam_select_op_t *op);
int llam_channel_select_try_one(llam_select_op_t *op);

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
