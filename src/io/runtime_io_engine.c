/**
 * @file src/io/runtime_io_engine.c
 * @brief Shared I/O engine setup and teardown across platform backends.
 *
 * @details
 * Linux nodes wrap io_uring rings and optional features such as SQPOLL,
 * completion eventfd registration, provided receive buffers, and opcode
 * probing. Non-Linux nodes expose the same runtime-facing capability surface
 * using kqueue/Mach wake support where available.
 *
 * The rest of the runtime talks to nodes through capability flags and submit
 * helpers, so unsupported backend features degrade to direct or blocking
 * fallback paths instead of changing public API behavior.
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

#if defined(__linux__)

/**
 * @brief Pick the default SQPOLL CPU for an I/O node.
 *
 * The preferred CPU is the first shard CPU mapped to @p node_index. If no shard
 * maps to that node, the first allowed CPU is used as a conservative fallback.
 *
 * @param rt         Runtime being initialized.
 * @param node_index I/O node index.
 *
 * @return CPU id suitable for io_uring SQPOLL affinity.
 */
static unsigned nm_node_default_sqpoll_cpu(nm_runtime_t *rt, unsigned node_index) {
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        if (rt->shards[i].io_node_index == node_index) {
            return rt->shards[i].cpu_id;
        }
    }
    return rt->allowed_cpus[0];
}

/**
 * @brief Clear SQPOLL capability state on a node.
 *
 * @param node Node to update; may be @c NULL.
 */
static void nm_node_disable_sqpoll(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->sqpoll_enabled = false;
    node->sqpoll_cpu = UINT_MAX;
}

/**
 * @brief Clear completion eventfd registration state on a node.
 *
 * @param node Node to update; may be @c NULL.
 */
static void nm_node_disable_cq_eventfd(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->cq_eventfd_registered = false;
}

/**
 * @brief Clear provided-buffer receive-ring capability state.
 *
 * @param node Node to update; may be @c NULL.
 */
static void nm_node_disable_recv_buf_ring(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->supports_provided_buffers = false;
    node->recv_buf_entries = 0U;
    node->recv_buf_mask = 0U;
    node->recv_buf_group = -1;
}

/**
 * @brief Initialize a Linux io_uring ring for an I/O node.
 *
 * SQPOLL is attempted first when requested and compatible with the runtime
 * topology. Unsupported SQPOLL setup errors fall back to a normal ring, while
 * hard setup failures propagate through @c errno.
 *
 * @param rt   Runtime owning the node.
 * @param node Node to initialize.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int nm_node_init_ring(nm_runtime_t *rt, nm_node_t *node) {
    struct io_uring_params params;
    int rc;

    if (rt == NULL || node == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&params, 0, sizeof(params));
    if (rt->experimental_sqpoll_requested != 0U && rt->experimental_shard_rings == 0U) {
        unsigned sqpoll_cpu = rt->sqpoll_cpu >= 0 ? (unsigned)rt->sqpoll_cpu : nm_node_default_sqpoll_cpu(rt, node->index);

        params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
        params.sq_thread_cpu = sqpoll_cpu;
        params.sq_thread_idle = 2000U;
        rc = io_uring_queue_init_params(NM_IO_RING_DEPTH, &node->ring, &params);
        if (rc == 0) {
            if (io_uring_register_eventfd(&node->ring, node->event_fd) == 0) {
                node->cq_eventfd_registered = true;
            } else {
                nm_node_disable_cq_eventfd(node);
            }
            node->ring_ready = true;
            node->sqpoll_enabled = true;
            node->sqpoll_cpu = sqpoll_cpu;
            rt->experimental_sqpoll_active = 1U;
            return 0;
        }
        nm_node_disable_sqpoll(node);
        if (!nm_io_sqpoll_setup_error(-rc)) {
            errno = -rc;
            return -1;
        }
    }

    rc = io_uring_queue_init(NM_IO_RING_DEPTH, &node->ring, 0);
    if (rc == 0) {
        nm_node_disable_cq_eventfd(node);
        node->ring_ready = true;
        return 0;
    }
    errno = -rc;
    return -1;
}

/**
 * @brief Check whether an I/O node supports a request kind.
 *
 * @param node Node whose capability flags are inspected.
 * @param kind Runtime I/O request kind.
 *
 * @return @c true when the backend can submit @p kind.
 */
