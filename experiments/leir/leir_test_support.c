#include "leir_test_support.h"

#include <errno.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

int leir_test_socketpair(llam_fd_t pair_out[2]) {
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

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
            return -1;
        }
        pair_out[0] = (llam_fd_t)pair[0];
        pair_out[1] = (llam_fd_t)pair[1];
        return 0;
    }
#endif
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
