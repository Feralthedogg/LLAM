/**
 * @file src/io/windows/watch/socket.c
 * @brief Windows IOCP socket association and extension-function helpers.
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

#include "io/windows/runtime_io_watch_windows_internal.h"

static llam_windows_fd_assoc_t *llam_windows_find_assoc_locked(llam_node_t *node, uintptr_t key) {
    llam_windows_fd_assoc_t *assoc;

    if (node == NULL) {
        return NULL;
    }
    for (assoc = node->windows_fd_assoc_head; assoc != NULL; assoc = assoc->next) {
        if ((uintptr_t)assoc->fd == key) {
            return assoc;
        }
    }
    return NULL;
}

static unsigned llam_windows_try_skip_completion_on_success(llam_node_t *node, HANDLE handle) {
    if (node == NULL || handle == NULL || node->windows_use_skip_completion_on_success == 0U) {
        return 0U;
    }
    /*
     * Windows 11 tuning can skip IOCP packets for immediately successful
     * overlapped requests.  That removes a completion-port round trip on the
     * hot direct-success path; submit.c completes those requests inline only
     * when this per-handle opt-in succeeded.
     */
    if (SetFileCompletionNotificationModes(handle, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
        atomic_fetch_add_explicit(&node->windows_skip_completion_handles, 1U, memory_order_relaxed);
        return 1U;
    }
    atomic_fetch_add_explicit(&node->windows_skip_completion_failures, 1U, memory_order_relaxed);
    return 0U;
}

int llam_windows_associate_fd(llam_node_t *node, llam_fd_t fd) {
    llam_windows_fd_assoc_t *assoc;
    HANDLE handle;
    DWORD error_code;

    if (node == NULL || node->windows_iocp_handle == NULL || LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }

    assoc = calloc(1, sizeof(*assoc));
    if (assoc == NULL) {
        errno = ENOMEM;
        return -1;
    }

    /*
     * Association metadata is keyed by raw HANDLE/SOCKET values and is touched
     * by public close paths as well as IOCP submit paths.  Keep it off
     * watch_lock so slow Windows association calls cannot block the worker from
     * draining watch tables or control packets.
     */
    pthread_mutex_lock(&node->windows_assoc_lock);
    if (llam_windows_find_assoc_locked(node, (uintptr_t)fd) != NULL) {
        pthread_mutex_unlock(&node->windows_assoc_lock);
        free(assoc);
        return 0;
    }
    handle = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, (HANDLE)node->windows_iocp_handle, 0, 0);
    if (handle == NULL) {
        error_code = GetLastError();
        pthread_mutex_unlock(&node->windows_assoc_lock);
        free(assoc);
        errno = error_code == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EINVAL;
        return -1;
    }
    assoc->fd = fd;
    assoc->skip_completion_on_success = llam_windows_try_skip_completion_on_success(node, (HANDLE)(uintptr_t)fd);
    assoc->next = node->windows_fd_assoc_head;
    node->windows_fd_assoc_head = assoc;
    pthread_mutex_unlock(&node->windows_assoc_lock);
    return 0;
}

int llam_windows_associate_handle(llam_node_t *node, llam_handle_t raw_handle) {
    uintptr_t key = (uintptr_t)raw_handle;
    llam_windows_fd_assoc_t *assoc;
    HANDLE handle;
    DWORD error_code;

    if (node == NULL || node->windows_iocp_handle == NULL || LLAM_HANDLE_IS_INVALID(raw_handle)) {
        errno = EINVAL;
        return -1;
    }

    assoc = calloc(1, sizeof(*assoc));
    if (assoc == NULL) {
        errno = ENOMEM;
        return -1;
    }

    pthread_mutex_lock(&node->windows_assoc_lock);
    if (llam_windows_find_assoc_locked(node, key) != NULL) {
        pthread_mutex_unlock(&node->windows_assoc_lock);
        free(assoc);
        return 0;
    }
    handle = CreateIoCompletionPort((HANDLE)raw_handle, (HANDLE)node->windows_iocp_handle, 0, 0);
    if (handle == NULL) {
        error_code = GetLastError();
        pthread_mutex_unlock(&node->windows_assoc_lock);
        free(assoc);
        errno = llam_windows_system_error_to_errno(error_code);
        return -1;
    }
    assoc->fd = (llam_fd_t)key;
    assoc->skip_completion_on_success = llam_windows_try_skip_completion_on_success(node, (HANDLE)raw_handle);
    assoc->next = node->windows_fd_assoc_head;
    node->windows_fd_assoc_head = assoc;
    pthread_mutex_unlock(&node->windows_assoc_lock);
    return 0;
}

void llam_windows_forget_fd_assoc(llam_runtime_t *rt, llam_fd_t fd) {
    uintptr_t key = (uintptr_t)fd;

    if (rt == NULL || rt->nodes == NULL) {
        return;
    }
    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        llam_node_t *node = &rt->nodes[i];
        llam_windows_fd_assoc_t *prev = NULL;
        llam_windows_fd_assoc_t *assoc;

        pthread_mutex_lock(&node->windows_assoc_lock);
        assoc = node->windows_fd_assoc_head;
        while (assoc != NULL) {
            if ((uintptr_t)assoc->fd == key) {
                if (prev != NULL) {
                    prev->next = assoc->next;
                } else {
                    node->windows_fd_assoc_head = assoc->next;
                }
                free(assoc);
                break;
            }
            prev = assoc;
            assoc = assoc->next;
        }
        pthread_mutex_unlock(&node->windows_assoc_lock);
    }
}

