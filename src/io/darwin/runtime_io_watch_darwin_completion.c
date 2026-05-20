/**
 * @file src/io/darwin/runtime_io_watch_darwin_completion.c
 * @brief Darwin/kqueue completion helpers and request finalization.
 *
 * @details
 * Darwin readiness events often require a follow-up nonblocking syscall to
 * produce the final read/write/accept result. This file translates kqueue events
 * into request results, manages copied owned-buffer payloads, and wakes parked
 * tasks through the common reinjection path.
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

#include "runtime_io_watch_darwin_internal.h"

/** @brief Convert a kevent filter/flags pair into poll-style revents. */
short llam_darwin_poll_revents(const struct kevent *event) {
    short revents = 0;

    if (event->filter == EVFILT_READ) {
        revents |= POLLIN;
    } else if (event->filter == EVFILT_WRITE) {
        revents |= POLLOUT;
    }
    if ((event->flags & EV_EOF) != 0U) {
        revents |= POLLHUP;
    }
    if ((event->flags & EV_ERROR) != 0U) {
        revents |= POLLERR;
    }
    return revents;
}

/**
 * @brief Complete one Darwin I/O request and wake its task.
 *
 * @param node              Node that observed the completion.
 * @param req               Completed request.
 * @param res               Result value, or negative errno.
 * @param decrement_pending Whether pending_ops should be decremented.
 */
void llam_io_complete_req(llam_node_t *node, llam_io_req_t *req, int res, bool decrement_pending) {
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
         * Balance the in-flight owner counter established by submission.  The
         * exchanged owner is also the authoritative completion target because
         * dynamic shard rehome can update req->owner_shard concurrently after
         * transferring this atomic owner.
         */
        inflight_owner = atomic_exchange_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_acq_rel);
        if (inflight_owner < node->runtime->active_shards) {
            llam_shard_note_inflight_io_waiter(inflight_owner, -1);
            completion_owner = inflight_owner;
        }
    } else {
        atomic_store_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_release);
        completion_owner = req->owner_shard;
    }
    abort_reason = (llam_io_abort_reason_t)atomic_exchange(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;

    if (res >= 0) {
        req->result = req->kind == LLAM_IO_KIND_POLL ? (res != 0 ? 1 : 0) : res;
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

/** @brief Drain deferred accept completions outside watch locks. */
void llam_darwin_complete_accept_completions(llam_node_t *node, llam_darwin_accept_completion_t *head) {
    while (head != NULL) {
        llam_darwin_accept_completion_t *next = head->next;

        llam_io_complete_req(node, head->req, head->result, false);
        free(head);
        head = next;
    }
}

/**
 * @brief Temporarily set an fd to nonblocking mode when needed.
 *
 * @return 0 on success, -1 on fcntl failure.
 */
int llam_darwin_fd_set_nonblocking(int fd, int *saved_flags_out, bool *restore_out) {
    int saved_flags;

    if (saved_flags_out != NULL) {
        *saved_flags_out = 0;
    }
    if (restore_out != NULL) {
        *restore_out = false;
    }

    saved_flags = fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0) {
        return -1;
    }
    if ((saved_flags & O_NONBLOCK) != 0) {
        if (saved_flags_out != NULL) {
            *saved_flags_out = saved_flags;
        }
        return 0;
    }
    if (fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
        return -1;
    }
    if (saved_flags_out != NULL) {
        *saved_flags_out = saved_flags;
    }
    if (restore_out != NULL) {
        *restore_out = true;
    }
    return 0;
}

/** @brief Restore an fd's previous flags if ::llam_darwin_fd_set_nonblocking changed them. */
void llam_darwin_fd_restore(int fd, int saved_flags, bool restore) {
    if (restore) {
        (void)fcntl(fd, F_SETFL, saved_flags);
    }
}

/**
 * @brief Assign bytes to an owned-buffer request result.
 *
 * @param req            Request with an owned buffer.
 * @param data           Source bytes when copying.
 * @param size           Valid byte count.
 * @param owned_data     Optional heap buffer whose ownership should transfer.
 * @param owned_capacity Capacity of @p owned_data.
 * @return true on success.
 */
bool llam_darwin_assign_owned_buffer(llam_io_req_t *req,
                                          const unsigned char *data,
                                          size_t size,
                                          unsigned char *owned_data,
                                          size_t owned_capacity) {
    llam_io_buffer_t *buffer;

    if (req == NULL || req->owned_buffer == NULL) {
        return false;
    }
    buffer = req->owned_buffer;
    if (buffer->external_storage && buffer->data != NULL) {
        free(buffer->data);
        buffer->external_storage = false;
    }
    buffer->provided_storage = false;
    buffer->provided_bid = 0U;
    if (owned_data != NULL) {
        // The caller already allocated/copied payload memory; transfer it into
        // the public owned-buffer wrapper.
        buffer->data = owned_data;
        buffer->capacity = owned_capacity;
        buffer->external_storage = true;
    } else if (size <= LLAM_IO_BUFFER_INLINE_BYTES) {
        if (size > 0U && data != NULL) {
            memcpy(buffer->inline_data, data, size);
        }
        buffer->data = buffer->inline_data;
        buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
        buffer->external_storage = false;
    } else {
        buffer->data = malloc(size);
        if (buffer->data == NULL) {
            buffer->data = buffer->inline_data;
            buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
            buffer->external_storage = false;
            return false;
        }
        memcpy(buffer->data, data, size);
        buffer->capacity = size;
        buffer->external_storage = true;
    }
    buffer->size = size;
    return true;
}

/**
 * @brief Try to complete a readiness request with a nonblocking syscall.
 *
 * @param req        Request to execute.
 * @param result_out Receives result or negative errno.
 * @return 1 completed, 0 would block and should register kqueue, -1 hard error.
 */
int llam_darwin_try_req_syscall(llam_io_req_t *req, int *result_out) {
    int saved_flags = 0;
    bool restore_flags = false;
    int rc = 0;

    if (req == NULL || result_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (llam_darwin_fd_set_nonblocking(req->fd, &saved_flags, &restore_flags) != 0) {
        *result_out = -errno;
        return -1;
    }

    for (;;) {
        switch (req->kind) {
        case LLAM_IO_KIND_READ:
            rc = req->use_recv_op ? (int)recv(req->fd, req->buf, req->count, req->recv_flags) :
                                    (int)read(req->fd, req->buf, req->count);
            break;
        case LLAM_IO_KIND_WRITE:
            rc = (int)write(req->fd, req->buf, req->count);
            break;
        case LLAM_IO_KIND_ACCEPT:
            rc = accept(req->fd, req->addr, req->addrlen);
            break;
        case LLAM_IO_KIND_CONNECT:
            rc = connect(req->fd, req->addr, req->addr_len);
            break;
        default:
            llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
            errno = EOPNOTSUPP;
            *result_out = -EOPNOTSUPP;
            return -1;
        }

        if (rc >= 0) {
            llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
            *result_out = rc;
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // kqueue reported readiness but the nonblocking operation would
            // still block; re-register and wait for the next event.
            llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
            return 0;
        }
        if (req->kind == LLAM_IO_KIND_CONNECT && (errno == EINPROGRESS || errno == EALREADY)) {
            llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
            return 0;
        }
        if (req->kind == LLAM_IO_KIND_CONNECT && errno == EISCONN) {
            llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
            *result_out = 0;
            return 1;
        }
        llam_darwin_fd_restore(req->fd, saved_flags, restore_flags);
        *result_out = -errno;
        return -1;
    }
}
