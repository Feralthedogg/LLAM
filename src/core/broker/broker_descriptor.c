/**
 * @file src/core/broker/broker_descriptor.c
 * @brief Broker-owned descriptor and HANDLE capability data plane.
 *
 * @details
 * Descriptor authority is broker-owned. Clients may request reads and writes
 * with MAC-protected tokens, but every operation first validates the live slot
 * and duplicates the underlying descriptor/HANDLE while the broker table is
 * locked. The duplicate is then used outside the lock so close/reuse races
 * cannot retarget I/O at a different broker slot.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "runtime_internal.h"
#include "runtime_broker.h"

#include <limits.h>
#include <string.h>
#if !LLAM_PLATFORM_WINDOWS
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#define LLAM_BROKER_DESCRIPTOR_IO_TIMEOUT_MS 250

static int llam_broker_descriptor_set_cloexec(llam_handle_t handle) {
#if LLAM_PLATFORM_WINDOWS
    if (LLAM_UNLIKELY(LLAM_HANDLE_IS_INVALID(handle))) {
        errno = EINVAL;
        return -1;
    }
    if (!SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0U)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    return 0;
#else
    int fd = (int)handle;
    int flags;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#endif
}

static bool llam_broker_descriptor_handle_invalid(llam_handle_t handle) {
#if LLAM_PLATFORM_WINDOWS
    return LLAM_HANDLE_IS_INVALID(handle);
#else
    return handle < 0;
#endif
}

#if LLAM_PLATFORM_WINDOWS
/*
 * Keep the ntdll query ABI local instead of including winternl.h after the
 * security headers pulled in by runtime_internal.h.  Recent Windows SDKs
 * declare STRING/UNICODE_STRING in both header families with incompatible
 * typedef forms, and FILE_MODE_INFORMATION is not part of the stable Win32
 * surface.  These layouts and numeric values are the documented NT ABI used
 * by NtQueryInformationFile.
 */
typedef struct llam_broker_io_status_block {
    union {
        LONG status;
        PVOID pointer;
    } value;
    ULONG_PTR information;
} llam_broker_io_status_block_t;

typedef struct llam_broker_file_mode_information {
    ULONG mode;
} llam_broker_file_mode_information_t;

#define LLAM_BROKER_FILE_MODE_INFORMATION_CLASS 16UL
#define LLAM_BROKER_FILE_SYNCHRONOUS_IO_ALERT 0x10UL
#define LLAM_BROKER_FILE_SYNCHRONOUS_IO_NONALERT 0x20UL

typedef LONG(NTAPI *llam_broker_nt_query_information_file_fn)(
    HANDLE,
    llam_broker_io_status_block_t *,
    PVOID,
    ULONG,
    ULONG);

