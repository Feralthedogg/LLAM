/**
 * @file src/io/linux/watch/linux_worker.c
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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "io/linux/runtime_io_watch_linux_internal.h"

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
    atomic_store_explicit(&node->last_cq_depth, cq_depth, memory_order_relaxed);
    llam_atomic_update_peak(&node->max_cq_depth, cq_depth);
    count = io_uring_peek_batch_cqe(&node->ring, cqes, 32U);
    for (i = 0; i < count; ++i) {
        llam_io_handle_cqe(node, cqes[i]);
    }
}

/** @brief Return whether a timed CQ wait failure should poison the runtime. */
bool llam_linux_wait_cqe_error_is_fatal(int err) {
    return err != ETIME && err != EINTR;
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
            (node->linux_submit_terminal ||
             (pending == 0U && atomic_load(&node->pending_ops) == 0U))) {
            break;
        }

        pending = atomic_load(&node->pending_ops);
        if (pending == 0U) {
            (void)llam_node_wait_eventfd(node, LLAM_IDLE_POLL_TIMEOUT_MS);
            continue;
        }

        if (node->linux_submit_terminal || node->linux_submit_retry ||
            io_uring_sq_ready(&node->ring) > 0U) {
            /*
             * A prior short/error submit left SQEs ring-visible, or a terminal
             * submit failure is waiting for explicit shutdown teardown. Avoid
             * the indefinite SQPOLL sleep and keep completion/stop checks
             * bounded.
             */
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
            } else if (rc < 0 && llam_linux_wait_cqe_error_is_fatal(-rc)) {
                llam_record_fatal(rt, -rc);
            }
        }
    }

    return NULL;
}
