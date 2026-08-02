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
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int leir_test_set_nonblocking_socket(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    u_long enabled = 1UL;

    if (ioctlsocket((SOCKET)fd, FIONBIO, &enabled) == SOCKET_ERROR) {
        errno = EIO;
        return -1;
    }
#else
    int flags = fcntl((int)fd, F_GETFL, 0);

    if (flags < 0 || fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
#endif
    return 0;
}

int leir_test_connect_write_program_create(
    leir_phase0_program_t **program_out) {
    static const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    static const leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_CONNECT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_WRITE,
            .fd_slot = 0U,
            .buffer_slot = 4U,
            .length_slot = 5U,
            .result_slot = 6U,
            .on_success = 2U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
    };
    const leir_phase0_program_desc_t desc = {
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = sizeof(nodes) / sizeof(nodes[0]),
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };

    return leir_phase0_program_create(&desc, program_out);
}

int leir_test_connect_write_values(
    leir_phase0_value_t *values,
    size_t value_count,
    llam_fd_t fd,
    const void *address,
    socklen_t address_length,
    const void *payload,
    size_t payload_length) {
    if (values == NULL || value_count != 7U ||
        LLAM_FD_IS_INVALID(fd) || address == NULL ||
        address_length == 0U ||
        (payload == NULL && payload_length != 0U)) {
        errno = EINVAL;
        return -1;
    }
    memset(values, 0, value_count * sizeof(values[0]));
    values[0].fd = fd;
    values[1].buffer.data = (void *)(uintptr_t)address;
    values[1].buffer.size = (size_t)address_length;
    values[2].u64 = (uint64_t)address_length;
    values[3].i64 = -1;
    values[4].buffer.data = (void *)(uintptr_t)payload;
    values[4].buffer.size = payload_length;
    values[5].u64 = (uint64_t)payload_length;
    values[6].i64 = -1;
    return 0;
}

bool leir_test_backend_is_unavailable(int error_code) {
    return error_code == ENOTSUP || error_code == EAGAIN ||
           error_code == ENOSYS || error_code == EPERM ||
           error_code == EACCES;
}

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

int leir_test_tcp_listener(
    llam_fd_t *listener_out,
    struct sockaddr_storage *address_out,
    socklen_t *address_length_out) {
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    llam_fd_t listener;

    if (listener_out == NULL || address_out == NULL ||
        address_length_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *listener_out = LLAM_INVALID_FD;
    memset(address_out, 0, sizeof(*address_out));
    *address_length_out = 0U;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (LLAM_FD_IS_INVALID(listener)) {
#if LLAM_PLATFORM_WINDOWS
        errno = EIO;
#endif
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(0U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener,
             (const struct sockaddr *)(const void *)&address,
             (socklen_t)sizeof(address)) != 0 ||
        listen(listener, 16) != 0 ||
        getsockname(listener,
                    (struct sockaddr *)(void *)&address,
                    &address_length) != 0 ||
        leir_test_set_nonblocking_socket(listener) != 0) {
#if LLAM_PLATFORM_WINDOWS
        errno = EIO;
#endif
        leir_test_close(&listener);
        return -1;
    }
    memcpy(address_out, &address, sizeof(address));
    *address_length_out = address_length;
    *listener_out = listener;
    return 0;
}

int leir_test_tcp_client(llam_fd_t *client_out) {
    llam_fd_t client;

    if (client_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *client_out = LLAM_INVALID_FD;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (LLAM_FD_IS_INVALID(client)) {
#if LLAM_PLATFORM_WINDOWS
        errno = EIO;
#endif
        return -1;
    }
    if (leir_test_set_nonblocking_socket(client) != 0) {
        leir_test_close(&client);
        return -1;
    }
    *client_out = client;
    return 0;
}

int leir_test_tcp_refused_address(
    struct sockaddr_storage *address_out,
    socklen_t *address_length_out) {
    llam_fd_t listener = LLAM_INVALID_FD;

    if (leir_test_tcp_listener(
            &listener, address_out, address_length_out) != 0) {
        return -1;
    }
    leir_test_close(&listener);
    return 0;
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