bool llam_windows_fd_skips_completion_on_success(llam_node_t *node, llam_fd_t fd) {
    llam_windows_fd_assoc_t *assoc;
    bool skips;

    if (node == NULL) {
        return false;
    }
    pthread_mutex_lock(&node->windows_assoc_lock);
    assoc = llam_windows_find_assoc_locked(node, (uintptr_t)fd);
    skips = assoc != NULL && assoc->skip_completion_on_success != 0U;
    pthread_mutex_unlock(&node->windows_assoc_lock);
    return skips;
}

bool llam_windows_handle_skips_completion_on_success(llam_node_t *node, llam_handle_t handle) {
    llam_windows_fd_assoc_t *assoc;
    bool skips;

    if (node == NULL) {
        return false;
    }
    pthread_mutex_lock(&node->windows_assoc_lock);
    assoc = llam_windows_find_assoc_locked(node, (uintptr_t)handle);
    skips = assoc != NULL && assoc->skip_completion_on_success != 0U;
    pthread_mutex_unlock(&node->windows_assoc_lock);
    return skips;
}

int llam_windows_load_acceptex(llam_node_t *node, SOCKET socket_fd, LPFN_ACCEPTEX *fn_out) {
    GUID guid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX fn = NULL;
    DWORD bytes = 0;

    if (node == NULL || fn_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (node->windows_acceptex != NULL) {
        *fn_out = node->windows_acceptex;
        return 0;
    }
    if (WSAIoctl(socket_fd,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid,
                 sizeof(guid),
                 &fn,
                 sizeof(fn),
                 &bytes,
                 NULL,
                 NULL) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    node->windows_acceptex = fn;
    *fn_out = fn;
    return 0;
}

int llam_windows_load_connectex(llam_node_t *node, SOCKET socket_fd, LPFN_CONNECTEX *fn_out) {
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX fn = NULL;
    DWORD bytes = 0;

    if (node == NULL || fn_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (node->windows_connectex != NULL) {
        *fn_out = node->windows_connectex;
        return 0;
    }
    if (WSAIoctl(socket_fd,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid,
                 sizeof(guid),
                 &fn,
                 sizeof(fn),
                 &bytes,
                 NULL,
                 NULL) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    node->windows_connectex = fn;
    *fn_out = fn;
    return 0;
}

int llam_windows_bind_connect_socket(SOCKET socket_fd, const struct sockaddr *addr) {
    int rc;

    if (addr == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in local_addr;

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        rc = bind(socket_fd, (const struct sockaddr *)&local_addr, (int)sizeof(local_addr));
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 local_addr;

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin6_family = AF_INET6;
        rc = bind(socket_fd, (const struct sockaddr *)&local_addr, (int)sizeof(local_addr));
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    if (rc == 0) {
        return 0;
    }
    {
        int err = WSAGetLastError();

        if (err == WSAEINVAL || err == WSAEADDRINUSE) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
}

bool llam_windows_socket_info(llam_fd_t fd, int *family_out, int *socket_type_out) {
    struct sockaddr_storage local_addr;
    int local_len = (int)sizeof(local_addr);
    int socket_type = 0;
    int socket_type_len = (int)sizeof(socket_type);

    if (family_out != NULL) {
        *family_out = AF_UNSPEC;
    }
    if (socket_type_out != NULL) {
        *socket_type_out = 0;
    }
    if (LLAM_FD_IS_INVALID(fd)) {
        return false;
    }
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(fd, (struct sockaddr *)&local_addr, &local_len) != 0) {
        return false;
    }
    if (((struct sockaddr *)&local_addr)->sa_family != AF_INET &&
        ((struct sockaddr *)&local_addr)->sa_family != AF_INET6) {
        return false;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&socket_type, &socket_type_len) != 0) {
        return false;
    }
    if (family_out != NULL) {
        *family_out = ((struct sockaddr *)&local_addr)->sa_family;
    }
    if (socket_type_out != NULL) {
        *socket_type_out = socket_type;
    }
    return true;
}

/**
 * @brief Return whether experimental TCP POLLIN IOCP probes are enabled.
 *
 * Stream read readiness uses a one-byte overlapped @c WSARecv(MSG_PEEK). It is
 * useful for controlled Windows 10/11 experiments, but remains opt-in because
 * repeated stream-readiness probes have shown workload-sensitive behavior on
 * loopback.
 */
static bool llam_windows_iocp_tcp_pollin_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_WINDOWS_IOCP_TCP_POLLIN");

        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return whether experimental UDP POLLIN IOCP probes are enabled.
 *
 * Datagram read readiness uses a one-byte overlapped @c WSARecvFrom(MSG_PEEK).
 * Public poll callers still get correct readiness through the immediate
 * platform-poll path; the native IOCP probe remains opt-in because hosted
 * Windows loopback runs have shown completion timing that can be less
 * predictable than ordinary level-triggered polling.
 */
static bool llam_windows_iocp_udp_pollin_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_WINDOWS_IOCP_UDP_POLLIN");

        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

bool llam_windows_iocp_poll_supported(llam_fd_t fd, short events) {
    int socket_type = 0;
    short unsupported = (short)(events & ~(POLLIN | POLLOUT | POLLHUP | POLLERR));
    bool wants_read = (events & POLLIN) != 0;
    bool wants_write = (events & POLLOUT) != 0;

    if (unsupported != 0 || (!wants_read && !wants_write) || (wants_read && wants_write)) {
        return false;
    }
    if (!llam_windows_socket_info(fd, NULL, &socket_type)) {
        return false;
    }
    if (socket_type == SOCK_STREAM) {
        return wants_write || (wants_read && llam_windows_iocp_tcp_pollin_enabled());
    }
    return socket_type == SOCK_DGRAM && wants_read && llam_windows_iocp_udp_pollin_enabled();
}
