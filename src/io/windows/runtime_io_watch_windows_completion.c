/**
 * @file src/io/windows/runtime_io_watch_windows_completion.c
 * @brief Windows IOCP completion handling and request wakeup path.
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

#include "runtime_io_watch_windows_internal.h"

void llam_windows_complete_req(llam_node_t *node, llam_io_req_t *req, int res, bool decrement_pending) {
    llam_io_abort_reason_t abort_reason;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned inflight_owner = UINT_MAX;
    unsigned completion_owner = UINT_MAX;
    unsigned wait_mode;

    if (decrement_pending) {
        atomic_fetch_sub(&node->pending_ops, 1U);
    }
    wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
    if (wait_mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
        /*
         * The exchanged owner is authoritative for completion routing.  A
         * concurrent dynamic rehome may still be rewriting req->owner_shard
         * after it successfully transferred this atomic in-flight owner.
         */
        inflight_owner = atomic_exchange_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_acq_rel);
        if (inflight_owner < node->runtime->active_shards) {
            llam_shard_note_inflight_io_waiter(req->owner_runtime, inflight_owner, -1);
            completion_owner = inflight_owner;
        }
    } else {
        atomic_store_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_release);
        completion_owner = req->owner_shard;
    }
    abort_reason = (llam_io_abort_reason_t)atomic_exchange(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->platform_data = NULL;
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;

    if (res >= 0) {
        if (req->kind == LLAM_IO_KIND_POLL) {
            req->result = res != 0 ? 1 : 0;
        } else {
            req->result = req->kind == LLAM_IO_KIND_ACCEPT && !LLAM_FD_IS_INVALID(req->fd_result) ? (ssize_t)req->fd_result : res;
        }
        req->error_code = 0;
        req->poll_revents = req->kind == LLAM_IO_KIND_POLL ? (short)res : 0;
        if (req->owned_buffer != NULL && req->kind == LLAM_IO_KIND_READ) {
            req->owned_buffer->size = (size_t)res;
        }
    } else if (res == -ECANCELED && abort_reason != LLAM_IO_ABORT_NONE) {
        llam_io_set_abort_result(req, abort_reason);
        wake_reason = llam_io_abort_wait_reason(abort_reason);
    } else {
        req->result = -1;
        req->error_code = -res;
        req->poll_revents = 0;
        if (req->kind == LLAM_IO_KIND_ACCEPT) {
            req->fd_result = LLAM_INVALID_FD;
        }
    }

    if (completion_owner < node->runtime->active_shards) {
        llam_shard_t *shard = &node->runtime->shards[completion_owner];
        uint64_t now_ns = llam_now_ns();

        pthread_mutex_lock(&shard->lock);
        if (wake_reason == LLAM_WAIT_CANCEL) {
            shard->metrics.cancel_wakes += 1U;
        } else if (wake_reason == LLAM_WAIT_TIMEOUT) {
            shard->metrics.timeout_wakes += 1U;
        }
        if (now_ns >= req->submit_ts_ns) {
            shard->metrics.io_completion_latency_ns += now_ns - req->submit_ts_ns;
            shard->metrics.io_completion_samples += 1U;
        }
        pthread_mutex_unlock(&shard->lock);
    }

    llam_reinject_task_on_shard(node->runtime,
                              req->task,
                              completion_owner,
                              true,
                              LLAM_TRACE_IO_COMPLETE,
                              wake_reason);
}

void llam_windows_complete_submit_error(llam_node_t *node, llam_io_req_t *req, int err) {
    if (err <= 0) {
        err = EIO;
    }
    llam_windows_complete_req(node, req, -err, true);
}

static void llam_windows_copy_accept_addr(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;
    struct sockaddr *local_addr = NULL;
    struct sockaddr *remote_addr = NULL;
    int local_len = 0;
    int remote_len = 0;

    if (req->addr == NULL || req->addrlen == NULL) {
        return;
    }

    GetAcceptExSockaddrs(op->accept_buffer,
                         0,
                         LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                         LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                         &local_addr,
                         &local_len,
                         &remote_addr,
                         &remote_len);
    (void)local_addr;
    (void)local_len;
    if (remote_addr == NULL || remote_len <= 0) {
        *req->addrlen = 0;
        return;
    }
    if (*req->addrlen > 0) {
        size_t copy_len = (size_t)*req->addrlen < (size_t)remote_len ? (size_t)*req->addrlen : (size_t)remote_len;

        memcpy(req->addr, remote_addr, copy_len);
    }
    *req->addrlen = (socklen_t)remote_len;
}

static int llam_windows_finalize_accept(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;

    if (setsockopt(op->accept_socket,
                   SOL_SOCKET,
                   SO_UPDATE_ACCEPT_CONTEXT,
                   (const char *)&req->fd,
                   (int)sizeof(req->fd)) != 0) {
        return llam_windows_wsa_error_to_errno(WSAGetLastError());
    }
    llam_windows_copy_accept_addr(op);
    req->fd_result = op->accept_socket;
    op->accept_socket = INVALID_SOCKET;
    return 0;
}

