/**
 * @file src/core/broker/transport/broker_transport_posix_message.c
 * @brief POSIX broker wire framing and SCM_RIGHTS message helpers.
 *
 * @details
 * This file owns exact request/response reads, descriptor-bearing messages,
 * and synchronous client request helpers for POSIX broker transports. The
 * serving loop keeps subject lifetime state in broker_transport_posix.c.
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
#include <unistd.h>

#include <string.h>

#if !defined(MSG_NOSIGNAL)
static void llam_broker_disable_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int saved_errno = errno;
    int one = 1;

    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));
    errno = saved_errno;
#else
    (void)fd;
#endif
}
#endif

static ssize_t llam_broker_send_transport(int fd, const void *data, size_t len) {
#if defined(MSG_NOSIGNAL)
    return send(fd, data, len, MSG_NOSIGNAL);
#else
    llam_broker_disable_sigpipe(fd);
    return send(fd, data, len, 0);
#endif
}

static ssize_t llam_broker_sendmsg_transport(int fd, const struct msghdr *msg) {
#if defined(MSG_NOSIGNAL)
    return sendmsg(fd, msg, MSG_NOSIGNAL);
#else
    llam_broker_disable_sigpipe(fd);
    return sendmsg(fd, msg, 0);
#endif
}

static void llam_broker_close_received_fds(int fds[4], size_t *count) {
    size_t i;

    if (fds == NULL || count == NULL) {
        return;
    }
    for (i = 0U; i < *count; ++i) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
    *count = 0U;
}

static int llam_broker_read_message_fail(void *message,
                                         size_t message_len,
                                         int fds[4],
                                         size_t *count,
                                         int error_code) {
    /*
     * Wire reads are an authority boundary. If a peer closes mid-message or
     * attaches malformed ancillary data, do not leave attacker-controlled
     * partial fields in caller storage for buggy callers to accidentally reuse.
     */
    llam_broker_close_received_fds(fds, count);
    if (message != NULL && message_len > 0U) {
        memset(message, 0, message_len);
    }
    errno = error_code;
    return -1;
}