bool nm_node_supports_kind(const nm_node_t *node, nm_io_kind_t kind) {
    switch (kind) {
    case NM_IO_KIND_READ:
        return node->supports_read;
    case NM_IO_KIND_WRITE:
        return node->supports_write;
    case NM_IO_KIND_ACCEPT:
        return node->supports_accept;
    case NM_IO_KIND_POLL:
        return node->supports_poll;
    default:
        return false;
    }
}

/**
 * @brief Set up an io_uring provided-buffer ring for small receives.
 *
 * Provided buffers let recv completions hand ownership of a pre-registered
 * buffer id back to the runtime. The feature is optional; setup failure disables
 * provided buffers and lets callers use regular buffers instead.
 *
 * @param node Node whose ring should receive the buffer group.
 *
 * @return 0 when the feature is ready or intentionally disabled.
 * @return -1 when allocation or kernel setup failed.
 */
int nm_node_setup_recv_buf_ring(nm_node_t *node) {
    int err = 0;
    unsigned i;

    if (node == NULL || !node->ring_ready || !node->supports_recv) {
        nm_node_disable_recv_buf_ring(node);
        return 0;
    }

    node->recv_buf_group = (int)(node->index + 1U);
    node->recv_buf_entries = NM_IO_RECV_BUF_RING_ENTRIES;
    node->recv_buf_mask = (unsigned)io_uring_buf_ring_mask(node->recv_buf_entries);
    node->recv_buf_ring = io_uring_setup_buf_ring(&node->ring,
                                                  node->recv_buf_entries,
                                                  node->recv_buf_group,
                                                  0U,
                                                  &err);
    if (node->recv_buf_ring == NULL) {
        nm_node_disable_recv_buf_ring(node);
        return -1;
    }

    node->recv_buf_storage = calloc(node->recv_buf_entries, NM_IO_BUFFER_INLINE_BYTES);
    if (node->recv_buf_storage == NULL) {
        (void)io_uring_free_buf_ring(&node->ring, node->recv_buf_ring, node->recv_buf_entries, node->recv_buf_group);
        node->recv_buf_ring = NULL;
        nm_node_disable_recv_buf_ring(node);
        errno = ENOMEM;
        return -1;
    }

    io_uring_buf_ring_init(node->recv_buf_ring);
    for (i = 0; i < node->recv_buf_entries; ++i) {
        io_uring_buf_ring_add(node->recv_buf_ring,
                              node->recv_buf_storage + (i * NM_IO_BUFFER_INLINE_BYTES),
                              NM_IO_BUFFER_INLINE_BYTES,
                              (unsigned short)i,
                              (int)node->recv_buf_mask,
                              (int)i);
    }
    io_uring_buf_ring_advance(node->recv_buf_ring, (int)node->recv_buf_entries);
    node->supports_provided_buffers = true;
    return 0;
}

/**
 * @brief Return a provided receive buffer to an io_uring buffer ring.
 *
 * @param node Node owning the buffer group.
 * @param bid  Buffer id returned by a receive completion.
 *
 * @return 0 on success, or -1 with @c errno set for invalid input.
 */
int nm_node_recycle_recv_buffer(nm_node_t *node, unsigned short bid) {
    if (node == NULL || !node->ring_ready || !node->supports_provided_buffers || node->recv_buf_ring == NULL ||
        node->recv_buf_storage == NULL || bid >= node->recv_buf_entries) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&node->recv_buf_lock);
    io_uring_buf_ring_add(node->recv_buf_ring,
                          node->recv_buf_storage + ((size_t)bid * NM_IO_BUFFER_INLINE_BYTES),
                          NM_IO_BUFFER_INLINE_BYTES,
                          bid,
                          (int)node->recv_buf_mask,
                          0);
    io_uring_buf_ring_advance(node->recv_buf_ring, 1);
    node->provided_buf_returns += 1U;
    pthread_mutex_unlock(&node->recv_buf_lock);
    return 0;
}

/**
 * @brief Destroy an io_uring provided-buffer ring and its backing storage.
 *
 * @param node Node to clean up; may be @c NULL.
 */
void nm_node_destroy_recv_buf_ring(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    if (node->recv_buf_ring != NULL) {
        (void)io_uring_free_buf_ring(&node->ring, node->recv_buf_ring, node->recv_buf_entries, node->recv_buf_group);
        node->recv_buf_ring = NULL;
    }
    free(node->recv_buf_storage);
    node->recv_buf_storage = NULL;
    nm_node_disable_recv_buf_ring(node);
}

