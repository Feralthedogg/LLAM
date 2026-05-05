/**
 * @file src/io/runtime_io_watch_linux_prelude.c
 * @brief Linux/io_uring backend compatibility helpers and setup prelude.
 *
 * @details
 * These helpers are shared by Linux watch backend files before the main worker
 * and submission paths. They cover liburing compatibility shims, fd-to-node
 * hashing for multishot ownership, SQPOLL syscall decisions, eventfd waiting,
 * short CQE spinning, and in-flight waiter accounting.
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
 * @brief Set the buffer group field on an SQE.
 *
 * @param sqe       Submission queue entry.
 * @param buf_group Provided-buffer group id.
 */
void nm_io_uring_sqe_set_buf_group_compat(struct io_uring_sqe *sqe, int buf_group) {
    sqe->buf_group = (unsigned short)buf_group;
}

/**
 * @brief Mix a 64-bit watch identity component.
 *
 * @param value Input value.
 * @return Mixed hash value.
 */
uint64_t nm_hash_watch_identity_u64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

/**
 * @brief Pick the owner node for multishot watch state.
 *
 * @param rt                  Runtime owning nodes.
 * @param fallback_node_index Default node index.
 * @param fd                  File descriptor used for identity hashing.
 * @return Node index that should own the watch.
 */
unsigned nm_multishot_owner_node_index(nm_runtime_t *rt, unsigned fallback_node_index, int fd) {
    struct stat st;
    uint64_t hash;

    if (rt == NULL) {
        return 0U;
    }
    if (fallback_node_index >= rt->active_nodes) {
        fallback_node_index = rt->active_nodes > 0U ? 0U : fallback_node_index;
    }
    if (rt->experimental_shard_rings == 0U ||
        rt->experimental_shard_rings_multishot == 0U ||
        rt->active_nodes <= 1U ||
        fd < 0) {
        return fallback_node_index;
    }
    if (fstat(fd, &st) != 0) {
        return fallback_node_index;
    }

    hash = nm_hash_watch_identity_u64((uint64_t)st.st_dev);
    // Include fd as a final tie-breaker. Device/inode gives stable ownership
    // across duplicate descriptors; fd reduces clustering for anonymous handles.
    hash ^= nm_hash_watch_identity_u64((uint64_t)st.st_ino);
    hash ^= nm_hash_watch_identity_u64((uint64_t)(unsigned)fd);
    return (unsigned)(hash % rt->active_nodes);
}

/**
 * @brief Demote shard-owned I/O worker priority when many rings are active.
 *
 * @param node Node whose worker is starting.
 */
void nm_node_lower_worker_priority(const nm_node_t *node) {
    if (node == NULL || node->runtime == NULL || node->runtime->experimental_shard_rings == 0U) {
        return;
    }
#ifdef __linux__
    // Shard-owned rings add one I/O worker per shard, so demote them slightly to
    // reduce CPU contention with scheduler workers.
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
}

/**
 * @brief Decide whether submitting the ring needs an explicit syscall.
 *
 * @param node Node whose ring is being submitted.
 * @return true if @c io_uring_submit should wake the kernel.
 */
bool nm_node_submit_needs_syscall(nm_node_t *node) {
    // With SQPOLL active we only need a syscall when the kernel poller has gone
    // idle and asks for wakeup.
    if (!node->sqpoll_enabled) {
        return true;
    }
    if (node->ring.sq.kflags == NULL) {
        return true;
    }
    return ((*node->ring.sq.kflags & IORING_SQ_NEED_WAKEUP) != 0U);
}

/**
 * @brief Submit pending SQEs and update submit metrics.
 *
 * @param node Node whose ring should be submitted.
 * @return liburing submit result.
 */
int nm_node_submit_ring(nm_node_t *node) {
    int rc;

    node->submit_calls += 1U;
    if (nm_node_submit_needs_syscall(node)) {
        node->submit_syscalls += 1U;
    }
    rc = io_uring_submit(&node->ring);
    return rc;
}

/**
 * @brief Wait for and drain a node eventfd wake.
 *
 * @param node       Node whose wake fd should be polled.
 * @param timeout_ms Poll timeout in milliseconds; -1 blocks indefinitely.
 * @return true if a wake was observed.
 */
bool nm_node_wait_eventfd(nm_node_t *node, int timeout_ms) {
    struct pollfd pfd;
    int rc;

    if (node == NULL || node->event_fd < 0) {
        return false;
    }

    pfd.fd = node->event_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (rc > 0 && (pfd.revents & POLLIN) != 0) {
        nm_drain_node_wake(node);
        return true;
    }
    return false;
}

/**
 * @brief Spin briefly for CQEs before sleeping in SQPOLL mode.
 *
 * This only catches completions that are effectively "in flight right now" so
 * the SQPOLL path does not pay an avoidable sleep/wakeup penalty.
 *
 * @param node Node whose ring should be checked.
 * @return true if at least one completion was drained.
 */
bool nm_node_spin_for_cqe(nm_node_t *node) {
    uint64_t start_ns;
    unsigned iters;

    if (node == NULL || !node->sqpoll_enabled || atomic_load(&node->pending_ops) == 0U) {
        return false;
    }

    start_ns = nm_now_ns();
    for (iters = 0; iters < NM_IO_SQPOLL_SPIN_ITERS; ++iters) {
        if (io_uring_cq_ready(&node->ring) > 0U) {
            nm_io_drain_completions(node);
            return true;
        }
        if (atomic_load(&node->pending_ops) == 0U || nm_now_ns() - start_ns >= NM_IO_SQPOLL_SPIN_NS) {
            break;
        }
        nm_pause_cpu();
    }
    return false;
}

/**
 * @brief Update per-shard in-flight I/O waiter pressure accounting.
 *
 * @param owner_shard Shard id to update.
 * @param delta       Positive to increment, negative to decrement.
 */
void nm_shard_note_inflight_io_waiter(unsigned owner_shard, int delta) {
    nm_runtime_t *rt = &g_nm_runtime;

    if (delta == 0 || owner_shard >= rt->active_shards) {
        return;
    }
    if (delta > 0) {
        atomic_fetch_add_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)delta, memory_order_acq_rel);
    } else {
        atomic_fetch_sub_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)(-delta), memory_order_acq_rel);
    }
}

/**
 * @brief Transfer in-flight request ownership between shards.
 *
 * @param req        Request whose owner counter should move.
 * @param from_shard Expected current owner shard id.
 * @param to_shard   New owner shard id.
 * @return true if ownership moved.
 */
bool nm_io_req_transfer_inflight_owner(nm_io_req_t *req, unsigned from_shard, unsigned to_shard) {
    unsigned expected;

    if (req == NULL || from_shard == to_shard || from_shard >= g_nm_runtime.active_shards || to_shard >= g_nm_runtime.active_shards) {
        return false;
    }

    expected = from_shard;
    if (!atomic_compare_exchange_strong_explicit(&req->inflight_owner_shard,
                                                 &expected,
                                                 to_shard,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }

    nm_shard_note_inflight_io_waiter(from_shard, -1);
    nm_shard_note_inflight_io_waiter(to_shard, 1);
    return true;
}