static int llam_windows_finalize_connect(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;

    if (setsockopt(req->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
        return llam_windows_wsa_error_to_errno(WSAGetLastError());
    }
    return 0;
}

static void llam_windows_handle_completion(const llam_windows_iocp_completion_t *completion) {
    llam_windows_io_op_t *op = (llam_windows_io_op_t *)completion->overlapped;
    llam_io_req_t *req;
    llam_node_t *node;
    DWORD transferred;
    DWORD flags = 0;
    int result;
    int err = 0;

    if (completion->overlapped == 0U) {
        return;
    }
    if (op == NULL || op->magic != LLAM_WINDOWS_IO_OP_MAGIC || op->req == NULL || op->node == NULL) {
        return;
    }

    req = op->req;
    node = op->node;
    transferred = completion->bytes;
    if (req->kind == LLAM_IO_KIND_HANDLE_READ || req->kind == LLAM_IO_KIND_HANDLE_WRITE) {
        if (!GetOverlappedResult((HANDLE)req->handle, &op->overlapped, &transferred, FALSE)) {
            err = llam_windows_system_error_to_errno(GetLastError());
        }
    } else if (!WSAGetOverlappedResult(req->fd, &op->overlapped, &transferred, FALSE, &flags)) {
        err = llam_windows_wsa_error_to_errno(WSAGetLastError());
    }

    if (req->kind == LLAM_IO_KIND_POLL && err == EMSGSIZE &&
        op->poll_backend == LLAM_WINDOWS_POLL_BACKEND_RECVFROM_PEEK) {
        err = 0;
    }

    if (err == 0 && req->kind == LLAM_IO_KIND_ACCEPT) {
        err = llam_windows_finalize_accept(op);
        result = 0;
    } else if (err == 0 && req->kind == LLAM_IO_KIND_CONNECT) {
        err = llam_windows_finalize_connect(op);
        result = 0;
    } else if (err == 0 && req->kind == LLAM_IO_KIND_POLL) {
        result = op->poll_backend == LLAM_WINDOWS_POLL_BACKEND_SEND ? POLLOUT : POLLIN;
    } else {
        result = (int)transferred;
    }

    if (err != 0) {
        if (req->kind == LLAM_IO_KIND_ACCEPT) {
            req->fd_result = LLAM_INVALID_FD;
        }
        llam_windows_complete_req(node, req, -err, true);
    } else {
        llam_windows_complete_req(node, req, result, true);
    }
    llam_windows_io_op_free(op);
}

void llam_windows_complete_immediate_op(llam_windows_io_op_t *op, DWORD bytes) {
    llam_windows_iocp_completion_t completion;

    if (op == NULL || op->node == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&op->node->windows_immediate_completions, 1U, memory_order_relaxed);
    /*
     * Handles configured with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS do not post
     * an IOCP packet for synchronous overlapped success.  Reuse the ordinary
     * completion finalizer so AcceptEx/ConnectEx context updates, owned-buffer
     * sizing, owner routing, and op recycling stay identical to queued packets.
     */
    memset(&completion, 0, sizeof(completion));
    completion.overlapped = (uintptr_t)&op->overlapped;
    completion.bytes = (uint32_t)bytes;
    llam_windows_handle_completion(&completion);
}

void llam_windows_drain_completions(llam_node_t *node, DWORD timeout_ms) {
    llam_windows_iocp_completion_t entries[LLAM_WINDOWS_IOCP_BATCH_MAX];
    size_t batch = node->windows_completion_batch != 0U ? node->windows_completion_batch : 64U;
    unsigned rounds = 0U;

    if (batch > LLAM_WINDOWS_IOCP_BATCH_MAX) {
        batch = LLAM_WINDOWS_IOCP_BATCH_MAX;
    }
    for (;;) {
        size_t count = 0U;
        size_t i;

        if (llam_windows_iocp_drain(node->windows_iocp_handle, entries, batch, timeout_ms, &count) != 0) {
            if (errno != ETIMEDOUT) {
                llam_record_fatal(node->runtime, errno != 0 ? errno : EIO);
            }
            return;
        }
        atomic_store_explicit(&node->last_cq_depth, (unsigned)count, memory_order_relaxed);
        llam_atomic_update_peak(&node->max_cq_depth, (unsigned)count);
        for (i = 0U; i < count; ++i) {
            if (entries[i].overlapped == 0U && entries[i].key == LLAM_WINDOWS_IOCP_WAKE_KEY) {
                llam_drain_node_wake(node);
                continue;
            }
            llam_windows_handle_completion(&entries[i]);
        }
        rounds += 1U;
        if (count < batch || rounds >= 4U) {
            return;
        }
        timeout_ms = 0U;
    }
}
