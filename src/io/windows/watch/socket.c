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

typedef BOOL(WINAPI *llam_windows_compare_object_handles_fn)(
    HANDLE,
    HANDLE);

static INIT_ONCE g_llam_windows_compare_handles_once =
    INIT_ONCE_STATIC_INIT;
static llam_windows_compare_object_handles_fn
    g_llam_windows_compare_handles;

static BOOL CALLBACK llam_windows_load_compare_handles(
    PINIT_ONCE once,
    PVOID parameter,
    PVOID *context) {
    HMODULE module;
    FARPROC symbol;

    (void)once;
    (void)parameter;
    (void)context;
    module = GetModuleHandleW(L"kernelbase.dll");
    if (module == NULL) {
        module = GetModuleHandleW(L"kernel32.dll");
    }
    symbol =
        module != NULL
            ? GetProcAddress(module, "CompareObjectHandles")
            : NULL;
    if (symbol != NULL &&
        sizeof(g_llam_windows_compare_handles) == sizeof(symbol)) {
        memcpy(&g_llam_windows_compare_handles,
               &symbol,
               sizeof(g_llam_windows_compare_handles));
    }
    return TRUE;
}

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

static void llam_windows_unlink_assoc_locked(
    llam_node_t *node,
    llam_windows_fd_assoc_t *target) {
    llam_windows_fd_assoc_t **link;

    if (node == NULL || target == NULL) {
        return;
    }
    link = (llam_windows_fd_assoc_t **)&node->windows_fd_assoc_head;
    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            target->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static bool llam_windows_assoc_authority_valid(
    const llam_windows_fd_assoc_t *assoc) {
    if (assoc == NULL) {
        return false;
    }
    if (assoc->is_socket) {
        return (SOCKET)assoc->authority != INVALID_SOCKET;
    }
    return (HANDLE)assoc->authority != NULL &&
           (HANDLE)assoc->authority != INVALID_HANDLE_VALUE;
}

void llam_windows_fd_assoc_destroy(
    llam_windows_fd_assoc_t *assoc) {
    if (assoc == NULL) {
        return;
    }
    if (llam_windows_assoc_authority_valid(assoc)) {
        if (assoc->is_socket) {
            (void)closesocket((SOCKET)assoc->authority);
        } else {
            (void)CloseHandle((HANDLE)assoc->authority);
        }
    }
    assoc->authority =
        assoc->is_socket
            ? (uintptr_t)INVALID_SOCKET
            : (uintptr_t)INVALID_HANDLE_VALUE;
    free(assoc);
}

static void llam_windows_cancel_assoc(
    const llam_windows_fd_assoc_t *assoc) {
    if (llam_windows_assoc_authority_valid(assoc)) {
        /*
         * Managed close and stale-generation replacement retire the stable
         * authority, not the caller's reusable numeric value. Cancel every
         * operation issued through that authority so its final IOCP packet can
         * release the last association pin.
         */
        (void)CancelIoEx((HANDLE)assoc->authority, NULL);
    }
}

static int llam_windows_same_kernel_object(
    uintptr_t current,
    const llam_windows_fd_assoc_t *assoc) {
    if (!InitOnceExecuteOnce(
            &g_llam_windows_compare_handles_once,
            llam_windows_load_compare_handles,
            NULL,
            NULL) ||
        g_llam_windows_compare_handles == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    SetLastError(ERROR_SUCCESS);
    if (g_llam_windows_compare_handles(
            (HANDLE)current,
            (HANDLE)assoc->authority)) {
        return 1;
    }
    /*
     * FALSE is the kernel-authoritative stale/invalid answer. The subsequent
     * duplication step distinguishes a valid replacement from a closed value;
     * no numeric cache hit is trusted after this point.
     */
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static llam_windows_socket_authority_create_hook_t
    g_llam_windows_socket_authority_create_hook;

void llam_windows_test_set_socket_authority_create_hook(
    llam_windows_socket_authority_create_hook_t hook) {
    g_llam_windows_socket_authority_create_hook = hook;
}
#endif

static SOCKET llam_windows_create_socket_authority(
    int address_family,
    int socket_type,
    int protocol,
    LPWSAPROTOCOL_INFOW protocol_info,
    GROUP group,
    DWORD flags) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (g_llam_windows_socket_authority_create_hook != NULL) {
        return g_llam_windows_socket_authority_create_hook(
            address_family,
            socket_type,
            protocol,
            protocol_info,
            group,
            flags);
    }
#endif
    return WSASocketW(
        address_family,
        socket_type,
        protocol,
        protocol_info,
        group,
        flags);
}

static int llam_windows_duplicate_socket_authority(
    SOCKET socket_fd,
    uintptr_t *authority_out) {
    WSAPROTOCOL_INFOW protocol_info;
    SOCKET duplicate;
    DWORD error_code;

    if (authority_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *authority_out = (uintptr_t)INVALID_SOCKET;
    memset(&protocol_info, 0, sizeof(protocol_info));
    if (WSADuplicateSocketW(
            socket_fd,
            GetCurrentProcessId(),
            &protocol_info) != 0) {
        errno =
            llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    duplicate = llam_windows_create_socket_authority(
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        &protocol_info,
        0U,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (duplicate == INVALID_SOCKET) {
        errno =
            llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    /*
     * Windows Server 2022 can ignore WSA_FLAG_NO_HANDLE_INHERIT when
     * WSASocket consumes WSADuplicateSocket protocol information. Keep the
     * creation flag for providers that honor the atomic contract, then enforce
     * the actual handle postcondition before publishing the retained
     * authority. Fail closed rather than retain a socket that a child process
     * could inherit.
     */
    if (!SetHandleInformation(
            (HANDLE)(uintptr_t)duplicate,
            HANDLE_FLAG_INHERIT,
            0U)) {
        error_code = GetLastError();
        (void)closesocket(duplicate);
        errno =
            llam_windows_system_error_to_errno(error_code);
        return -1;
    }
    *authority_out = (uintptr_t)duplicate;
    return 0;
}

static int llam_windows_duplicate_handle_authority(
    HANDLE handle,
    uintptr_t *authority_out) {
    HANDLE duplicate = INVALID_HANDLE_VALUE;

    if (authority_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *authority_out = (uintptr_t)INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            handle,
            GetCurrentProcess(),
            &duplicate,
            0U,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        errno =
            llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    *authority_out = (uintptr_t)duplicate;
    return 0;
}

static unsigned llam_windows_try_skip_completion_on_success(llam_node_t *node, HANDLE handle) {
    /*
     * Never suppress the normal IOCP packet. The submit path used to decide
     * whether to complete an immediately successful overlapped request inline
     * by looking up mutable association metadata after the OS call returned.
     * A concurrent close could erase that metadata in between, leaving neither
     * an inline completion nor a kernel packet. Keeping skip mode disabled
     * makes the kernel completion packet the single authoritative path across
     * association teardown and handle reuse.
     */
    (void)node;
    (void)handle;
    return 0U;
}

static int llam_windows_associate_object(
    llam_node_t *node,
    uintptr_t key,
    bool is_socket) {
    llam_windows_fd_assoc_t *assoc = NULL;
    llam_windows_fd_assoc_t *existing;
    llam_windows_fd_assoc_t *retired = NULL;
    HANDLE handle;
    DWORD error_code;
    int same_object;
    int saved_errno = 0;

    if (node == NULL || node->windows_iocp_handle == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Association metadata is keyed by raw HANDLE/SOCKET values and is touched
     * by public close paths as well as IOCP submit paths.  Keep it off
     * watch_lock so slow Windows association calls cannot block the worker from
     * draining watch tables or control packets.
     */
    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->windows_assoc_lock);
    existing = llam_windows_find_assoc_locked(node, key);
    if (existing != NULL) {
        same_object =
            existing->is_socket == is_socket
                ? llam_windows_same_kernel_object(key, existing)
                : 0;
        if (same_object < 0) {
            saved_errno = errno;
            goto fail_locked;
        }
        if (same_object > 0 && !existing->closing) {
            pthread_mutex_unlock(&node->windows_assoc_lock);
            llam_fd_watch_lifecycle_unlock();
            return 0;
        }

        /*
         * Raw closesocket()/CloseHandle bypasses LLAM's close boundary, and
         * Windows can immediately recycle the numeric value. Retire the old
         * generation before attempting to publish one for the replacement.
         * In-flight operations keep their authority alive until their canceled
         * completion packets release the final pins.
         */
        llam_windows_unlink_assoc_locked(node, existing);
        existing->closing = true;
        llam_windows_cancel_assoc(existing);
        if (existing->inflight_ops == 0U) {
            retired = existing;
        }
    }
    if (node->windows_assoc_generation == UINT64_MAX) {
        saved_errno = EOVERFLOW;
        goto fail_locked;
    }
    assoc = calloc(1, sizeof(*assoc));
    if (assoc == NULL) {
        saved_errno = ENOMEM;
        goto fail_locked;
    }
    assoc->fd = (llam_fd_t)key;
    assoc->authority =
        is_socket
            ? (uintptr_t)INVALID_SOCKET
            : (uintptr_t)INVALID_HANDLE_VALUE;
    assoc->owner_node = node;
    assoc->is_socket = is_socket;
    if ((is_socket
             ? llam_windows_duplicate_socket_authority(
                   (SOCKET)key, &assoc->authority)
             : llam_windows_duplicate_handle_authority(
                   (HANDLE)key, &assoc->authority)) != 0) {
        saved_errno = errno;
        goto fail_locked;
    }

    /*
     * Bind the retained authority rather than the caller-owned numeric value.
     * All OVERLAPPED submissions use this same authority, so a concurrent raw
     * close/reuse cannot retarget an operation after object validation.
     */
    handle = CreateIoCompletionPort(
        (HANDLE)assoc->authority,
        (HANDLE)node->windows_iocp_handle,
        (ULONG_PTR)(uintptr_t)node,
        0U);
    if (handle == NULL ||
        handle != (HANDLE)node->windows_iocp_handle) {
        error_code = GetLastError();
        saved_errno =
            handle == NULL
                ? llam_windows_system_error_to_errno(error_code)
                : EXDEV;
        goto fail_locked;
    }
    assoc->generation =
        ++node->windows_assoc_generation;
    assoc->skip_completion_on_success =
        llam_windows_try_skip_completion_on_success(
            node, (HANDLE)assoc->authority);
    assoc->next = node->windows_fd_assoc_head;
    node->windows_fd_assoc_head = assoc;
    pthread_mutex_unlock(&node->windows_assoc_lock);
    llam_fd_watch_lifecycle_unlock();
    llam_windows_fd_assoc_destroy(retired);
    return 0;

fail_locked:
    pthread_mutex_unlock(&node->windows_assoc_lock);
    llam_fd_watch_lifecycle_unlock();
    llam_windows_fd_assoc_destroy(assoc);
    llam_windows_fd_assoc_destroy(retired);
    errno = saved_errno != 0 ? saved_errno : EIO;
    return -1;
}

int llam_windows_associate_fd(llam_node_t *node, llam_fd_t fd) {
    if (LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }
    return llam_windows_associate_object(
        node, (uintptr_t)fd, true);
}

int llam_windows_associate_handle(
    llam_node_t *node,
    llam_handle_t raw_handle) {
    if (LLAM_HANDLE_IS_INVALID(raw_handle)) {
        errno = EINVAL;
        return -1;
    }
    return llam_windows_associate_object(
        node, (uintptr_t)raw_handle, false);
}

llam_windows_fd_assoc_t *llam_windows_fd_assoc_pin(llam_node_t *node, uintptr_t key) {
    llam_windows_fd_assoc_t *assoc;

    if (node == NULL) {
        errno = EINVAL;
        return NULL;
    }
    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->windows_assoc_lock);
    assoc = llam_windows_find_assoc_locked(node, key);
    if (assoc == NULL || assoc->closing) {
        assoc = NULL;
        errno = EBADF;
    } else if (assoc->inflight_ops == UINT_MAX) {
        assoc = NULL;
        errno = EOVERFLOW;
    } else {
        assoc->inflight_ops += 1U;
    }
    pthread_mutex_unlock(&node->windows_assoc_lock);
    llam_fd_watch_lifecycle_unlock();
    return assoc;
}

void llam_windows_fd_assoc_unpin(llam_node_t *node, llam_windows_fd_assoc_t *assoc) {
    bool free_assoc = false;

    if (node == NULL || assoc == NULL) {
        return;
    }
    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->windows_assoc_lock);
    if (assoc->inflight_ops == 0U) {
        llam_record_fatal(node->runtime, EINVAL);
    } else {
        assoc->inflight_ops -= 1U;
        free_assoc = assoc->closing && assoc->inflight_ops == 0U;
    }
    pthread_mutex_unlock(&node->windows_assoc_lock);
    if (free_assoc) {
        llam_windows_fd_assoc_destroy(assoc);
    }
    llam_fd_watch_lifecycle_unlock();
}

void llam_windows_forget_fd_assoc(llam_runtime_t *rt, llam_fd_t fd) {
    uintptr_t key = (uintptr_t)fd;

    if (rt == NULL || rt->nodes == NULL) {
        return;
    }
    llam_fd_watch_lifecycle_lock();
    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        llam_node_t *node = &rt->nodes[i];
        llam_windows_fd_assoc_t *prev = NULL;
        llam_windows_fd_assoc_t *assoc;
        llam_windows_fd_assoc_t *retired = NULL;

        pthread_mutex_lock(&node->windows_assoc_lock);
        assoc = node->windows_fd_assoc_head;
        while (assoc != NULL) {
            if ((uintptr_t)assoc->fd == key) {
                if (prev != NULL) {
                    prev->next = assoc->next;
                } else {
                    node->windows_fd_assoc_head = assoc->next;
                }
                assoc->next = NULL;
                assoc->closing = true;
                llam_windows_cancel_assoc(assoc);
                if (assoc->inflight_ops == 0U) {
                    retired = assoc;
                }
                break;
            }
            prev = assoc;
            assoc = assoc->next;
        }
        pthread_mutex_unlock(&node->windows_assoc_lock);
        llam_windows_fd_assoc_destroy(retired);
    }
    llam_fd_watch_lifecycle_unlock();
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
