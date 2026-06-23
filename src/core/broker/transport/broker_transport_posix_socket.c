/**
 * @file src/core/broker/transport/broker_transport_posix_socket.c
 * @brief POSIX Unix-domain broker socket lifecycle helpers.
 *
 * @details
 * POSIX broker transports use Unix-domain sockets for local control messages
 * and SCM_RIGHTS fd passing for descriptor authority. This file owns endpoint
 * creation, connection, accept, close, and CLOEXEC setup; framing and request
 * serving stay in broker_transport_posix.c.
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
#include "runtime_broker.h"

#if !LLAM_PLATFORM_WINDOWS

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <fcntl.h>
#include <limits.h>
#include <string.h>

int llam_broker_set_cloexec_fd(int fd) {
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
}

int llam_broker_dup_cloexec_fd(int fd) {
    int duplicate;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
#if defined(F_DUPFD_CLOEXEC)
    /*
     * Prefer an atomic close-on-exec duplicate.  The fallback exists for older
     * kernels/libcs that expose the constant but reject it at runtime.
     */
    duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate >= 0) {
        return duplicate;
    }
    if (errno != EINVAL) {
        return -1;
    }
#endif
    duplicate = dup(fd);
    if (duplicate < 0) {
        return -1;
    }
    if (llam_broker_set_cloexec_fd(duplicate) != 0) {
        int saved_errno = errno;

        close(duplicate);
        errno = saved_errno;
        return -1;
    }
    return duplicate;
}

static int llam_broker_socket_cloexec(int domain, int type, int protocol) {
    int fd;

#if defined(SOCK_CLOEXEC)
    /*
     * SOCK_CLOEXEC removes the fork/exec inheritance window between socket()
     * and fcntl(F_SETFD).  Some older targets define the flag but reject it for
     * a socket family, so fall back only for that compatibility case.
     */
    fd = socket(domain, type | SOCK_CLOEXEC, protocol);
    if (fd >= 0) {
        return fd;
    }
    if (errno != EINVAL && errno != EPROTONOSUPPORT) {
        return -1;
    }
#endif
    fd = socket(domain, type, protocol);
    if (fd < 0) {
        return -1;
    }
    if (llam_broker_set_cloexec_fd(fd) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int llam_broker_open_unix_endpoint(const char *path, struct sockaddr_un *addr, int *out_fd) {
    size_t path_len;
    int fd;

    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (LLAM_UNLIKELY(path == NULL || addr == NULL || out_fd == NULL)) {
        errno = EINVAL;
        return -1;
    }
    path_len = strlen(path);
    if (LLAM_UNLIKELY(path_len == 0U || path_len >= sizeof(addr->sun_path))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = llam_broker_socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1U);
    *out_fd = fd;
    return 0;
}

static int llam_broker_parent_dir_private(const char *path) {
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *slash;
    size_t len;
    struct stat st;

    if (LLAM_UNLIKELY(path == NULL || path[0] == '\0')) {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        memcpy(parent, ".", 2U);
    } else if (slash == path) {
        memcpy(parent, "/", 2U);
    } else {
        len = (size_t)(slash - path);
        if (LLAM_UNLIKELY(len == 0U || len >= sizeof(parent))) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(parent, path, len);
        parent[len] = '\0';
    }
    if (stat(parent, &st) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(!S_ISDIR(st.st_mode))) {
        errno = ENOTDIR;
        return -1;
    }
    if (LLAM_UNLIKELY((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int llam_broker_listen_unix(const char *path, int *out_fd) {
    struct sockaddr_un addr;
    llam_broker_socket_identity_t identity;
    struct stat existing;
    int fd;
    bool identity_valid = false;

    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (llam_broker_parent_dir_private(path) != 0) {
        return -1;
    }
    if (llam_broker_open_unix_endpoint(path, &addr, &fd) != 0) {
        return -1;
    }
    /*
     * Do not unlink a client-controlled path during listen. Even "unlink only
     * sockets" has a time-of-check/time-of-use gap in writable directories; the
     * caller must choose a private directory or clean stale endpoints itself.
     */
    if (lstat(path, &existing) == 0) {
        (void)existing;
        close(fd);
        errno = EEXIST;
        return -1;
    } else if (errno != ENOENT) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        llam_broker_capture_owned_socket(path, &identity) != 0 ||
        (identity_valid = true, llam_broker_restrict_owned_socket(path, &identity) != 0) ||
        listen(fd, 16) != 0) {
        int saved_errno = errno;

        if (identity_valid) {
            llam_broker_unlink_owned_socket(path, &identity);
        }
        close(fd);
        errno = saved_errno;
        return -1;
    }
    *out_fd = fd;
    return 0;
}

int llam_broker_connect_unix(const char *path, int *out_fd) {
    struct sockaddr_un addr;
    int fd;

    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (llam_broker_open_unix_endpoint(path, &addr, &fd) != 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    *out_fd = fd;
    return 0;
}

int llam_broker_accept_one(int listen_fd, int *out_fd) {
    int fd;

    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (LLAM_UNLIKELY(listen_fd < 0 || out_fd == NULL)) {
        errno = EINVAL;
        return -1;
    }
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    /*
     * accept()+fcntl(FD_CLOEXEC) leaves a narrow fork/exec inheritance window
     * in multi-threaded embedders.  Linux exposes an atomic close-on-exec accept
     * path; fall back only when the kernel/libc rejects the flag path.
     */
    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        *out_fd = fd;
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#endif
    do {
        fd = accept(listen_fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return -1;
    }
    if (llam_broker_set_cloexec_fd(fd) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    *out_fd = fd;
    return 0;
}

void llam_broker_close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int llam_broker_listen_pipe(const char *name, llam_handle_t *out_handle) {
    (void)name;
    if (out_handle != NULL) {
        *out_handle = LLAM_INVALID_HANDLE;
    }
    errno = ENOTSUP;
    return -1;
}

int llam_broker_listen_pipe_instance(const char *name, bool first_instance, llam_handle_t *out_handle) {
    (void)first_instance;
    return llam_broker_listen_pipe(name, out_handle);
}

int llam_broker_connect_pipe(const char *name, llam_handle_t *out_handle) {
    (void)name;
    if (out_handle != NULL) {
        *out_handle = LLAM_INVALID_HANDLE;
    }
    errno = ENOTSUP;
    return -1;
}

void llam_broker_close_handle(llam_handle_t handle) {
    llam_broker_close_fd((int)handle);
}

#endif