static int llam_broker_write_remainder(int fd, const unsigned char *cursor, size_t len) {
    size_t done = 0U;

    while (done < len) {
        ssize_t nwritten = llam_broker_send_transport(fd, cursor + done, len - done);

        if (nwritten > 0) {
            done += (size_t)nwritten;
            continue;
        }
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int llam_broker_read_message_fd(int fd, void *message, size_t message_len, int *out_descriptor_fd) {
    unsigned char *cursor = (unsigned char *)message;
    size_t done = 0U;
    int descriptor_fds[4] = {-1, -1, -1, -1};
    size_t descriptor_count = 0U;

    if (out_descriptor_fd != NULL) {
        *out_descriptor_fd = -1;
    }
    if (LLAM_UNLIKELY(fd < 0 || message == NULL || message_len == 0U || out_descriptor_fd == NULL)) {
        return llam_broker_read_message_fail(message,
                                             message_len,
                                             descriptor_fds,
                                             &descriptor_count,
                                             EINVAL);
    }
    while (done < message_len) {
        unsigned char control[CMSG_SPACE(sizeof(int) * 4U)];
        struct iovec iov;
        struct msghdr msg;
        ssize_t nread;
        struct cmsghdr *cmsg;

        memset(control, 0, sizeof(control));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = cursor + done;
        iov.iov_len = message_len - done;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1U;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        /*
         * MSG_CMSG_CLOEXEC atomically marks SCM_RIGHTS descriptors close-on-exec
         * on platforms that support it.  We still call llam_broker_set_cloexec_fd
         * below because not every supported POSIX target exposes this flag.
         */
#if defined(MSG_CMSG_CLOEXEC)
        nread = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
#else
        nread = recvmsg(fd, &msg, 0);
#endif
        if (nread > 0) {
            if ((msg.msg_flags & MSG_CTRUNC) != 0) {
                return llam_broker_read_message_fail(message,
                                                     message_len,
                                                     descriptor_fds,
                                                     &descriptor_count,
                                                     EINVAL);
            }
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                    size_t payload = cmsg->cmsg_len >= CMSG_LEN(0)
                        ? (size_t)(cmsg->cmsg_len - CMSG_LEN(0))
                        : 0U;
                    size_t count = payload / sizeof(int);
                    const unsigned char *fds = (const unsigned char *)CMSG_DATA(cmsg);
                    size_t i;

                    if (LLAM_UNLIKELY((payload % sizeof(int)) != 0U)) {
                        return llam_broker_read_message_fail(message,
                                                             message_len,
                                                             descriptor_fds,
                                                             &descriptor_count,
                                                             EINVAL);
                    }
                    for (i = 0U; i < count; ++i) {
                        int received_fd;

                        memcpy(&received_fd, fds + (i * sizeof(int)), sizeof(received_fd));
                        if (llam_broker_set_cloexec_fd(received_fd) != 0) {
                            int saved_errno = errno;

                            close(received_fd);
                            return llam_broker_read_message_fail(message,
                                                                 message_len,
                                                                 descriptor_fds,
                                                                 &descriptor_count,
                                                                 saved_errno);
                        }
                        if (descriptor_count >= 4U) {
                            close(received_fd);
                            return llam_broker_read_message_fail(message,
                                                                 message_len,
                                                                 descriptor_fds,
                                                                 &descriptor_count,
                                                                 EINVAL);
                        }
                        descriptor_fds[descriptor_count] = received_fd;
                        descriptor_count += 1U;
                    }
                }
            }
            if (descriptor_count > 1U) {
                return llam_broker_read_message_fail(message,
                                                     message_len,
                                                     descriptor_fds,
                                                     &descriptor_count,
                                                     EINVAL);
            }
            done += (size_t)nread;
            continue;
        }
        if (nread == 0) {
            return llam_broker_read_message_fail(message,
                                                 message_len,
                                                 descriptor_fds,
                                                 &descriptor_count,
                                                 EPIPE);
        }
        if (errno == EINTR) {
            continue;
        }
        if (descriptor_count > 0U) {
            int saved_errno = errno;

            return llam_broker_read_message_fail(message,
                                                 message_len,
                                                 descriptor_fds,
                                                 &descriptor_count,
                                                 saved_errno);
        }
        {
            int saved_errno = errno;

            return llam_broker_read_message_fail(message,
                                                 message_len,
                                                 descriptor_fds,
                                                 &descriptor_count,
                                                 saved_errno);
        }
    }
    if (descriptor_count == 1U) {
        *out_descriptor_fd = descriptor_fds[0];
        descriptor_fds[0] = -1;
        descriptor_count = 0U;
    }
    return 0;
}

int llam_broker_read_request_fd(int fd, llam_broker_wire_request_t *request, int *out_descriptor_fd) {
    return llam_broker_read_message_fd(fd, request, sizeof(*request), out_descriptor_fd);
}

int llam_broker_read_response_fd(int fd, llam_broker_wire_response_t *response, int *out_descriptor_fd) {
    int rc;

    rc = llam_broker_read_message_fd(fd, response, sizeof(*response), out_descriptor_fd);
    if (rc != 0) {
        return rc;
    }
    if (llam_broker_validate_response_frame_or_clear(response) != 0) {
        if (out_descriptor_fd != NULL && *out_descriptor_fd >= 0) {
            close(*out_descriptor_fd);
            *out_descriptor_fd = -1;
        }
        return -1;
    }
    if (response != NULL && response->status != 0) {
        /*
         * A descriptor-bearing error response is malformed authority. The
         * transport read itself succeeded, so keep the response status visible,
         * but close any attached fd and scrub authority outputs before callers
         * can accidentally treat them as success payload.
         */
        if (out_descriptor_fd != NULL && *out_descriptor_fd >= 0) {
            close(*out_descriptor_fd);
            *out_descriptor_fd = -1;
        }
    }
    return 0;
}

