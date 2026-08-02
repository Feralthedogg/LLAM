// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#include "leir_test_support.h"

#include "llam/io.h"

#include <errno.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

int leir_test_socketpair_type(
    int socket_type,
    llam_fd_t pair_out[2]) {
    if (pair_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pair_out[0] = LLAM_INVALID_FD;
    pair_out[1] = LLAM_INVALID_FD;

#if LLAM_PLATFORM_WINDOWS
    {
        SOCKET listener = INVALID_SOCKET;
        SOCKET client = INVALID_SOCKET;
        SOCKET server = INVALID_SOCKET;
        struct sockaddr_in addr;
        int addr_len = (int)sizeof(addr);
        BOOL no_delay = TRUE;

        if (socket_type != SOCK_STREAM) {
            errno = EPROTONOSUPPORT;
            return -1;
        }
        listener = WSASocket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            NULL,
            0,
            WSA_FLAG_OVERLAPPED);
        if (listener == INVALID_SOCKET) {
            errno = EIO;
            return -1;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listener,
                 (const struct sockaddr *)&addr,
                 (int)sizeof(addr)) == SOCKET_ERROR ||
            listen(listener, 1) == SOCKET_ERROR ||
            getsockname(listener,
                        (struct sockaddr *)&addr,
                        &addr_len) == SOCKET_ERROR) {
            closesocket(listener);
            errno = EIO;
            return -1;
        }
        client = WSASocket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            NULL,
            0,
            WSA_FLAG_OVERLAPPED);
        if (client == INVALID_SOCKET ||
            connect(client,
                    (const struct sockaddr *)&addr,
                    addr_len) == SOCKET_ERROR) {
            if (client != INVALID_SOCKET) {
                closesocket(client);
            }
            closesocket(listener);
            errno = EIO;
            return -1;
        }
        server = accept(listener, NULL, NULL);
        closesocket(listener);
        if (server == INVALID_SOCKET) {
            closesocket(client);
            errno = EIO;
            return -1;
        }
        (void)setsockopt(
            client,
            IPPROTO_TCP,
            TCP_NODELAY,
            (const char *)&no_delay,
            (int)sizeof(no_delay));
        (void)setsockopt(
            server,
            IPPROTO_TCP,
            TCP_NODELAY,
            (const char *)&no_delay,
            (int)sizeof(no_delay));
        pair_out[0] = (llam_fd_t)client;
        pair_out[1] = (llam_fd_t)server;
        return 0;
    }
#else
    {
        int pair[2];

        if (socketpair(AF_UNIX, socket_type, 0, pair) != 0) {
            return -1;
        }
        pair_out[0] = (llam_fd_t)pair[0];
        pair_out[1] = (llam_fd_t)pair[1];
        return 0;
    }
#endif
}

int leir_test_socketpair(llam_fd_t pair_out[2]) {
    return leir_test_socketpair_type(SOCK_STREAM, pair_out);
}

void leir_test_close(llam_fd_t *fd) {
    if (fd == NULL || LLAM_FD_IS_INVALID(*fd)) {
        return;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)closesocket((SOCKET)*fd);
#else
    (void)close((int)*fd);
#endif
    *fd = LLAM_INVALID_FD;
}

int leir_test_set_socket_buffers(llam_fd_t fd, int size) {
    if (LLAM_FD_IS_INVALID(fd) || size <= 0) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    if (setsockopt(
            (SOCKET)fd,
            SOL_SOCKET,
            SO_SNDBUF,
            (const char *)&size,
            (int)sizeof(size)) == SOCKET_ERROR ||
        setsockopt(
            (SOCKET)fd,
            SOL_SOCKET,
            SO_RCVBUF,
            (const char *)&size,
            (int)sizeof(size)) == SOCKET_ERROR) {
        errno = EIO;
        return -1;
    }
#else
    if (setsockopt(
            (int)fd,
            SOL_SOCKET,
            SO_SNDBUF,
            &size,
            (socklen_t)sizeof(size)) != 0 ||
        setsockopt(
            (int)fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &size,
            (socklen_t)sizeof(size)) != 0) {
        return -1;
    }
#endif
    return 0;
}

