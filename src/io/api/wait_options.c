/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "io/runtime_io_api_internal.h"
#include <limits.h>
#if LLAM_PLATFORM_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#endif

typedef struct llam_io_scope {
    llam_task_t *task;
    llam_cancel_token_t *token;
} llam_io_scope_t;

int llam_wait_opts_init(llam_wait_opts_t *opts, size_t size) {
    if (opts == NULL || size == 0U) { errno = EINVAL; return -1; }
    memset(opts, 0, size < sizeof(*opts) ? size : sizeof(*opts));
    return 0;
}

static int llam_wait_opts_normalize(const llam_wait_opts_t *opts, size_t size,
                                    llam_wait_opts_t *out) {
    memset(out, 0, sizeof(*out));
    if (opts == NULL) return 0;
    if (size == 0U) { errno = EINVAL; return -1; }
    llam_wait_opts_t raw = {0};
    memcpy(&raw, opts, size < sizeof(raw) ? size : sizeof(raw));
#define WAIT_FIELD_PRESENT(field) (size >= offsetof(llam_wait_opts_t, field) + sizeof(raw.field))
    if (WAIT_FIELD_PRESENT(deadline_ns)) out->deadline_ns = raw.deadline_ns;
    if (WAIT_FIELD_PRESENT(cancel_token)) out->cancel_token = raw.cancel_token;
    if ((WAIT_FIELD_PRESENT(flags) && raw.flags != 0U) ||
        (WAIT_FIELD_PRESENT(reserved0) && raw.reserved0 != 0U)) {
        errno = EINVAL;
        return -1;
    }
#undef WAIT_FIELD_PRESENT
    return 0;
}

int llam_io_scope_error(const llam_task_t *task) {
    if (task == NULL || !task->io_scope_active) return 0;
    if (atomic_load_explicit(&task->owner_runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&task->owner_runtime->shutdown_requested, memory_order_acquire) ||
        llam_task_cancel_token_is_cancelled(task)) return ECANCELED;
    uint64_t deadline = llam_task_request_deadline(task);
    return deadline != 0U && llam_now_ns() >= deadline ? ETIMEDOUT : 0;
}

