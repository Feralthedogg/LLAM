/**
 * @file src/io/linux/runtime_io_watch_linux_worker.c
 * @brief Linux I/O worker loop and ring polling lifecycle.
 *
 * @details
 * Each I/O node owns a worker thread that submits queued operations, drains
 * completions, honors runtime shutdown by deactivating watches, and chooses an
 * idle wait strategy based on pending operation count and SQPOLL/eventfd state.
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

#include "runtime_io_watch_linux_internal.h"

/**
 * @brief Drain a bounded batch of ready io_uring completions.
 *
 * Completion queue depth metrics are updated before dispatch. Individual CQEs
 * are handed to ::llam_io_handle_cqe, which owns request/watch completion logic.
 *
 * @param node Node whose completion queue should be drained.
 */
void llam_io_drain_completions(llam_node_t *node) {
    struct io_uring_cqe *cqes[32];
    unsigned cq_depth;
    unsigned count;
    unsigned i;

    cq_depth = io_uring_cq_ready(&node->ring);
    node->last_cq_depth = cq_depth;
    if (cq_depth > node->max_cq_depth) {
        node->max_cq_depth = cq_depth;
    }
    count = io_uring_peek_batch_cqe(&node->ring, cqes, 32U);
    for (i = 0; i < count; ++i) {
        llam_io_handle_cqe(node, cqes[i]);
    }
}

/**
 * @brief Main loop for a Linux I/O node worker.
 *
 * The loop repeatedly submits pending work, drains completions, and waits using
 * the cheapest available mechanism. SQPOLL nodes with a registered eventfd spin
 * briefly for CQEs before sleeping on the eventfd; other nodes use timed
 * @c io_uring_wait_cqe_timeout calls while work is pending.
 *
 * @param arg Pointer to an ::llam_node_t.
 *
 * @return Always @c NULL.
 */
void *llam_io_worker_main(void *arg) {
    llam_node_t *node = arg;
    llam_runtime_t *rt = node->runtime;

    llam_node_lower_worker_priority(node);
    llam_tune_io_worker_thread(node);

    for (;;) {
        unsigned pending = atomic_load(&node->pending_ops);

        if (atomic_load(&rt->stop_requested)) {
            llam_io_queue_shutdown_controls(node);
        }
        llam_io_submit_batch(node);
        llam_io_drain_completions(node);

        if (atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
            pending == 0U &&
            atomic_load(&node->pending_ops) == 0U) {
            break;
        }

        pending = atomic_load(&node->pending_ops);
        if (pending == 0U) {
            (void)llam_node_wait_eventfd(node, LLAM_IDLE_POLL_TIMEOUT_MS);
            continue;
        }

        if (node->sqpoll_enabled && node->cq_eventfd_registered) {
            if (llam_node_spin_for_cqe(node)) {
                continue;
            }
            (void)llam_node_wait_eventfd(node, -1);
            continue;
        }

        {
            struct __kernel_timespec ts;
            struct io_uring_cqe *cqe;
            int rc;

            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            rc = io_uring_wait_cqe_timeout(&node->ring, &cqe, &ts);
            if (rc == 0 && cqe != NULL) {
                llam_io_handle_cqe(node, cqe);
            } else if (rc != -ETIME && rc < 0) {
                llam_record_fatal(rt, -rc);
            }
        }
    }

    return NULL;
}
