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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "runtime_broker.h"

#if !LLAM_PLATFORM_WINDOWS

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <string.h>

#define LLAM_BROKER_TRANSPORT_IO_TIMEOUT_MS 250
#define LLAM_BROKER_TRANSPORT_FRAME_TIMEOUT_NS (2ULL * 1000ULL * 1000ULL * 1000ULL)
#define LLAM_BROKER_SCM_RIGHTS_MAX_FDS 512U

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint_fast64_t g_llam_broker_scm_rights_receive_limit;

void llam_broker_test_set_scm_rights_receive_limit(size_t max_fds) {
    if (max_fds > LLAM_BROKER_SCM_RIGHTS_MAX_FDS) {
        max_fds = LLAM_BROKER_SCM_RIGHTS_MAX_FDS;
    }
    atomic_store_explicit(&g_llam_broker_scm_rights_receive_limit,
                          (uint_fast64_t)max_fds,
                          memory_order_relaxed);
}

static size_t llam_broker_scm_rights_receive_limit(void) {
    uint_fast64_t limit = atomic_load_explicit(&g_llam_broker_scm_rights_receive_limit,
                                               memory_order_relaxed);

    return limit == 0U ? LLAM_BROKER_SCM_RIGHTS_MAX_FDS : (size_t)limit;
}
#else
static size_t llam_broker_scm_rights_receive_limit(void) {
    return LLAM_BROKER_SCM_RIGHTS_MAX_FDS;
}
#endif

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

static uint64_t llam_broker_transport_frame_deadline(void) {
    uint64_t now_ns = llam_now_ns();

    if (now_ns == 0U || UINT64_MAX - now_ns < LLAM_BROKER_TRANSPORT_FRAME_TIMEOUT_NS) {
        return UINT64_MAX;
    }
    return now_ns + LLAM_BROKER_TRANSPORT_FRAME_TIMEOUT_NS;
}

static int llam_broker_transport_wait_ms(uint64_t deadline_ns) {
    uint64_t now_ns;
    uint64_t remaining_ns;
    uint64_t remaining_ms;

    if (deadline_ns == UINT64_MAX) {
        return LLAM_BROKER_TRANSPORT_IO_TIMEOUT_MS;
    }
    now_ns = llam_now_ns();
    if (now_ns == 0U) {
        return LLAM_BROKER_TRANSPORT_IO_TIMEOUT_MS;
    }
    if (deadline_ns <= now_ns) {
        return 0;
    }
    remaining_ns = deadline_ns - now_ns;
    remaining_ms = (remaining_ns + 999999ULL) / 1000000ULL;
    if (remaining_ms == 0U) {
        return 0;
    }
    if (remaining_ms > LLAM_BROKER_TRANSPORT_IO_TIMEOUT_MS) {
        return LLAM_BROKER_TRANSPORT_IO_TIMEOUT_MS;
    }
    return (int)remaining_ms;
}