static int llam_io_scope_begin(const llam_wait_opts_t *opts, size_t size, llam_io_scope_t *scope) {
    llam_wait_opts_t normalized;
    if (llam_wait_opts_normalize(opts, size, &normalized) != 0) return -1;
    scope->task = g_llam_tls_task;
    scope->token = NULL;
    if (scope->task == NULL) {
        if (normalized.deadline_ns != 0U || normalized.cancel_token != NULL) {
            errno = ENOTSUP; /* A native blocking syscall cannot promise this scope. */
            return -1;
        }
        return 0;
    }
    if (llam_require_task_context() != 0 ||
        atomic_load_explicit(&scope->task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        errno = ENOTSUP;
        return -1;
    }
    if (scope->task->io_scope_active) { errno = EBUSY; return -1; }
    if (normalized.cancel_token != NULL &&
        llam_cancel_token_retain_task_ref_for_runtime(normalized.cancel_token,
                                                       scope->task->owner_runtime, &scope->token) != 0)
        return -1;
    scope->task->operation_cancel_token = scope->token;
    scope->task->operation_deadline_ns = normalized.deadline_ns;
    scope->task->io_scope_active = true;
    int error = llam_io_scope_error(scope->task);
    if (error != 0) {
        scope->task->operation_cancel_token = NULL;
        scope->task->operation_deadline_ns = 0U;
        scope->task->io_scope_active = false;
        llam_cancel_token_release_task_ref(scope->token);
        errno = error;
        return -1;
    }
    return 0;
}

static void llam_io_scope_end(llam_io_scope_t *scope, int saved_errno) {
    if (scope->task != NULL) {
        llam_cancel_token_unregister_task(scope->task);
        /* An old token's detached cancellation must finish before this slot
         * can describe another operation. No wait-owner lock is held here;
         * yielding lets a same-P canceller complete without blocking M. */
        while (atomic_load_explicit(&scope->task->cancel_links[2].delivering, memory_order_acquire))
            llam_yield_internal();
        scope->task->operation_cancel_token = NULL;
        scope->task->operation_deadline_ns = 0U;
        scope->task->io_scope_active = false;
        llam_cancel_token_release_task_ref(scope->token);
    }
    llam_thread_errno_store(saved_errno);
}

static int llam_io_scope_validate_fd(const llam_io_scope_t *scope, llam_fd_t fd) {
#if LLAM_PLATFORM_POSIX
    if (scope->task != NULL && (llam_task_request_deadline(scope->task) != 0U ||
                                llam_task_has_cancel_tokens(scope->task))) {
        struct stat st;
        if (fstat(fd, &st) != 0) return -1;
        if (S_ISSOCK(st.st_mode)) return 0;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        if ((!S_ISFIFO(st.st_mode) && !S_ISCHR(st.st_mode)) || (flags & O_NONBLOCK) == 0) {
            errno = ENOTSUP;
            return -1;
        }
    }
#else
    (void)scope;
    (void)fd;
#endif
    return 0;
}

typedef enum llam_scoped_byte_kind {
    LLAM_SCOPED_READ, LLAM_SCOPED_WRITE, LLAM_SCOPED_HANDLE_READ, LLAM_SCOPED_HANDLE_WRITE
} llam_scoped_byte_kind_t;
typedef struct llam_scoped_endpoint {
    llam_scoped_byte_kind_t kind;
    union { llam_fd_t fd; llam_handle_t handle; } as;
} llam_scoped_endpoint_t;

static ssize_t llam_scoped_bytes(llam_scoped_endpoint_t endpoint, void *buf, size_t count,
                                  const llam_wait_opts_t *opts, size_t size) {
    if (buf == NULL && count != 0U) { errno = EINVAL; return -1; }
    llam_io_scope_t scope;
    if (llam_io_scope_begin(opts, size, &scope) != 0) return -1;
    ssize_t result = -1;
    bool fd_mode = endpoint.kind == LLAM_SCOPED_READ || endpoint.kind == LLAM_SCOPED_WRITE;
#if LLAM_PLATFORM_POSIX
    llam_fd_t fd = fd_mode ? endpoint.as.fd : (llam_fd_t)endpoint.as.handle;
    if (llam_io_scope_validate_fd(&scope, fd) != 0) goto done;
#else
    if (fd_mode && llam_io_scope_validate_fd(&scope, endpoint.as.fd) != 0) goto done;
#endif
    switch (endpoint.kind) {
    case LLAM_SCOPED_READ: result = llam_read(endpoint.as.fd, buf, count); break;
    case LLAM_SCOPED_WRITE: result = llam_write(endpoint.as.fd, buf, count); break;
    case LLAM_SCOPED_HANDLE_READ: result = llam_read_handle(endpoint.as.handle, buf, count); break;
    case LLAM_SCOPED_HANDLE_WRITE: result = llam_write_handle(endpoint.as.handle, buf, count); break;
    }
done:
    llam_io_scope_end(&scope, llam_thread_errno_load_current());
    return result;
}

ssize_t llam_read_ex(llam_fd_t fd, void *buf, size_t count, const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_scoped_endpoint_t endpoint = {.kind = LLAM_SCOPED_READ, .as.fd = fd};
    return llam_scoped_bytes(endpoint, buf, count, opts, size);
}
ssize_t llam_write_ex(llam_fd_t fd, const void *buf, size_t count, const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_scoped_endpoint_t endpoint = {.kind = LLAM_SCOPED_WRITE, .as.fd = fd};
    return llam_scoped_bytes(endpoint, (void *)buf, count, opts, size);
}
ssize_t llam_read_handle_ex(llam_handle_t handle, void *buf, size_t count,
                            const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_scoped_endpoint_t endpoint = {.kind = LLAM_SCOPED_HANDLE_READ, .as.handle = handle};
    return llam_scoped_bytes(endpoint, buf, count, opts, size);
}
ssize_t llam_write_handle_ex(llam_handle_t handle, const void *buf, size_t count,
                             const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_scoped_endpoint_t endpoint = {.kind = LLAM_SCOPED_HANDLE_WRITE, .as.handle = handle};
    return llam_scoped_bytes(endpoint, (void *)buf, count, opts, size);
}

int llam_connect_ex(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen,
                    const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_io_scope_t scope;
    if (llam_io_scope_begin(opts, size, &scope) != 0) return -1;
    int result = llam_connect(fd, addr, addrlen);
    llam_io_scope_end(&scope, llam_thread_errno_load_current());
    return result;
}

static int llam_deadline_timeout_ms(uint64_t deadline) {
    if (deadline == 0U) return -1;
    uint64_t now = llam_now_ns();
    if (now >= deadline) return 0;
    uint64_t remaining = deadline - now;
    uint64_t ms = remaining / UINT64_C(1000000) + (remaining % UINT64_C(1000000) != 0U);
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static int llam_host_poll_scope(llam_fd_t fd, short events, short *revents,
                                const llam_wait_opts_t *opts, size_t size) {
    llam_wait_opts_t normalized;
    if (llam_wait_opts_normalize(opts, size, &normalized) != 0) return -1;
    llam_cancel_token_t *token = NULL;
    if (normalized.cancel_token != NULL &&
        llam_cancel_token_retain_task_ref(normalized.cancel_token, &token) != 0) return -1;
    llam_runtime_t *owner = NULL;
    if (token != NULL && !llam_runtime_begin_object_owner_op_if_live(token->owner_runtime, &owner) &&
        token->owner_runtime != llam_runtime_default_storage()) {
        llam_cancel_token_release_task_ref(token);
        errno = EXDEV;
        return -1;
    }
    int result;
    for (;;) {
        if ((token != NULL && atomic_load_explicit(&token->cancelled, memory_order_acquire)) ||
            (owner != NULL && (atomic_load_explicit(&owner->stop_requested, memory_order_acquire) ||
                               atomic_load_explicit(&owner->shutdown_requested, memory_order_acquire) ||
                               atomic_load_explicit(&owner->draining, memory_order_acquire)))) {
            errno = ECANCELED;
            result = -1;
            break;
        }
        int timeout = llam_deadline_timeout_ms(normalized.deadline_ns);
        if (timeout == 0) { result = 0; break; }
        if (token != NULL && (timeout < 0 || timeout > 10)) timeout = 10;
        result = llam_poll_fd(fd, events, timeout, revents);
        if (result > 0 || (result < 0 && errno != EINTR)) break;
    }
    int error = errno;
    llam_cancel_token_release_task_ref(token);
    llam_runtime_end_public_op(owner);
    errno = error;
    return result;
}

int llam_poll_fd_ex(llam_fd_t fd, int events, short *revents,
                    const llam_wait_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    if (events < 0 || events > SHRT_MAX) { errno = EINVAL; return -1; }
    if (revents != NULL) *revents = 0;
    if (g_llam_tls_task == NULL) return llam_host_poll_scope(fd, events, revents, opts, size);
    llam_io_scope_t scope;
    if (llam_io_scope_begin(opts, size, &scope) != 0) return errno == ETIMEDOUT ? 0 : -1;
    int result = llam_poll_fd(fd, events, llam_deadline_timeout_ms(llam_task_request_deadline(scope.task)), revents);
    int error = llam_thread_errno_load_current();
    if (result < 0 && error == ETIMEDOUT) result = 0;
    llam_io_scope_end(&scope, error);
    return result;
}

ssize_t llam_read_until(llam_fd_t fd, void *buf, size_t count, uint64_t deadline) {
    LLAM_NATIVE_API_GUARD(-1);
    if (deadline == 0U) { errno = ETIMEDOUT; return -1; }
    llam_wait_opts_t opts = {.deadline_ns = deadline};
    return llam_read_ex(fd, buf, count, &opts, sizeof(opts));
}
ssize_t llam_write_until(llam_fd_t fd, const void *buf, size_t count, uint64_t deadline) {
    LLAM_NATIVE_API_GUARD(-1);
    if (deadline == 0U) { errno = ETIMEDOUT; return -1; }
    llam_wait_opts_t opts = {.deadline_ns = deadline};
    return llam_write_ex(fd, buf, count, &opts, sizeof(opts));
}
int llam_connect_until(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t deadline) {
    LLAM_NATIVE_API_GUARD(-1);
    if (deadline == 0U) { errno = ETIMEDOUT; return -1; }
    llam_wait_opts_t opts = {.deadline_ns = deadline};
    return llam_connect_ex(fd, addr, addrlen, &opts, sizeof(opts));
}
int llam_poll_fd_until(llam_fd_t fd, int events, uint64_t deadline, short *revents) {
    LLAM_NATIVE_API_GUARD(-1);
    if (deadline == 0U) { if (revents != NULL) *revents = 0; errno = ETIMEDOUT; return 0; }
    llam_wait_opts_t opts = {.deadline_ns = deadline};
    return llam_poll_fd_ex(fd, events, revents, &opts, sizeof(opts));
}

static int llam_transfer_all(llam_fd_t fd, void *buf, size_t count, bool writing,
                              const llam_wait_opts_t *opts, size_t size, size_t *progress) {
    if (progress == NULL) { errno = EINVAL; return -1; }
    *progress = 0U;
    if ((buf == NULL && count != 0U) || count > (SIZE_MAX >> 1U)) { errno = EINVAL; return -1; }
    llam_io_scope_t scope;
    if (llam_io_scope_begin(opts, size, &scope) != 0) return -1;
    int result = -1;
    if (llam_io_scope_validate_fd(&scope, fd) != 0) goto done;
    while (*progress < count) {
        int error = llam_io_scope_error(scope.task);
        if (error != 0) { llam_thread_errno_store(error); goto done; }
        size_t chunk = count - *progress;
        if (chunk > INT_MAX) chunk = INT_MAX;
        void *current = (unsigned char *)buf + *progress;
        ssize_t n = writing ? llam_write(fd, current, chunk) : llam_read(fd, current, chunk);
        if (n < 0) goto done;
        if (n == 0 || (size_t)n > chunk) {
            llam_thread_errno_store(n == 0 ? (writing ? EIO : EPIPE) : EIO);
            goto done;
        }
        *progress += (size_t)n;
    }
    result = 0;
done:
    llam_io_scope_end(&scope, llam_thread_errno_load_current());
    return result;
}

int llam_read_exact_ex(llam_fd_t fd, void *buf, size_t count,
                       const llam_wait_opts_t *opts, size_t size, size_t *progress) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_transfer_all(fd, buf, count, false, opts, size, progress);
}
int llam_write_all_ex(llam_fd_t fd, const void *buf, size_t count,
                      const llam_wait_opts_t *opts, size_t size, size_t *progress) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_transfer_all(fd, (void *)buf, count, true, opts, size, progress);
}
int llam_read_exact(llam_fd_t fd, void *buf, size_t count, size_t *progress) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_transfer_all(fd, buf, count, false, NULL, 0U, progress);
}
int llam_write_all(llam_fd_t fd, const void *buf, size_t count, size_t *progress) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_transfer_all(fd, (void *)buf, count, true, NULL, 0U, progress);
}
