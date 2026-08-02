/**
 * @file src/io/api/issue.c
 * @brief One-shot and Linux native I/O submission transitions.
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

#include "io/runtime_io_api_internal.h"

#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
#include "io/linux/runtime_io_segment_linux_internal.h"
#endif

/**
 * @brief Issue a one-shot I/O request through the current shard's I/O node.
 *
 * This is the fallback/general path for operations that cannot use an
 * indefinite shared watch.  The request is queued under node->submit_lock,
 * counted as pending backend work, and then the owning task parks until the
 * backend completion path wakes it.
 *
 * @param req I/O request to submit.
 * @param has_deadline true when @p deadline_ns should be armed.
 * @param deadline_ns Absolute deadline in runtime monotonic nanoseconds.
 *
 * @return 0 on completion, -1 when the backend cannot accept the request or
 *         completion reports an error.
 */
int llam_issue_io(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns) {
    llam_runtime_t *rt;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_node_t *node;
    bool kind_supported;

    if (task == NULL || shard == NULL) {
        return 0;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL || shard->runtime != rt) {
        errno = EXDEV;
        return -1;
    }

    node = &rt->nodes[shard->io_node_index];
    kind_supported =
        req->kind == LLAM_IO_KIND_READ && req->use_recv_op
            ? node->supports_recv
            : req->kind == LLAM_IO_KIND_WRITE && req->use_send_op
                ? node->supports_send
            : llam_node_supports_kind(node, req->kind);
#if LLAM_RUNTIME_BACKEND_KQUEUE
#if LLAM_BUILD_RESEARCH
    /*
     * Generic public read/write/connect keeps using its conservative blocking
     * fallback on kqueue. A trusted internal completion sink, however, can use
     * the existing one-shot request backend so it can retain ownership across
     * a composed runtime-effect region.
     */
    if (!kind_supported && req->completion_sink != NULL &&
        (req->kind == LLAM_IO_KIND_READ ||
         req->kind == LLAM_IO_KIND_WRITE ||
         req->kind == LLAM_IO_KIND_CONNECT)) {
        kind_supported = true;
    }
#endif
#endif
    /*
     * Do not inspect a socket through the caller's descriptor before queuing a
     * poll. Windows submissions are generation-bound to a retained duplicate,
     * and ConnectEx/SO_UPDATE_CONNECT_CONTEXT is performed on that association
     * authority. The raw descriptor is still the lookup key, but it is not the
     * authoritative handle for post-connect capability state.
     *
     * llam_windows_submit_poll() validates the event mask, socket family, and
     * type after the association has been revalidated and pinned. Unsupported
     * requests complete with a capability error and retain the normal public
     * blocking-fallback contract.
     */
    if (!node->ring_ready || !kind_supported) {
        atomic_fetch_add_explicit(&node->unsupported_ops, 1U, memory_order_relaxed);
        shard->metrics.io_fallbacks += 1U;
        errno = EAGAIN;
        return -1;
    }

    req->task = task;
    req->result = -1;
    req->error_code = 0;
    atomic_store_explicit(&req->owner_shard,
                          shard->id,
                          memory_order_release);
    atomic_store_explicit(&req->attached_node_index,
                          node->index,
                          memory_order_release);
    req->submit_ts_ns = llam_now_ns();
    atomic_store(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    if (llam_prepare_io_wait(req, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE, has_deadline ? deadline_ns : 0U) != 0) {
        return -1;
    }

    if (!llam_node_submit_io_req(node, req)) {
        int saved_errno = errno;

        llam_cleanup_io_wait_setup(task, req);
        return llam_fail_io_setup_req(req, saved_errno);
    }
    if (llam_park_io_req(req, has_deadline, deadline_ns, node) != 0) {
        return -1;
    }
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
/**
 * @brief Submit one bounded native batch and park its task exactly once.
 */
int llam_issue_linux_native_batch(
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_runtime_t *rt;
    llam_node_t *node;
    unsigned request_refs;
    unsigned i;
    int error = EINVAL;

    if (batch == NULL || req == NULL ||
        shard == NULL || task == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    rt = task->owner_runtime;
    if (rt == NULL ||
        shard->runtime != rt ||
        batch->owner_runtime != rt ||
        req->owner_runtime != rt) {
        return llam_fail_io_setup_req(req, EXDEV);
    }
    if (shard->id >= rt->active_shards ||
        shard->io_node_index >= rt->active_nodes ||
        rt->nodes == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    node = &rt->nodes[shard->io_node_index];
    request_refs = atomic_load_explicit(
        &req->lifetime_refs, memory_order_acquire);
    if (request_refs == 0U || request_refs == UINT_MAX ||
        atomic_load_explicit(
            &req->wait_mode,
            memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE ||
        atomic_load_explicit(
            &batch->state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_IDLE ||
        batch->segment_count == 0U ||
        batch->segment_count >
            LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS) {
        return llam_fail_io_setup_req(req, EBUSY);
    }
    if (atomic_load_explicit(
            &rt->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(
            &rt->shutdown_requested, memory_order_acquire)) {
        return llam_fail_io_setup_req(req, ESHUTDOWN);
    }
    if (!node->ring_ready || node->linux_submit_terminal) {
        return llam_fail_io_setup_req(req, EAGAIN);
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];
        unsigned operation_index;

        if (segment == NULL ||
            segment->owner_runtime != rt ||
            segment->generation == 0U ||
            segment->op_count == 0U ||
            segment->op_count >
                LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
            atomic_load_explicit(
                &segment->state,
                memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
            return llam_fail_io_setup_req(req, EINVAL);
        }
        if (segment->mode ==
                LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP &&
            (node->linux_ring_features &
             IORING_FEAT_CQE_SKIP) == 0U) {
            return llam_fail_io_setup_req(req, ENOTSUP);
        }
        for (operation_index = 0U;
             operation_index < segment->op_count;
             operation_index += 1U) {
            if (segment->ops[operation_index].kind ==
                LLAM_LINUX_NATIVE_OP_RECV) {
                if (node->supports_recv) {
                    continue;
                }
                error = EAGAIN;
                break;
            }
            if (segment->ops[operation_index].kind ==
                LLAM_LINUX_NATIVE_OP_SEND) {
                if (node->supports_send) {
                    continue;
                }
                error = EAGAIN;
                break;
            }
            if (segment->ops[operation_index].kind ==
                LLAM_LINUX_NATIVE_OP_CONNECT) {
                if (node->supports_connect) {
                    continue;
                }
                error = EAGAIN;
                break;
            }
            error = EINVAL;
            break;
        }
        if (operation_index != segment->op_count) {
            return llam_fail_io_setup_req(req, error);
        }
    }

    if (llam_prepare_io_wait(
            req,
            LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
            0U) != 0) {
        return -1;
    }
    if (!llam_linux_native_batch_enqueue(
            node, batch, req)) {
        int saved_errno = errno != 0 ? errno : EIO;

        llam_cleanup_io_wait_setup(task, req);
        return llam_fail_io_setup_req(req, saved_errno);
    }
    batch->segments[0]->task_parks += 1U;
    return llam_park_io_req(req, false, 0U, node);
}

/**
 * @brief Preserve the width-one native API through a stack-owned batch ticket.
 */
int llam_issue_linux_native_segment(
    llam_linux_native_segment_t *segment,
    llam_io_req_t *req) {
    llam_linux_native_batch_t batch;

    if (segment == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    memset(&batch, 0, sizeof(batch));
    batch.owner_runtime = segment->owner_runtime;
    batch.segments[0] = segment;
    batch.segment_count = 1U;
    atomic_init(&batch.state, LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&batch.terminal_claimed, 0U);
    atomic_init(
        &batch.cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&batch.cancel_requested, 0U);
    return llam_issue_linux_native_batch(&batch, req);
}
#endif
