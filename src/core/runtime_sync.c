/**
 * @file src/core/runtime_sync.c
 * @brief Shared synchronization and wait-list primitives used by mutexes, condvars, and channels.
 *
 * @details
 * Synchronization primitives share a small wait-node abstraction. The current
 * task's embedded wait node handles the common one-wait-at-a-time case without
 * allocation; heap wait nodes are used only for nested or exceptional ownership
 * patterns.
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

/**
 * @brief Require that the caller is running inside a managed task context.
 *
 * @return 0 when runtime, task, and shard TLS are all present.
 * @return -1 with @c errno set to @c ENOTSUP otherwise.
 */
int llam_require_task_context(void) {
    if (!g_llam_runtime.initialized || g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

/**
 * @brief Acquire a wait node for a synchronization primitive.
 *
 * The current task's embedded wait node is used when it is free. Otherwise a
 * shard-local heap wait node is allocated.
 *
 * @param shard Shard requesting the wait node.
 *
 * @return Wait node on success, or @c NULL on allocation failure.
 */
llam_wait_node_t *llam_sync_wait_node_acquire(llam_shard_t *shard) {
    llam_task_t *task = g_llam_tls_task;
    llam_wait_node_t *node;

    if (task != NULL && task->active_wait_node == NULL) {
        node = &task->embedded_wait_node;
        memset(node, 0, sizeof(*node));
        node->task = task;
        node->owner_shard = UINT_MAX;
        return node;
    }

    node = llam_wait_node_alloc(shard);
    if (node != NULL) {
        node->task = task;
        node->owner_shard = shard != NULL ? shard->id : UINT_MAX;
    }
    return node;
}

/**
 * @brief Release a wait node acquired by ::llam_sync_wait_node_acquire.
 *
 * Embedded nodes are zeroed in place. Heap nodes are returned to the shard-local
 * allocator.
 *
 * @param shard Shard owning the allocator.
 * @param node  Wait node to release; may be @c NULL.
 */
void llam_sync_wait_node_release(llam_shard_t *shard, llam_wait_node_t *node) {
    if (node == NULL) {
        return;
    }
    if (node->task != NULL && node == &node->task->embedded_wait_node) {
        memset(node, 0, sizeof(*node));
        return;
    }
    llam_wait_node_free(shard, node);
}

/**
 * @brief Wake and remove every waiter from a wait queue.
 *
 * @param queue      Queue to drain.
 * @param error_code Completion error stored in each wait node.
 * @param reason     Trace/wake reason associated with the wakeup.
 */
void llam_wake_wait_queue_all(llam_wait_queue_t *queue, int error_code, llam_wait_reason_t reason) {
    llam_wait_node_t *node;

    while ((node = llam_wait_queue_pop_head(queue)) != NULL) {
        node->error_code = error_code;
        llam_wake_wait_node(node, true, reason);
    }
}