int leir_test_shutdown_write(llam_fd_t fd) {
    if (LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    if (shutdown((SOCKET)fd, SD_SEND) == SOCKET_ERROR) {
        errno = EIO;
        return -1;
    }
#else
    if (shutdown((int)fd, SHUT_WR) != 0) {
        return -1;
    }
#endif
    return 0;
}

int leir_test_read_exact(llam_fd_t fd, void *data, size_t size) {
    unsigned char *bytes = data;
    size_t offset = 0U;

    if ((data == NULL && size != 0U) || LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }
    while (offset < size) {
        ssize_t result = llam_read(fd, bytes + offset, size - offset);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

int leir_test_write_all(llam_fd_t fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0U;

    if ((data == NULL && size != 0U) || LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }
    while (offset < size) {
        ssize_t result = llam_write(fd, bytes + offset, size - offset);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

void leir_test_fill_pattern(
    unsigned char *data,
    size_t size,
    uint64_t seed) {
    size_t i;

    if (data == NULL) {
        return;
    }
    for (i = 0U; i < size; i += 1U) {
        uint64_t mixed = seed + (uint64_t)i * UINT64_C(0x9e3779b97f4a7c15);

        mixed ^= mixed >> 29U;
        mixed *= UINT64_C(0xbf58476d1ce4e5b9);
        mixed ^= mixed >> 31U;
        data[i] = (unsigned char)mixed;
    }
}

static uint64_t leir_test_payload_token(
    uint64_t connection,
    uint64_t sequence) {
    uint64_t value =
        sequence ^
        (connection + UINT64_C(1)) *
            UINT64_C(0x9e3779b97f4a7c15);

    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static unsigned char leir_test_payload_byte(
    uint64_t token,
    size_t index) {
    uint64_t value =
        token +
        ((uint64_t)index + UINT64_C(1)) *
            UINT64_C(0xd6e8feb86659fd93);

    value ^= value >> 32U;
    value *= UINT64_C(0xa5a3564e27f8862f);
    value ^= value >> 29U;
    return (unsigned char)(value >> 56U);
}

void leir_test_prepare_payload(
    unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence) {
    uint64_t token;
    size_t i;

    if (data == NULL) {
        return;
    }
    token = leir_test_payload_token(connection, sequence);
    for (i = 0U; i < size; i += 1U) {
        data[i] = i < sizeof(token)
            ? (unsigned char)(token >> (i * 8U))
            : leir_test_payload_byte(token, i);
    }
}

bool leir_test_payload_is_valid(
    const unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence) {
    uint64_t token;
    size_t i;

    if (data == NULL) {
        return size == 0U;
    }
    token = leir_test_payload_token(connection, sequence);
    for (i = 0U; i < size; i += 1U) {
        unsigned char expected = i < sizeof(token)
            ? (unsigned char)(token >> (i * 8U))
            : leir_test_payload_byte(token, i);

        if (data[i] != expected) {
            return false;
        }
    }
    return true;
}

void leir_test_transform_payload(
    unsigned char *data,
    size_t size) {
    static const unsigned char mask[8] = {
        0x4cU, 0x45U, 0x49U, 0x52U,
        0xa5U, 0x5aU, 0xc3U, 0x3cU,
    };
    size_t i;
    size_t limit = size < sizeof(mask) ? size : sizeof(mask);

    if (data == NULL) {
        return;
    }
    for (i = 0U; i < limit; i += 1U) {
        data[i] ^= mask[i];
    }
}

uint64_t leir_test_payload_checksum(
    const unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence) {
    uint64_t hash =
        UINT64_C(1469598103934665603) ^
        leir_test_payload_token(connection, sequence);
    size_t i;

    if (data == NULL && size != 0U) {
        return 0U;
    }
    for (i = 0U; i < size; i += 1U) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint64_t)size;
    hash *= UINT64_C(1099511628211);
    return hash != 0U ? hash : UINT64_C(1);
}