/**
 * @brief Unregister a completion eventfd from a Linux io_uring node.
 *
 * @param node Node to update; may be @c NULL.
 */
void nm_node_unregister_cq_eventfd(nm_node_t *node) {
    if (node == NULL || !node->ring_ready || !node->cq_eventfd_registered) {
        return;
    }
    (void)io_uring_unregister_eventfd(&node->ring);
    node->cq_eventfd_registered = false;
}

/**
 * @brief Probe io_uring opcode support and cache node capability flags.
 *
 * When probing is unavailable, the runtime assumes support and lets submission
 * failures drive fallback decisions. This preserves compatibility with older
 * liburing/kernel combinations that cannot produce a probe object.
 *
 * @param node Node whose ring should be probed.
 */
void nm_probe_ring_support(nm_node_t *node) {
    struct io_uring_probe *probe = io_uring_get_probe_ring(&node->ring);

    if (probe == NULL) {
        node->supports_read = true;
        node->supports_recv = true;
        node->supports_write = true;
        node->supports_accept = true;
        node->supports_poll = true;
        node->supports_multishot_recv = true;
        node->supports_multishot_accept = true;
        node->supports_multishot_poll = true;
        return;
    }

    node->supports_read = io_uring_opcode_supported(probe, IORING_OP_READ) != 0;
    node->supports_recv = io_uring_opcode_supported(probe, IORING_OP_RECV) != 0;
    node->supports_write = io_uring_opcode_supported(probe, IORING_OP_WRITE) != 0;
    node->supports_accept = io_uring_opcode_supported(probe, IORING_OP_ACCEPT) != 0;
    node->supports_poll = io_uring_opcode_supported(probe, IORING_OP_POLL_ADD) != 0;
    node->supports_multishot_recv = node->supports_recv;
    node->supports_multishot_accept = node->supports_accept;
    node->supports_multishot_poll = node->supports_poll;
    io_uring_free_probe(probe);
}

#else

#include <mach/mach.h>
#include <sys/event.h>

/**
 * @brief Release Mach wake resources used by a Darwin I/O node.
 *
 * @param port Receive/send right port to deallocate.
 * @param pset Port set used by kqueue Mach-port wake integration.
 */
static void nm_node_destroy_mach_wake(mach_port_t port, mach_port_t pset) {
    if (port != MACH_PORT_NULL) {
        (void)mach_port_deallocate(mach_task_self(), port);
        (void)mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    }
    if (pset != MACH_PORT_NULL) {
        (void)mach_port_mod_refs(mach_task_self(), pset, MACH_PORT_RIGHT_PORT_SET, -1);
    }
}

/**
 * @brief Initialize Darwin Mach-port wake integration for a node.
 *
 * The Mach port is placed into a port set and registered with the node's kqueue
 * descriptor. Wake setup is optional for runtime functionality; failures are
 * returned to the caller so initialization can decide whether to ignore them.
 *
 * @param node Node whose event descriptor should receive Mach wake events.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
static int nm_node_init_mach_wake(nm_node_t *node) {
    mach_port_t port = MACH_PORT_NULL;
    mach_port_t pset = MACH_PORT_NULL;
    struct kevent change;
    kern_return_t kr;

    if (node == NULL || node->event_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr != KERN_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        nm_node_destroy_mach_wake(port, MACH_PORT_NULL);
        errno = EINVAL;
        return -1;
    }
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &pset);
    if (kr != KERN_SUCCESS) {
        nm_node_destroy_mach_wake(port, MACH_PORT_NULL);
        errno = EINVAL;
        return -1;
    }
    kr = mach_port_move_member(mach_task_self(), port, pset);
    if (kr != KERN_SUCCESS) {
        nm_node_destroy_mach_wake(port, pset);
        errno = EINVAL;
        return -1;
    }

    EV_SET(&change, (uintptr_t)pset, EVFILT_MACHPORT, EV_ADD | EV_CLEAR, 0U, 0, NULL);
    if (kevent(node->event_fd, &change, 1, NULL, 0, NULL) != 0) {
        int saved_errno = errno;

        nm_node_destroy_mach_wake(port, pset);
        errno = saved_errno;
        return -1;
    }

    node->mach_wake_port = (uint32_t)port;
    node->mach_wake_pset = (uint32_t)pset;
    node->mach_wake_enabled = true;
    return 0;
}

/**
 * @brief Initialize the non-Linux I/O node capability surface.
 *
 * Darwin does not use io_uring, but the runtime still exposes a "ready" node
 * with poll/accept/read/write capability flags so higher layers can use the
 * same API and dispatch through kqueue-backed watcher code.
 *
 * @param rt   Runtime owning the node; currently unused on this backend.
 * @param node Node to initialize.
 *
 * @return 0 on success, or -1 with @c errno set for invalid input.
 */