static int llam_broker_wait_transport_fd(int fd, short events, uint64_t deadline_ns) {
    struct pollfd pfd;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;
    for (;;) {
        int timeout_ms = llam_broker_transport_wait_ms(deadline_ns);
        int rc;

        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        rc = poll(&pfd, 1U, timeout_ms);

        if (rc > 0) {
            if ((pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return 0;
            }
            errno = EIO;
            return -1;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static void llam_broker_close_received_fds(int *fds, size_t *count) {
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

static bool llam_broker_collect_received_fds(const struct msghdr *msg,
                                             size_t control_capacity,
                                             int *descriptor_fds,
                                             size_t *descriptor_count) {
    const size_t header_len = (size_t)CMSG_LEN(0);
    const size_t alignment = (size_t)CMSG_SPACE(1U) - (size_t)CMSG_LEN(0);
    unsigned char *cursor;
    unsigned char *control_end;
    size_t control_len;
    bool malformed = false;

    if (msg == NULL || descriptor_fds == NULL || descriptor_count == NULL ||
        (msg->msg_control == NULL && msg->msg_controllen != 0U) || alignment == 0U) {
        return true;
    }
    control_len = (size_t)msg->msg_controllen;
    if (control_len > control_capacity) {
        control_len = control_capacity;
        malformed = true;
    }
    if (control_len == 0U) {
        return malformed;
    }
    cursor = (unsigned char *)msg->msg_control;
    control_end = cursor + control_len;

    while (cursor < control_end) {
        struct cmsghdr *cmsg;
        unsigned char *data;
        size_t remaining = (size_t)(control_end - cursor);
        size_t claimed_len;
        size_t claimed_payload;
        size_t available_payload;
        size_t bounded_payload;
        size_t advance;
        size_t remainder;

        /* A partial final header cannot describe any owned descriptors. */
        if (remaining < header_len) {
            break;
        }
        cmsg = (struct cmsghdr *)(void *)cursor;
        claimed_len = (size_t)cmsg->cmsg_len;
        if (claimed_len < header_len) {
            malformed = true;
            break;
        }

        /* CMSG_DATA is safe only after proving the fixed header is present. */
        data = (unsigned char *)CMSG_DATA(cmsg);
        if (data != cursor + header_len || data > control_end) {
            malformed = true;
            break;
        }
        claimed_payload = claimed_len - header_len;
        available_payload = (size_t)(control_end - data);
        bounded_payload = claimed_payload < available_payload ? claimed_payload : available_payload;
        if (claimed_payload > available_payload) {
            malformed = true;
        }

        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t complete_fds = bounded_payload / sizeof(int);
            size_t i;

            if ((claimed_payload % sizeof(int)) != 0U) {
                malformed = true;
            }
            for (i = 0U; i < complete_fds; ++i) {
                int received_fd;

                memcpy(&received_fd, data + (i * sizeof(int)), sizeof(received_fd));
                /* Only non-negative values can be descriptors installed for us. */
                if (received_fd < 0) {
                    malformed = true;
                    continue;
                }
                if (*descriptor_count >= LLAM_BROKER_SCM_RIGHTS_MAX_FDS) {
                    close(received_fd);
                    malformed = true;
                    continue;
                }
                descriptor_fds[*descriptor_count] = received_fd;
                *descriptor_count += 1U;
            }
        }

        /*
         * An oversized current header is the Darwin MSG_CTRUNC shape: the
         * kernel leaves cmsg_len at the untruncated size. We have collected all
         * complete returned fds above; never pass this length to CMSG_NXTHDR.
         */
        if (claimed_len > remaining) {
            malformed = true;
            break;
        }

        /* Advance only after proving the aligned size cannot overflow or escape. */
        remainder = claimed_len % alignment;
        advance = claimed_len;
        if (remainder != 0U) {
            size_t padding = alignment - remainder;

            if (advance > SIZE_MAX - padding) {
                malformed = true;
                break;
            }
            advance += padding;
        }
        if (advance == 0U || advance > remaining) {
            break;
        }
        cursor += advance;
    }
    return malformed;
}

static int llam_broker_read_message_fail(void *message,
                                         size_t message_len,
                                         int *fds,
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

static int llam_broker_write_remainder(int fd, const unsigned char *cursor, size_t len, uint64_t deadline_ns) {
    size_t done = 0U;

    while (done < len) {
        ssize_t nwritten;

        if (llam_broker_wait_transport_fd(fd, POLLOUT, deadline_ns) != 0) {
            return -1;
        }
        nwritten = llam_broker_send_transport(fd, cursor + done, len - done);
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
    int descriptor_fds[LLAM_BROKER_SCM_RIGHTS_MAX_FDS];
    size_t descriptor_count = 0U;
    uint64_t deadline_ns = llam_broker_transport_frame_deadline();
    size_t descriptor_index;

    for (descriptor_index = 0U; descriptor_index < LLAM_BROKER_SCM_RIGHTS_MAX_FDS; ++descriptor_index) {
        descriptor_fds[descriptor_index] = -1;
    }

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
        _Alignas(struct cmsghdr)
            unsigned char control[CMSG_SPACE(sizeof(int) * LLAM_BROKER_SCM_RIGHTS_MAX_FDS)];
        struct iovec iov;
        struct msghdr msg;
        ssize_t nread;
        bool ancillary_malformed = false;
        size_t receive_limit = llam_broker_scm_rights_receive_limit();
        size_t receive_control_capacity = CMSG_SPACE(sizeof(int) * receive_limit);

        /* Invalid/padding bytes must never look like descriptor zero. */
        memset(control, 0xff, sizeof(control));
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (receive_limit < LLAM_BROKER_SCM_RIGHTS_MAX_FDS &&
            receive_control_capacity <= sizeof(control) - sizeof(int)) {
            int boundary_probe_fd = STDIN_FILENO;

            /* Prove the parser never interprets bytes past recvmsg's capacity. */
            memcpy(control + receive_control_capacity, &boundary_probe_fd, sizeof(boundary_probe_fd));
        }
#endif
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = cursor + done;
        iov.iov_len = message_len - done;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1U;
        msg.msg_control = control;
        msg.msg_controllen = receive_control_capacity;
        /*
         * MSG_CMSG_CLOEXEC atomically marks SCM_RIGHTS descriptors close-on-exec
         * on platforms that support it.  We still call llam_broker_set_cloexec_fd
         * below because not every supported POSIX target exposes this flag.
         */
        if (llam_broker_wait_transport_fd(fd, POLLIN, deadline_ns) != 0) {
            return llam_broker_read_message_fail(message,
                                                 message_len,
                                                 descriptor_fds,
                                                 &descriptor_count,
                                                 errno);
        }
#if defined(MSG_CMSG_CLOEXEC)
        nread = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
#else
        nread = recvmsg(fd, &msg, 0);
#endif
        if (nread > 0) {
            ancillary_malformed = llam_broker_collect_received_fds(&msg,
                                                                    receive_control_capacity,
                                                                    descriptor_fds,
                                                                    &descriptor_count);
            /* The kernel installs SCM_RIGHTS fds before reporting MSG_CTRUNC.
             * Enumerate every complete ancillary payload first, then reject and
             * close the whole set so truncation cannot hide leaked descriptors. */
            if ((msg.msg_flags & MSG_CTRUNC) != 0) {
                ancillary_malformed = true;
            }
            for (descriptor_index = 0U; descriptor_index < descriptor_count; ++descriptor_index) {
                if (llam_broker_set_cloexec_fd(descriptor_fds[descriptor_index]) != 0) {
                    ancillary_malformed = true;
                }
            }
            if (ancillary_malformed || descriptor_count > 1U) {
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
    _Alignas(struct cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    uint64_t deadline_ns = llam_broker_transport_frame_deadline();

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
        ssize_t nwritten;

        if (llam_broker_wait_transport_fd(fd, POLLOUT, deadline_ns) != 0) {
            return -1;
        }
        nwritten = llam_broker_sendmsg_transport(fd, &msg);
        if (nwritten > 0) {
            if ((size_t)nwritten < message_len) {
                return llam_broker_write_remainder(fd,
                                                   ((const unsigned char *)message) + (size_t)nwritten,
                                                   message_len - (size_t)nwritten,
                                                   deadline_ns);
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
    pid_t peer_pid = 0;

#if defined(__linux__) && defined(SO_PEERCRED)
    struct {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } peer_cred;
    socklen_t peer_cred_len = (socklen_t)sizeof(peer_cred);

    memset(&peer_cred, 0, sizeof(peer_cred));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer_cred, &peer_cred_len) != 0) {
        if (errno == ENOTCONN) {
            errno = EPIPE;
        }
        return -1;
    }
    peer_pid = peer_cred.pid;
#elif defined(LOCAL_PEERPID) && defined(SOL_LOCAL)
    socklen_t peer_pid_len = (socklen_t)sizeof(peer_pid);

    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &peer_pid_len) != 0) {
        if (errno == ENOTCONN) {
            errno = EPIPE;
        }
        return -1;
    }
#else
    (void)fd;
    errno = ENOTSUP;
    return -1;
#endif
    if (peer_pid <= 0 || peer_pid != getpid()) {
        errno = EACCES;
        return -1;
    }
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
    uint64_t deadline_ns = llam_broker_transport_frame_deadline();

    if (LLAM_UNLIKELY(fd < 0 || data == NULL || len == 0U)) {
        return llam_broker_fail_clear_output(data, len, EINVAL);
    }
    while (done < len) {
        ssize_t nread;

        if (llam_broker_wait_transport_fd(fd, POLLIN, deadline_ns) != 0) {
            return llam_broker_fail_clear_output(data, len, errno);
        }
        nread = read(fd, cursor + done, len - done);
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
    uint64_t deadline_ns = llam_broker_transport_frame_deadline();

    while (done < len) {
        ssize_t nwritten;

        if (llam_broker_wait_transport_fd(fd, POLLOUT, deadline_ns) != 0) {
            return -1;
        }
        nwritten = llam_broker_send_transport(fd, cursor + done, len - done);
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

int llam_broker_request_fd_with_descriptor_trusted(int fd,
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
    if (llam_broker_write_message_with_descriptor(fd, request, sizeof(*request), descriptor_fd) != 0) {
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
    {
        pid_t peer_pid = 0;

#if defined(__linux__) && defined(SO_PEERCRED)
        struct {
            pid_t pid;
            uid_t uid;
            gid_t gid;
        } peer_cred;
        socklen_t peer_cred_len = (socklen_t)sizeof(peer_cred);

        memset(&peer_cred, 0, sizeof(peer_cred));
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer_cred, &peer_cred_len) != 0) {
            if (errno == ENOTCONN) {
                errno = EPIPE;
            }
            return llam_broker_fail_clear_output(response, sizeof(*response), errno);
        }
        peer_pid = peer_cred.pid;
#elif defined(LOCAL_PEERPID) && defined(SOL_LOCAL)
        socklen_t peer_pid_len = (socklen_t)sizeof(peer_pid);

        if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &peer_pid_len) != 0) {
            if (errno == ENOTCONN) {
                errno = EPIPE;
            }
            return llam_broker_fail_clear_output(response, sizeof(*response), errno);
        }
#else
        errno = ENOTSUP;
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
#endif
        if (peer_pid <= 0 || peer_pid != getpid()) {
            return llam_broker_fail_clear_output(response, sizeof(*response), EACCES);
        }
    }
    memset(response, 0, sizeof(*response));
    if (llam_broker_write_exact(fd, request, sizeof(*request)) != 0) {
        return llam_broker_fail_clear_output(response, sizeof(*response), errno);
    }
    return llam_broker_read_response_fd(fd, response, out_descriptor_fd);
}

int llam_broker_request_fd_with_response_descriptor_trusted(int fd,
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
