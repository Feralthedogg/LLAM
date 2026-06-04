/**
 * @file src/io/api/datagram.c
 * @brief Public datagram send/receive wrappers.
 *
 * @details
 * Datagram APIs use the same user-facing model as the stream APIs: unmanaged
 * callers delegate to platform socket calls, while managed tasks park on
 * readiness before issuing recvfrom/sendto so a blocking UDP socket cannot pin
 * a scheduler worker. Backend-specific recvmsg/sendmsg submission can be added
 * later without changing this public contract.
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

#include <limits.h>
#if LLAM_PLATFORM_POSIX
#include <poll.h>
#endif

#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif

static int llam_datagram_validate_addr_pair(const struct sockaddr *addr, const socklen_t *addrlen) {
    if ((addr == NULL) != (addrlen == NULL)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static ssize_t llam_platform_recvfrom_fd(llam_fd_t fd,
                                         void *buf,
                                         size_t count,
                                         int flags,
                                         struct sockaddr *src_addr,
                                         socklen_t *addrlen) {
    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_datagram_validate_addr_pair(src_addr, addrlen) != 0) {
        return -1;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (count > (size_t)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    {
        int native_len = addrlen != NULL ? (int)*addrlen : 0;
        int rc = recvfrom(fd,
                          (char *)buf,
                          (int)count,
                          flags,
                          src_addr,
                          addrlen != NULL ? &native_len : NULL);

        if (rc < 0) {
            errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
            return -1;
        }
        if (addrlen != NULL) {
            *addrlen = (socklen_t)native_len;
        }
        return (ssize_t)rc;
    }
#else
    return recvfrom(fd, buf, count, flags, src_addr, addrlen);
#endif
}

static ssize_t llam_platform_sendto_fd(llam_fd_t fd,
                                       const void *buf,
                                       size_t count,
                                       int flags,
                                       const struct sockaddr *dst_addr,
                                       socklen_t addrlen) {
    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (dst_addr == NULL && addrlen != 0U) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (count > (size_t)INT_MAX || addrlen > (socklen_t)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    {
        int rc = dst_addr != NULL
                     ? sendto(fd, (const char *)buf, (int)count, flags, dst_addr, (int)addrlen)
                     : send(fd, (const char *)buf, (int)count, flags);

        if (rc < 0) {
            errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
            return -1;
        }
        return (ssize_t)rc;
    }
#else
    if (dst_addr != NULL) {
        return sendto(fd, buf, count, flags, dst_addr, addrlen);
    }
    return send(fd, buf, count, flags);
#endif
}

ssize_t llam_recvfrom(llam_fd_t fd,
                      void *buf,
                      size_t count,
                      int flags,
                      struct sockaddr *src_addr,
                      socklen_t *addrlen) {
    short revents = 0;

    if (llam_datagram_validate_addr_pair(src_addr, addrlen) != 0) {
        return -1;
    }
    llam_task_safepoint();
    if (g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        return llam_platform_recvfrom_fd(fd, buf, count, flags, src_addr, addrlen);
    }
    for (;;) {
        ssize_t nread;

        if (llam_poll_fd(fd, POLLIN, -1, &revents) < 0) {
            return -1;
        }
        nread = llam_platform_recvfrom_fd(fd, buf, count, flags, src_addr, addrlen);
        if (nread >= 0) {
            return nread;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
}

ssize_t llam_sendto(llam_fd_t fd,
                    const void *buf,
                    size_t count,
                    int flags,
                    const struct sockaddr *dst_addr,
                    socklen_t addrlen) {
    short revents = 0;

    llam_task_safepoint();
    if (g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        return llam_platform_sendto_fd(fd, buf, count, flags, dst_addr, addrlen);
    }
    for (;;) {
        ssize_t nwritten;

        if (llam_poll_fd(fd, POLLOUT, -1, &revents) < 0) {
            return -1;
        }
        nwritten = llam_platform_sendto_fd(fd, buf, count, flags, dst_addr, addrlen);
        if (nwritten >= 0) {
            llam_maybe_handoff_after_socket_write(fd, (size_t)nwritten, true);
            return nwritten;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
}

ssize_t llam_recvfrom_owned(llam_fd_t fd,
                            size_t max_count,
                            int flags,
                            struct sockaddr *src_addr,
                            socklen_t *addrlen,
                            llam_io_buffer_t **out) {
    llam_io_buffer_t *buffer;
    llam_io_buffer_t *public_handle;
    ssize_t nread;
    int saved_errno;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (llam_datagram_validate_addr_pair(src_addr, addrlen) != 0) {
        return -1;
    }
    if (max_count == 0U) {
        return 0;
    }
    buffer = llam_io_buffer_alloc_detached(max_count, 0U, 0U);
    if (buffer == NULL) {
        return -1;
    }
    nread = llam_recvfrom(fd, buffer->data, max_count, flags, src_addr, addrlen);
    if (nread <= 0) {
        saved_errno = errno;
        llam_io_buffer_release_raw(buffer);
        if (nread < 0) {
            errno = saved_errno;
        }
        return nread;
    }
    buffer->size = (size_t)nread;
    public_handle = llam_io_buffer_public_handle(buffer);
    if (public_handle == NULL) {
        llam_io_buffer_release_raw(buffer);
        errno = ENOMEM;
        return -1;
    }
    *out = public_handle;
    return nread;
}