int nm_node_init_ring(nm_runtime_t *rt, nm_node_t *node) {
    (void)rt;
    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    node->ring_ready = true;
    node->supports_read = true;
    node->supports_recv = true;
    node->supports_write = true;
    node->supports_accept = true;
    node->supports_poll = true;
    node->supports_multishot_recv = true;
    node->supports_multishot_accept = true;
    node->supports_multishot_poll = true;
    node->supports_provided_buffers = false;
    node->cq_eventfd_registered = false;
    node->mach_wake_enabled = false;
    node->mach_wake_port = 0U;
    node->mach_wake_pset = 0U;
    (void)nm_node_init_mach_wake(node);
    return 0;
}

/**
 * @brief Check whether a non-Linux node supports an I/O request kind.
 *
 * @param node Node whose capability flags are inspected.
 * @param kind Runtime I/O request kind.
 *
 * @return @c true when @p kind is supported.
 */
bool nm_node_supports_kind(const nm_node_t *node, nm_io_kind_t kind) {
    if (node == NULL) {
        return false;
    }
    switch (kind) {
    case NM_IO_KIND_READ:
        return node->supports_read;
    case NM_IO_KIND_WRITE:
        return node->supports_write;
    case NM_IO_KIND_ACCEPT:
        return node->supports_accept;
    case NM_IO_KIND_POLL:
        return node->supports_poll;
    default:
        return false;
    }
}

/**
 * @brief Disable provided-buffer support on non-Linux backends.
 *
 * @param node Node to update.
 *
 * @return Always 0.
 */
int nm_node_setup_recv_buf_ring(nm_node_t *node) {
    if (node != NULL) {
        node->supports_provided_buffers = false;
        node->recv_buf_entries = 0U;
        node->recv_buf_mask = 0U;
        node->recv_buf_group = -1;
    }
    return 0;
}

/**
 * @brief Reject provided-buffer recycling on non-Linux backends.
 *
 * @param node Unused node pointer.
 * @param bid  Unused buffer id.
 *
 * @return Always -1 with @c errno set to @c ENOSYS.
 */
int nm_node_recycle_recv_buffer(nm_node_t *node, unsigned short bid) {
    (void)node;
    (void)bid;
    errno = ENOSYS;
    return -1;
}

/**
 * @brief Clear non-Linux provided-buffer state.
 *
 * @param node Node to clean up; may be @c NULL.
 */
void nm_node_destroy_recv_buf_ring(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    free(node->recv_buf_storage);
    node->recv_buf_storage = NULL;
    node->recv_buf_ring = NULL;
    node->supports_provided_buffers = false;
}

/**
 * @brief Tear down non-Linux wake registration state.
 *
 * @param node Node to update; may be @c NULL.
 */
void nm_node_unregister_cq_eventfd(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->cq_eventfd_registered = false;
    if (node->mach_wake_enabled) {
        nm_node_destroy_mach_wake((mach_port_t)node->mach_wake_port, (mach_port_t)node->mach_wake_pset);
        node->mach_wake_enabled = false;
        node->mach_wake_port = 0U;
        node->mach_wake_pset = 0U;
    }
}

/**
 * @brief Publish optimistic non-Linux capability flags.
 *
 * The backend uses watcher/fallback paths rather than io_uring opcode probing,
 * so all public operation kinds remain advertised as supported.
 *
 * @param node Node to update; may be @c NULL.
 */
void nm_probe_ring_support(nm_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->supports_read = true;
    node->supports_recv = true;
    node->supports_write = true;
    node->supports_accept = true;
    node->supports_poll = true;
    node->supports_multishot_recv = true;
    node->supports_multishot_accept = true;
    node->supports_multishot_poll = true;
}

#endif
