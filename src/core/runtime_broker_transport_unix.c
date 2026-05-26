/**
 * @file src/core/runtime_broker_transport_unix.c
 * @brief Unix-domain broker local session helpers.
 *
 * @details
 * This layer owns listener/session orchestration for POSIX local control
 * sockets. The lower POSIX transport file remains focused on fd passing and
 * exact wire reads/writes, while this file keeps local broker lifetime policy
 * visible in one place.
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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool broker_socket_identity_matches(const struct stat *info,
                                           const llam_broker_socket_identity_t *identity) {
    return identity != NULL &&
           (uint64_t)info->st_dev == identity->dev &&
           (uint64_t)info->st_ino == identity->ino;
}

int llam_broker_capture_owned_socket(const char *path, llam_broker_socket_identity_t *out_identity) {
    struct stat path_info;

    if (out_identity != NULL) {
        /*
         * Socket identity is cleanup authority for chmod/unlink guards. Clear
         * it before every failure path so callers cannot accidentally reuse a
         * stale endpoint identity after a failed capture.
         */
        memset(out_identity, 0, sizeof(*out_identity));
    }
    if (LLAM_UNLIKELY(path == NULL || out_identity == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &path_info) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(S_ISLNK(path_info.st_mode))) {
        errno = ELOOP;
        return -1;
    }
    if (LLAM_UNLIKELY(!S_ISSOCK(path_info.st_mode))) {
        errno = EINVAL;
        return -1;
    }
    out_identity->dev = (uint64_t)path_info.st_dev;
    out_identity->ino = (uint64_t)path_info.st_ino;
    return 0;
}

int llam_broker_restrict_owned_socket(const char *path, const llam_broker_socket_identity_t *identity) {
    struct stat path_info;
    struct stat verify_info;

    if (LLAM_UNLIKELY(path == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &path_info) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(S_ISLNK(path_info.st_mode))) {
        errno = ELOOP;
        return -1;
    }
    if (LLAM_UNLIKELY(!S_ISSOCK(path_info.st_mode) ||
                      !broker_socket_identity_matches(&path_info, identity))) {
        errno = EINVAL;
        return -1;
    }
#if defined(AT_FDCWD) && defined(AT_SYMLINK_NOFOLLOW)
    /*
     * chmod(2) follows symlinks. Broker endpoints may be created in caller
     * supplied directories, so use the no-follow variant where the platform
     * exposes it and verify that the path still names the bound socket after
     * the mode change. AF_UNIX listener fds do not portably fstat to the
     * filesystem socket node, so the stable identity we can verify here is the
     * path inode before/after the no-follow chmod.
     */
    if (fchmodat(AT_FDCWD, path, S_IRUSR | S_IWUSR, AT_SYMLINK_NOFOLLOW) != 0) {
        return -1;
    }
#else
    /*
     * Without a no-follow chmod primitive, a path swap can turn a safe lstat
     * into chmod-on-attacker-path. Fail closed rather than weakening the broker
     * endpoint permission contract on older POSIX targets.
     */
    errno = ENOTSUP;
    return -1;
#endif
    if (lstat(path, &verify_info) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(!S_ISSOCK(verify_info.st_mode) ||
                      !broker_socket_identity_matches(&verify_info, identity) ||
                      path_info.st_dev != verify_info.st_dev ||
                      path_info.st_ino != verify_info.st_ino)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void llam_broker_unlink_owned_socket(const char *path, const llam_broker_socket_identity_t *identity) {
    struct stat path_info;

    if (path == NULL || identity == NULL) {
        return;
    }
    if (lstat(path, &path_info) != 0) {
        return;
    }
    if (S_ISSOCK(path_info.st_mode) &&
        broker_socket_identity_matches(&path_info, identity)) {
        (void)unlink(path);
    }
}

int llam_broker_serve_unix_once(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_local_n(broker, path, 1U);
}

int llam_broker_serve_local_n(llam_broker_t *broker, const char *path, size_t max_connections) {
    llam_broker_socket_identity_t listen_identity;
    int listen_fd = -1;
    int rc = -1;
    int last_session_errno = 0;
    size_t served = 0U;
    size_t successful = 0U;

    if (LLAM_UNLIKELY(broker == NULL || max_connections == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_listen_unix(path, &listen_fd) != 0) {
        return -1;
    }
    if (llam_broker_capture_owned_socket(path, &listen_identity) != 0) {
        int saved_errno = errno;

        close(listen_fd);
        errno = saved_errno;
        return -1;
    }
    while (served < max_connections) {
        int client_fd = -1;

        if (llam_broker_accept_one(listen_fd, &client_fd) != 0) {
            break;
        }
        /*
         * Each accepted socket is its own authority audience. STOP closes only
         * that session; the long-running broker process keeps accepting until
         * its configured accepted-session budget is reached or the listener
         * fails. A malformed client can only kill its own session, not the
         * trusted broker process that owns the runtime and capability keys.
         */
        rc = llam_broker_serve_fd(broker, client_fd);
        if (rc == 0) {
            successful++;
        } else {
            last_session_errno = errno != 0 ? errno : EIO;
        }
        close(client_fd);
        client_fd = -1;
        served++;
    }
    if (served == max_connections) {
        if (successful > 0U) {
            rc = 0;
        } else {
            rc = -1;
            errno = last_session_errno != 0 ? last_session_errno : EPIPE;
        }
    }
    llam_broker_unlink_owned_socket(path, &listen_identity);
    close(listen_fd);
    return rc;
}

int llam_broker_serve_local_once(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_unix_once(broker, path);
}

int llam_broker_serve_local(llam_broker_t *broker, const char *path) {
    return llam_broker_serve_local_n(broker, path, (size_t)-1);
}

typedef struct llam_broker_fd_transport {
    int fd;
} llam_broker_fd_transport_t;

static int llam_broker_request_fd_adapter(void *transport,
                                          const llam_broker_wire_request_t *request,
                                          llam_broker_wire_response_t *response) {
    llam_broker_fd_transport_t *state = (llam_broker_fd_transport_t *)transport;

    return llam_broker_request_fd(state->fd, request, response);
}

int llam_broker_client_self_test_unix(const char *path) {
    llam_broker_fd_transport_t transport;
    int fd = -1;
    int rc = -1;

    if (llam_broker_connect_unix(path, &fd) != 0) {
        return -1;
    }

    transport.fd = fd;
    rc = llam_broker_client_self_test_exchange(llam_broker_request_fd_adapter, &transport);
    close(fd);
    return rc;
}

int llam_broker_client_self_test_local(const char *path) {
    return llam_broker_client_self_test_unix(path);
}

#endif