static int llam_broker_write_message_with_descriptor(int fd,
                                                     const void *message,
                                                     size_t message_len,
                                                     int descriptor_fd) {
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;

    if (LLAM_UNLIKELY(fd < 0 || message == NULL || message_len == 0U || descriptor_fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)message;
    iov.iov_len = message_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1U;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &descriptor_fd, sizeof(descriptor_fd));
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    for (;;) {
        ssize_t nwritten = llam_broker_sendmsg_transport(fd, &msg);

        if (nwritten > 0) {
            if ((size_t)nwritten < message_len) {
                return llam_broker_write_remainder(fd,
                                                   ((const unsigned char *)message) + (size_t)nwritten,
                                                   message_len - (size_t)nwritten);
            }
            return 0;
        }
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

int llam_broker_write_request_with_descriptor(int fd,
                                              const llam_broker_wire_request_t *request,
                                              int descriptor_fd) {
    return llam_broker_write_message_with_descriptor(fd, request, sizeof(*request), descriptor_fd);
}

int llam_broker_write_response_with_descriptor(int fd,
                                               const llam_broker_wire_response_t *response,
                                               int descriptor_fd) {
    return llam_broker_write_message_with_descriptor(fd, response, sizeof(*response), descriptor_fd);
}

static int llam_broker_read_exact(int fd, void *data, size_t len) {
    unsigned char *cursor = (unsigned char *)data;
    size_t done = 0U;

    if (LLAM_UNLIKELY(fd < 0 || data == NULL || len == 0U)) {
        return llam_broker_fail_clear_output(data, len, EINVAL);
    }
    while (done < len) {
        ssize_t nread = read(fd, cursor + done, len - done);

        if (nread > 0) {
            done += (size_t)nread;
            continue;
        }
        if (nread == 0) {
            return llam_broker_fail_clear_output(data, len, EPIPE);
        }
        if (errno == EINTR) {
            continue;
        }
        return llam_broker_fail_clear_output(data, len, errno);
    }
    return 0;
}

static int llam_broker_write_exact(int fd, const void *data, size_t len) {
    const unsigned char *cursor = (const unsigned char *)data;
    size_t done = 0U;

    while (done < len) {
        ssize_t nwritten = llam_broker_send_transport(fd, cursor + done, len - done);

        if (nwritten > 0) {
            done += (size_t)nwritten;
            continue;
        }
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

int llam_broker_write_response_fd(int fd, const llam_broker_wire_response_t *response) {
    if (LLAM_UNLIKELY(response == NULL)) {
        errno = EINVAL;
        return -1;
    }
    return llam_broker_write_exact(fd, response, sizeof(*response));
}

int llam_broker_request_fd(int fd,
                           const llam_broker_wire_request_t *request,
                           llam_broker_wire_response_t *response) {
    int rc;

    if (LLAM_UNLIKELY(fd < 0 || request == NULL || response == NULL)) {
        return llam_broker_fail_clear_output(response,
                                             response != NULL ? sizeof(*response) : 0U,
                                             EINVAL);
    }
    memset(response, 0, sizeof(*response));
    if (llam_broker_write_exact(fd, request, sizeof(*request)) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    rc = llam_broker_read_exact(fd, response, sizeof(*response));
    if (rc == 0) {
        rc = llam_broker_validate_response_frame_or_clear(response);
    }
    return rc;
}

int llam_broker_request_fd_with_descriptor(int fd,
                                           const llam_broker_wire_request_t *request,
                                           int descriptor_fd,
                                           llam_broker_wire_response_t *response) {
    int rc;

    if (LLAM_UNLIKELY(fd < 0 || request == NULL || descriptor_fd < 0 || response == NULL)) {
        return llam_broker_fail_clear_output(response,
                                             response != NULL ? sizeof(*response) : 0U,
                                             EINVAL);
    }
    memset(response, 0, sizeof(*response));
    if (llam_broker_write_request_with_descriptor(fd, request, descriptor_fd) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    rc = llam_broker_read_exact(fd, response, sizeof(*response));
    if (rc == 0) {
        rc = llam_broker_validate_response_frame_or_clear(response);
    }
    return rc;
}

int llam_broker_request_fd_with_response_descriptor(int fd,
                                                    const llam_broker_wire_request_t *request,
                                                    llam_broker_wire_response_t *response,
                                                    int *out_descriptor_fd) {
    if (out_descriptor_fd != NULL) {
        *out_descriptor_fd = -1;
    }
    if (LLAM_UNLIKELY(fd < 0 || request == NULL || response == NULL || out_descriptor_fd == NULL)) {
        return llam_broker_fail_clear_output(response,
                                             response != NULL ? sizeof(*response) : 0U,
                                             EINVAL);
    }
    memset(response, 0, sizeof(*response));
    if (llam_broker_write_exact(fd, request, sizeof(*request)) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    return llam_broker_read_response_fd(fd, response, out_descriptor_fd);
}

#endif
