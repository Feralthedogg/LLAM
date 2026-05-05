/**
 * @file src/core/runtime_allocator_quiescent.c
 * @brief Quiescent-state allocator reclamation helpers.
 *
 * @details
 * Cross-shard frees cannot safely splice objects directly into another shard's
 * local free lists without taking that shard's allocator lock. Instead, remote
 * producers push objects onto atomic remote-free lists and set a single pending
 * flag. The owner shard later calls ::nm_allocator_quiescent at a safe point to
 * drain those lists back into local caches in one locked batch.
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
 * @brief Drain remote-free lists into a shard's local allocator caches.
 *
 * @param shard Owner shard reaching a quiescent point.
 *
 * @note This function assumes @p shard is valid. Callers normally invoke it from
 *       scheduler safe points where the owner shard is active and can absorb
 *       remote returns without racing local cache users.
 */
void nm_allocator_quiescent(nm_shard_t *shard) {
    nm_allocator_t *allocator = &shard->allocator;
    nm_task_t *task_head;
    nm_wait_node_t *wait_head;
    nm_timer_node_t *timer_head;
    nm_io_req_t *io_head;
    nm_io_buffer_t *io_buffer_head;
    uint64_t task_burst = 0U;
    uint64_t wait_burst = 0U;
    uint64_t timer_burst = 0U;
    uint64_t io_burst = 0U;
    uint64_t io_buffer_burst = 0U;
    uint64_t epoch;

    // One flag covers all remote-free lists. If it is clear, avoid touching the
    // individual atomics and keep the safe-point fast path cheap.
    if (atomic_exchange_explicit(&allocator->remote_free_pending, 0U, memory_order_acquire) == 0U) {
        return;
    }

    // Publish a local epoch for diagnostics/reclamation accounting before
    // stealing the remote lists. Object safety is handled by ownership and the
    // atomic list exchange, not by waiting on this epoch.
    epoch = atomic_fetch_add(&g_nm_runtime.global_epoch, 1U) + 1U;
    atomic_store(&allocator->local_epoch, epoch);
    task_head = atomic_exchange(&allocator->task_remote_free, NULL);
    wait_head = atomic_exchange(&allocator->wait_remote_free, NULL);
    timer_head = atomic_exchange(&allocator->timer_remote_free, NULL);
    io_head = atomic_exchange(&allocator->io_req_remote_free, NULL);
    io_buffer_head = atomic_exchange(&allocator->io_buffer_remote_free, NULL);

    if (task_head == NULL && wait_head == NULL && timer_head == NULL && io_head == NULL && io_buffer_head == NULL) {
        return;
    }

    nm_allocator_lock(allocator);
    // Each list was atomically detached above, so the owner can splice nodes
    // into normal local free lists while holding only its allocator lock.
    while (task_head != NULL) {
        nm_task_t *next = task_head->alloc_next;
        task_head->alloc_next = allocator->task_free;
        allocator->task_free = task_head;
        allocator->task_remote_drains += 1U;
        task_burst += 1U;
        task_head = next;
    }
    while (wait_head != NULL) {
        nm_wait_node_t *next = wait_head->alloc_next;
        wait_head->alloc_next = allocator->wait_free;
        allocator->wait_free = wait_head;
        allocator->wait_remote_drains += 1U;
        wait_burst += 1U;
        wait_head = next;
    }
    while (timer_head != NULL) {
        nm_timer_node_t *next = timer_head->alloc_next;
        timer_head->alloc_next = allocator->timer_free;
        allocator->timer_free = timer_head;
        allocator->timer_remote_drains += 1U;
        timer_burst += 1U;
        timer_head = next;
    }
    while (io_head != NULL) {
        nm_io_req_t *next = io_head->alloc_next;
        io_head->alloc_next = allocator->io_req_free;
        allocator->io_req_free = io_head;
        allocator->io_req_remote_drains += 1U;
        io_burst += 1U;
        io_head = next;
    }
    while (io_buffer_head != NULL) {
        nm_io_buffer_t *next = io_buffer_head->alloc_next;
        io_buffer_head->alloc_next = allocator->io_buffer_free;
        allocator->io_buffer_free = io_buffer_head;
        allocator->io_buffer_remote_drains += 1U;
        io_buffer_burst += 1U;
        io_buffer_head = next;
    }
    allocator->task_remote_frees += task_burst;
    allocator->wait_remote_frees += wait_burst;
    allocator->timer_remote_frees += timer_burst;
    allocator->io_req_remote_frees += io_burst;
    allocator->io_buffer_remote_frees += io_buffer_burst;
    if (task_burst > allocator->task_remote_burst_max) {
        allocator->task_remote_burst_max = task_burst;
    }
    if (wait_burst > allocator->wait_remote_burst_max) {
        allocator->wait_remote_burst_max = wait_burst;
    }
    if (timer_burst > allocator->timer_remote_burst_max) {
        allocator->timer_remote_burst_max = timer_burst;
    }
    if (io_burst > allocator->io_req_remote_burst_max) {
        allocator->io_req_remote_burst_max = io_burst;
    }
    if (io_buffer_burst > allocator->io_buffer_remote_burst_max) {
        allocator->io_buffer_remote_burst_max = io_buffer_burst;
    }
    nm_allocator_unlock(allocator);
}