static int llam_broker_descriptor_require_overlapped(HANDLE handle) {
    llam_broker_file_mode_information_t mode;
    llam_broker_io_status_block_t io_status;
    llam_broker_nt_query_information_file_fn query_information_file = NULL;
    DWORD pipe_flags;
    HMODULE ntdll;
    FARPROC symbol;
    LONG status;

    if (LLAM_UNLIKELY(handle == NULL || handle == INVALID_HANDLE_VALUE)) {
        errno = EINVAL;
        return -1;
    }
    if (!GetNamedPipeInfo(handle, &pipe_flags, NULL, NULL, NULL)) {
        errno = ENOTSUP;
        return -1;
    }
    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    symbol = GetProcAddress(ntdll, "NtQueryInformationFile");
    if (symbol == NULL || sizeof(query_information_file) != sizeof(symbol)) {
        errno = ENOTSUP;
        return -1;
    }
    memcpy(&query_information_file, &symbol, sizeof(query_information_file));
    memset(&mode, 0, sizeof(mode));
    memset(&io_status, 0, sizeof(io_status));
    status = query_information_file(handle,
                                    &io_status,
                                    &mode,
                                    (ULONG)sizeof(mode),
                                    LLAM_BROKER_FILE_MODE_INFORMATION_CLASS);
    if (status < 0 ||
        (mode.mode & (LLAM_BROKER_FILE_SYNCHRONOUS_IO_ALERT |
                      LLAM_BROKER_FILE_SYNCHRONOUS_IO_NONALERT)) != 0U) {
        /*
         * Passing OVERLAPPED to a synchronous handle does not make the initial
         * ReadFile/WriteFile call asynchronous.  A client-selected pipe could
         * therefore block before the broker reaches its timeout.  Only issue
         * I/O after the kernel confirms that the file object was opened for
         * asynchronous operation; unqueryable handle families fail closed.
         */
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static ssize_t llam_broker_descriptor_overlapped_rw(HANDLE handle,
                                                    void *buffer,
                                                    size_t length,
                                                    bool write_op) {
    OVERLAPPED overlapped;
    HANDLE event;
    DWORD transferred = 0U;
    BOOL ok;

    if (LLAM_UNLIKELY(handle == NULL ||
                      handle == INVALID_HANDLE_VALUE ||
                      buffer == NULL ||
                      length == 0U ||
                      length > (size_t)ULONG_MAX)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_descriptor_require_overlapped(handle) != 0) {
        return -1;
    }
    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = event;
    ok = write_op
        ? WriteFile(handle, buffer, (DWORD)length, &transferred, &overlapped)
        : ReadFile(handle, buffer, (DWORD)length, &transferred, &overlapped);
    if (!ok) {
        DWORD error_code = GetLastError();

        if (error_code != ERROR_IO_PENDING) {
            CloseHandle(event);
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        error_code = WaitForSingleObject(event, LLAM_BROKER_DESCRIPTOR_IO_TIMEOUT_MS);
        if (error_code != WAIT_OBJECT_0) {
            (void)CancelIoEx(handle, &overlapped);
            (void)WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
            errno = error_code == WAIT_TIMEOUT ? ETIMEDOUT : llam_windows_system_error_to_errno(GetLastError());
            return -1;
        }
        if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
            error_code = GetLastError();
            CloseHandle(event);
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
    }
    CloseHandle(event);
    return (ssize_t)transferred;
}
#endif

#if !LLAM_PLATFORM_WINDOWS
static int llam_broker_descriptor_require_socket(int fd) {
    int socket_type;
    socklen_t socket_type_len = (socklen_t)sizeof(socket_type);

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_len) != 0) {
        if (errno == ENOTSOCK || errno == EINVAL) {
            errno = ENOTSUP;
        }
        return -1;
    }
    return 0;
}

static ssize_t llam_broker_descriptor_recv_nonblocking(int fd, void *data, size_t length) {
#if defined(MSG_DONTWAIT)
    return recv(fd, data, length, MSG_DONTWAIT);
#else
    (void)fd;
    (void)data;
    (void)length;
    errno = ENOTSUP;
    return -1;
#endif
}

static ssize_t llam_broker_descriptor_send_nonblocking(int fd, const void *data, size_t length) {
#if defined(MSG_DONTWAIT)
    int flags = MSG_DONTWAIT;

#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    return send(fd, data, length, flags);
#else
    sigset_t blocked;
    sigset_t old_mask;
    sigset_t pending;
    struct timespec zero_timeout;
    ssize_t result;
    int saved_errno;
    int mask_error;
    int pending_member;
    bool pending_before;

    if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGPIPE) != 0) {
        return -1;
    }
    mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);
    if (mask_error != 0) {
        errno = mask_error;
        return -1;
    }
    if (sigpending(&pending) != 0) {
        saved_errno = errno;
        (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        errno = saved_errno;
        return -1;
    }
    pending_member = sigismember(&pending, SIGPIPE);
    if (pending_member < 0) {
        saved_errno = errno;
        (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        errno = saved_errno;
        return -1;
    }
    pending_before = pending_member == 1;

    result = send(fd, data, length, flags);
    saved_errno = errno;

    /*
     * Darwin has no per-call MSG_NOSIGNAL. SO_NOSIGPIPE is not equivalent for
     * broker grants because it is mutable socket state shared with every client
     * duplicate: a grantor can clear the option between setsockopt() and send().
     * Block SIGPIPE only on this thread instead. If this call generated a new
     * pending signal, consume exactly that signal before restoring the caller's
     * mask; never consume a SIGPIPE that was already pending on entry.
     */
    if (!pending_before && result < 0 && saved_errno == EPIPE &&
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        int wait_result;

        memset(&zero_timeout, 0, sizeof(zero_timeout));
        do {
            wait_result = sigtimedwait(&blocked, NULL, &zero_timeout);
        } while (wait_result < 0 && errno == EINTR);
    }
    (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    errno = saved_errno;
    return result;
#endif
#else
    (void)fd;
    (void)data;
    (void)length;
    errno = ENOTSUP;
    return -1;
#endif
}

static int llam_broker_descriptor_wait_fd_ready(int fd, short events) {
    struct pollfd pfd;
    uint64_t start_ns;
    uint64_t deadline_ns;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;
    start_ns = llam_now_ns();
    deadline_ns = start_ns + ((uint64_t)LLAM_BROKER_DESCRIPTOR_IO_TIMEOUT_MS * UINT64_C(1000000));
    if (deadline_ns < start_ns) {
        deadline_ns = UINT64_MAX;
    }
    for (;;) {
        uint64_t now_ns = llam_now_ns();
        int timeout_ms = LLAM_BROKER_DESCRIPTOR_IO_TIMEOUT_MS;
        int rc;

        if (start_ns != 0U && now_ns != 0U) {
            uint64_t remaining_ns;
            uint64_t remaining_ms;

            if (now_ns >= deadline_ns) {
                errno = EAGAIN;
                return -1;
            }
            remaining_ns = deadline_ns - now_ns;
            remaining_ms = remaining_ns / UINT64_C(1000000);
            if ((remaining_ns % UINT64_C(1000000)) != 0U) {
                ++remaining_ms;
            }
            timeout_ms = remaining_ms > (uint64_t)INT_MAX ? INT_MAX : (int)remaining_ms;
            if (timeout_ms < 1) {
                timeout_ms = 1;
            }
        }
        rc = poll(&pfd, 1U, timeout_ms);

        if (rc > 0) {
            if ((pfd.revents & POLLNVAL) != 0) {
                errno = EBADF;
                return -1;
            }
            if ((pfd.revents & events) != 0 ||
                (pfd.revents & POLLERR) != 0 ||
                ((events & POLLIN) != 0 && (pfd.revents & POLLHUP) != 0)) {
                return 0;
            }
            errno = EAGAIN;
            return -1;
        }
        if (rc == 0) {
            errno = EAGAIN;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}
#endif

static void llam_broker_descriptor_close_owned_slot(llam_broker_descriptor_slot_t *slot) {
    if (slot == NULL || !slot->close_on_destroy) {
        return;
    }
#if LLAM_PLATFORM_WINDOWS
    if (!LLAM_HANDLE_IS_INVALID(slot->handle)) {
        (void)CloseHandle((HANDLE)slot->handle);
    }
#else
    if (slot->fd >= 0) {
        (void)close(slot->fd);
    }
#endif
}

static void llam_broker_descriptor_reset_slot(llam_broker_descriptor_slot_t *slot) {
    if (slot == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
#if LLAM_PLATFORM_WINDOWS
    slot->handle = LLAM_INVALID_HANDLE;
#else
    slot->fd = -1;
#endif
}

void llam_broker_clear_descriptors(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        /*
         * Ownership is represented by close_on_destroy plus a valid
         * descriptor/HANDLE. Destroy must still reclaim partially invalidated
         * slots whose active bit was cleared during an interrupted lifecycle.
         */
        llam_broker_descriptor_close_owned_slot(slot);
        llam_broker_descriptor_reset_slot(slot);
    }
}

void llam_broker_reclaim_subject_descriptors(llam_broker_t *broker, uint64_t subject_id) {
    size_t i;

    if (broker == NULL || subject_id == 0U || llam_broker_lock(broker) != 0) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        if (slot->subject_id == subject_id &&
            (slot->active || slot->close_on_destroy ||
             !llam_broker_descriptor_handle_invalid(
#if LLAM_PLATFORM_WINDOWS
                 slot->handle
#else
                 (llam_handle_t)slot->fd
#endif
             ))) {
            llam_broker_descriptor_close_owned_slot(slot);
            llam_broker_descriptor_reset_slot(slot);
        }
    }
    llam_broker_unlock(broker);
}

static llam_broker_descriptor_slot_t *llam_broker_find_descriptor_unlocked(
    llam_broker_t *broker,
    const llam_capability_token_t *token,
    uint64_t required_rights) {
    size_t i;

    if (llam_broker_validate_token_family_unlocked(broker,
                                                   token,
                                                   LLAM_BROKER_CAP_FAMILY_DESCRIPTOR,
                                                   required_rights) != 0) {
        return NULL;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        if (slot->active &&
            slot->id == token->slot &&
            slot->generation == token->generation) {
            if (LLAM_UNLIKELY((slot->rights & required_rights) != required_rights)) {
                errno = EACCES;
                return NULL;
            }
            return slot;
        }
    }
    errno = EACCES;
    return NULL;
}

int llam_broker_register_fd(llam_broker_t *broker,
                            int fd,
                            uint64_t rights,
                            bool close_on_destroy,
                            llam_capability_token_t *out_token) {
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)fd;
    (void)rights;
    (void)close_on_destroy;
    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    errno = ENOTSUP;
    return -1;
#else
    return llam_broker_register_handle(broker, (llam_handle_t)fd, rights, close_on_destroy, out_token);
#endif
}

int llam_broker_register_handle(llam_broker_t *broker,
                                llam_handle_t handle,
                                uint64_t rights,
                                bool close_on_destroy,
                                llam_capability_token_t *out_token) {
    llam_broker_descriptor_slot_t *slot = NULL;
    size_t i;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      llam_broker_descriptor_handle_invalid(handle) ||
                      rights == 0U ||
                      out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_DESCRIPTOR, rights) != 0) {
        return -1;
    }
    if (llam_broker_descriptor_set_cloexec(handle) != 0) {
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        if (!broker->descriptors[i].active) {
            slot = &broker->descriptors[i];
            break;
        }
    }
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = ENOSPC;
        return -1;
    }
    if (llam_broker_validate_next_object_id(broker->next_descriptor_id) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    /*
     * Free-list selection is based on active=false. If a previous internal
     * lifecycle was interrupted after clearing active, the slot can still own
     * descriptor authority. Reclaim it before overwriting the fd/HANDLE field.
     */
    llam_broker_descriptor_close_owned_slot(slot);
    llam_broker_descriptor_reset_slot(slot);

#if LLAM_PLATFORM_WINDOWS
    slot->handle = handle;
#else
    slot->fd = handle;
#endif
    slot->id = broker->next_descriptor_id++;
    slot->generation = 1U;
    slot->rights = rights;
    slot->subject_id = llam_broker_current_subject(broker);
    slot->close_on_destroy = close_on_destroy;
    slot->active = true;
    if (llam_broker_issue_object_cap_unlocked(broker,
                                              LLAM_BROKER_CAP_FAMILY_DESCRIPTOR,
                                              slot->id,
                                              slot->generation,
                                              rights,
                                              out_token) != 0) {
        memset(slot, 0, sizeof(*slot));
#if LLAM_PLATFORM_WINDOWS
        slot->handle = LLAM_INVALID_HANDLE;
#else
        slot->fd = -1;
#endif
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

static int llam_broker_duplicate_descriptor_unlocked(llam_broker_t *broker,
                                                     const llam_capability_token_t *token,
                                                     uint64_t required_rights,
                                                     llam_handle_t *out_handle) {
    llam_broker_descriptor_slot_t *slot;

    if (LLAM_UNLIKELY(out_handle == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_handle = LLAM_INVALID_HANDLE;
    slot = llam_broker_find_descriptor_unlocked(broker, token, required_rights);
    if (slot == NULL) {
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE duplicate = NULL;

        if (!DuplicateHandle(GetCurrentProcess(),
                             (HANDLE)slot->handle,
                             GetCurrentProcess(),
                             &duplicate,
                             0U,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            errno = llam_windows_system_error_to_errno(GetLastError());
            return -1;
        }
        *out_handle = (llam_handle_t)duplicate;
    }
#else
    {
        int duplicate = llam_broker_dup_cloexec_fd(slot->fd);

        if (duplicate < 0) {
            return -1;
        }
        *out_handle = (llam_handle_t)duplicate;
    }
#endif
    return 0;
}

ssize_t llam_broker_read_fd(llam_broker_t *broker,
                            const llam_capability_token_t *token,
                            void *out_data,
                            size_t length) {
    if (LLAM_UNLIKELY(out_data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)token;
    (void)out_data;
    (void)length;
    errno = ENOTSUP;
    return llam_broker_fail_clear_output_ssize(out_data, length, errno);
#else
    return llam_broker_read_handle(broker, token, out_data, length);
#endif
}

ssize_t llam_broker_write_fd(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             const void *data,
                             size_t length) {
    if (LLAM_UNLIKELY(data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)token;
    (void)data;
    (void)length;
    errno = ENOTSUP;
    return -1;
#else
    return llam_broker_write_handle(broker, token, data, length);
#endif
}

ssize_t llam_broker_read_handle(llam_broker_t *broker,
                                const llam_capability_token_t *token,
                                void *out_data,
                                size_t length) {
    llam_handle_t handle;
#if !LLAM_PLATFORM_WINDOWS
    ssize_t result;
#endif

    if (LLAM_UNLIKELY(out_data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return llam_broker_fail_clear_output_ssize(out_data, length, errno);
    }
    if (llam_broker_lock(broker) != 0) {
        int saved_errno = errno;

        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
    }
    if (llam_broker_duplicate_descriptor_unlocked(broker, token, LLAM_CAP_RIGHT_READ, &handle) != 0) {
        int saved_errno = errno;

        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
#if LLAM_PLATFORM_WINDOWS
    {
        ssize_t transferred;

        transferred = llam_broker_descriptor_overlapped_rw((HANDLE)handle, out_data, length, false);
        if (transferred < 0) {
            int saved_errno = errno;

            (void)CloseHandle((HANDLE)handle);
            return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
        }
        (void)CloseHandle((HANDLE)handle);
        return llam_broker_finish_read_clear_tail(out_data, length, transferred);
    }
#else
    {
        int fd = (int)handle;

        if (llam_broker_descriptor_require_socket(fd) != 0 ||
            llam_broker_descriptor_wait_fd_ready(fd, POLLIN) != 0) {
            int saved_errno = errno;

            (void)close(fd);
            return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
        }
        result = llam_broker_descriptor_recv_nonblocking(fd, out_data, length);
    }
    {
        int saved_errno = errno;

        (void)close((int)handle);
        errno = saved_errno;
    }
    return llam_broker_finish_read_clear_tail(out_data, length, result);
#endif
}

ssize_t llam_broker_write_handle(llam_broker_t *broker,
                                 const llam_capability_token_t *token,
                                 const void *data,
                                 size_t length) {
    llam_handle_t handle;
#if !LLAM_PLATFORM_WINDOWS
    ssize_t result;
#endif

    if (LLAM_UNLIKELY(data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (llam_broker_duplicate_descriptor_unlocked(broker, token, LLAM_CAP_RIGHT_WRITE, &handle) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
#if LLAM_PLATFORM_WINDOWS
    {
        ssize_t transferred;

        transferred = llam_broker_descriptor_overlapped_rw((HANDLE)handle, (void *)data, length, true);
        if (transferred < 0) {
            int saved_errno = errno;

            (void)CloseHandle((HANDLE)handle);
            errno = saved_errno;
            return -1;
        }
        (void)CloseHandle((HANDLE)handle);
        return transferred;
    }
#else
    {
        int fd = (int)handle;

        if (llam_broker_descriptor_require_socket(fd) != 0 ||
            llam_broker_descriptor_wait_fd_ready(fd, POLLOUT) != 0) {
            int saved_errno = errno;

            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
        result = llam_broker_descriptor_send_nonblocking(fd, data, length);
    }
    {
        int saved_errno = errno;

        (void)close((int)handle);
        errno = saved_errno;
    }
    return result;
#endif
}
