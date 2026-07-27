#include "leir_peer_process.h"
#include "leir_test_support.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef enum leir_bench_option_id {
    LEIR_BENCH_OPTION_WORKLOAD = 0,
    LEIR_BENCH_OPTION_NODES,
    LEIR_BENCH_OPTION_CONCURRENCY,
    LEIR_BENCH_OPTION_PAYLOAD,
    LEIR_BENCH_OPTION_INLINE_BUDGET,
    LEIR_BENCH_OPTION_ACTIVATIONS,
    LEIR_BENCH_OPTION_MIN_MODE_MS,
    LEIR_BENCH_OPTION_ORDER,
    LEIR_BENCH_OPTION_COUNT,
} leir_bench_option_id_t;

static int parse_u64(const char *text, uint64_t *out) {
    const unsigned char *cursor =
        (const unsigned char *)text;
    uint64_t value = 0U;

    if (text == NULL || out == NULL || *text == '\0') {
        return EINVAL;
    }
    while (*cursor != '\0') {
        unsigned digit;

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9') {
            return EINVAL;
        }
        digit = (unsigned)(*cursor - (unsigned char)'0');
        if (value >
            (UINT64_MAX - (uint64_t)digit) / UINT64_C(10)) {
            return EINVAL;
        }
        value = value * UINT64_C(10) + (uint64_t)digit;
        cursor += 1;
    }
    *out = value;
    return 0;
}

static int option_id_from_name(
    const char *name,
    leir_bench_option_id_t *out) {
    static const char *const names[LEIR_BENCH_OPTION_COUNT] = {
        "--workload",
        "--nodes",
        "--concurrency",
        "--payload",
        "--inline-budget",
        "--activations",
        "--min-mode-ms",
        "--order",
    };
    unsigned i;

    if (name == NULL || out == NULL) {
        return EINVAL;
    }
    for (i = 0U; i < LEIR_BENCH_OPTION_COUNT; i += 1U) {
        if (strcmp(name, names[i]) == 0) {
            *out = (leir_bench_option_id_t)i;
            return 0;
        }
    }
    return EINVAL;
}

static int assign_option(
    leir_bench_options_t *options,
    leir_bench_option_id_t id,
    const char *text) {
    uint64_t value;

    if (options == NULL || text == NULL) {
        return EINVAL;
    }
    if (id == LEIR_BENCH_OPTION_WORKLOAD) {
        if (strcmp(text, "socket_relay") == 0) {
            options->workload =
                LEIR_BENCH_WORKLOAD_SOCKET_RELAY;
            return 0;
        }
        if (strcmp(text, "framed_rpc") == 0) {
            options->workload =
                LEIR_BENCH_WORKLOAD_FRAMED_RPC;
            return 0;
        }
        if (strcmp(text, "graph_break") == 0) {
            options->workload =
                LEIR_BENCH_WORKLOAD_GRAPH_BREAK;
            return 0;
        }
        return EINVAL;
    }
    if (id == LEIR_BENCH_OPTION_ORDER) {
        if (strcmp(text, "ABBA") == 0) {
            options->order = LEIR_BENCH_ORDER_ABBA;
            return 0;
        }
        if (strcmp(text, "BAAB") == 0) {
            options->order = LEIR_BENCH_ORDER_BAAB;
            return 0;
        }
        return EINVAL;
    }
    if (parse_u64(text, &value) != 0) {
        return EINVAL;
    }

    switch (id) {
        case LEIR_BENCH_OPTION_NODES:
            if (value != 1U && value != 2U &&
                value != 4U && value != 8U) {
                return EINVAL;
            }
            options->nodes = (unsigned)value;
            return 0;
        case LEIR_BENCH_OPTION_CONCURRENCY:
            if (value == 0U || value > 512U) {
                return EINVAL;
            }
            options->concurrency = (unsigned)value;
            return 0;
        case LEIR_BENCH_OPTION_PAYLOAD:
            if (value < 64U || value > 16384U ||
                value > SIZE_MAX) {
                return EINVAL;
            }
            options->payload = (size_t)value;
            return 0;
        case LEIR_BENCH_OPTION_INLINE_BUDGET:
            if (value == 0U || value > 32U) {
                return EINVAL;
            }
            options->inline_budget = (unsigned)value;
            return 0;
        case LEIR_BENCH_OPTION_ACTIVATIONS:
            if (value == 0U) {
                return EINVAL;
            }
            options->activations = value;
            return 0;
        case LEIR_BENCH_OPTION_MIN_MODE_MS:
            if (value == 0U ||
                value > UINT64_MAX / UINT64_C(1000000)) {
                return EINVAL;
            }
            options->min_mode_ns =
                value * UINT64_C(1000000);
            return 0;
        case LEIR_BENCH_OPTION_WORKLOAD:
        case LEIR_BENCH_OPTION_ORDER:
        case LEIR_BENCH_OPTION_COUNT:
        default:
            return EINVAL;
    }
}

int leir_bench_parse_options(
    int argc,
    char *const argv[],
    leir_bench_options_t *out) {
    bool seen[LEIR_BENCH_OPTION_COUNT] = {false};
    leir_bench_options_t options;
    int index;

    if (out == NULL || argv == NULL ||
        argc != 1 + 2 * LEIR_BENCH_OPTION_COUNT) {
        return EINVAL;
    }
    memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; index += 2) {
        leir_bench_option_id_t id;

        if (argv[index] == NULL || argv[index + 1] == NULL ||
            option_id_from_name(argv[index], &id) != 0 ||
            seen[id] ||
            assign_option(
                &options, id, argv[index + 1]) != 0) {
            return EINVAL;
        }
        seen[id] = true;
    }
    for (index = 0;
         index < (int)LEIR_BENCH_OPTION_COUNT;
         index += 1) {
        if (!seen[index]) {
            return EINVAL;
        }
    }
    *out = options;
    return 0;
}

uint64_t leir_bench_calibration_target_block_ns(
    uint64_t min_mode_ns) {
    /*
     * Each mode owns eight measured blocks. Calibrate each block for half of
     * the requested mode duration, retaining a 4x margin when the final frozen
     * run is faster than the calibration probe.
     */
    return min_mode_ns / 2U +
           (min_mode_ns % 2U != 0U ? 1U : 0U);
}

const char *leir_bench_workload_name(
    leir_bench_workload_t workload) {
    switch (workload) {
        case LEIR_BENCH_WORKLOAD_SOCKET_RELAY:
            return "socket_relay";
        case LEIR_BENCH_WORKLOAD_FRAMED_RPC:
            return "framed_rpc";
        case LEIR_BENCH_WORKLOAD_GRAPH_BREAK:
            return "graph_break";
        default:
            return "unknown";
    }
}

const char *leir_bench_order_name(leir_bench_order_t order) {
    switch (order) {
        case LEIR_BENCH_ORDER_ABBA:
            return "ABBA";
        case LEIR_BENCH_ORDER_BAAB:
            return "BAAB";
        default:
            return "unknown";
    }
}

#define LEIR_PEER_RESULT_MAGIC UINT64_C(0x4c45495250454552)

typedef enum leir_peer_stage {
    LEIR_PEER_STAGE_SEND = 0,
    LEIR_PEER_STAGE_RECEIVE = 1,
    LEIR_PEER_STAGE_DONE = 2,
} leir_peer_stage_t;

typedef struct leir_peer_connection {
    uint64_t sequence;
    uint64_t transaction_count;
    size_t offset;
    leir_peer_stage_t stage;
    unsigned char *request;
    unsigned char *response;
} leir_peer_connection_t;

typedef struct leir_peer_wire_result {
    uint64_t magic;
    uint64_t checksum;
    uint64_t completed_transactions;
    int32_t status;
    uint32_t reserved;
} leir_peer_wire_result_t;

struct leir_peer_process {
    leir_peer_config_t config;
    llam_fd_t *server_fds;
    llam_fd_t *peer_fds;
    bool start_signaled;
#if LLAM_PLATFORM_WINDOWS
    HANDLE thread;
    atomic_uint thread_start;
    leir_peer_wire_result_t thread_result;
    bool winsock_started;
#else
    pid_t pid;
    int control_write_fd;
    int result_read_fd;
#endif
};

static void close_fd_array(llam_fd_t *fds, unsigned count) {
    unsigned i;

    if (fds == NULL) {
        return;
    }
    for (i = 0U; i < count; i += 1U) {
        leir_test_close(&fds[i]);
    }
}

static int validate_peer_config(
    const leir_peer_config_t *config) {
    if (config == NULL ||
        config->workload < LEIR_BENCH_WORKLOAD_SOCKET_RELAY ||
        config->workload > LEIR_BENCH_WORKLOAD_GRAPH_BREAK ||
        config->concurrency == 0U ||
        config->concurrency > 512U ||
        config->payload < 8U ||
        config->payload > 16384U ||
        config->activations == 0U ||
        config->transactions_per_activation == 0U) {
        return EINVAL;
    }
    return 0;
}

static uint64_t transactions_for_connection(
    const leir_peer_config_t *config,
    unsigned connection) {
    uint64_t activations =
        config->activations / config->concurrency;

    if ((uint64_t)connection <
        config->activations % config->concurrency) {
        activations += 1U;
    }
    if (activations >
        UINT64_MAX / config->transactions_per_activation) {
        return UINT64_MAX;
    }
    return activations * config->transactions_per_activation;
}

#if LLAM_PLATFORM_WINDOWS
typedef WSAPOLLFD leir_peer_pollfd_t;
#else
typedef struct pollfd leir_peer_pollfd_t;
#endif

static int peer_poll(
    leir_peer_pollfd_t *fds,
    unsigned count) {
#if LLAM_PLATFORM_WINDOWS
    int rc = WSAPoll(fds, (ULONG)count, -1);

    if (rc == SOCKET_ERROR) {
        errno = EIO;
        return -1;
    }
    return rc;
#else
    int rc;

    do {
        rc = poll(fds, (nfds_t)count, -1);
    } while (rc < 0 && errno == EINTR);
    return rc;
#endif
}

static int peer_socket_set_nonblocking(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    u_long enabled = 1UL;

    if (ioctlsocket((SOCKET)fd, FIONBIO, &enabled) == SOCKET_ERROR) {
        errno = EIO;
        return -1;
    }
#else
    int flags = fcntl((int)fd, F_GETFL, 0);

    if (flags < 0 ||
        fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
#endif
    return 0;
}

static ssize_t peer_socket_send(
    llam_fd_t fd,
    const void *data,
    size_t size) {
#if LLAM_PLATFORM_WINDOWS
    int chunk = size > INT_MAX ? INT_MAX : (int)size;
    int result = send((SOCKET)fd, data, chunk, 0);

    if (result == SOCKET_ERROR) {
        errno = WSAGetLastError() == WSAEWOULDBLOCK
            ? EAGAIN
            : EIO;
        return -1;
    }
    return (ssize_t)result;
#else
    int flags = 0;

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    return send((int)fd, data, size, flags);
#endif
}

static ssize_t peer_socket_receive(
    llam_fd_t fd,
    void *data,
    size_t size) {
#if LLAM_PLATFORM_WINDOWS
    int chunk = size > INT_MAX ? INT_MAX : (int)size;
    int result = recv((SOCKET)fd, data, chunk, 0);

    if (result == SOCKET_ERROR) {
        errno = WSAGetLastError() == WSAEWOULDBLOCK
            ? EAGAIN
            : EIO;
        return -1;
    }
    return (ssize_t)result;
#else
    return recv((int)fd, data, size, 0);
#endif
}

static uint64_t rotate_checksum(
    uint64_t value,
    unsigned rotation) {
    rotation &= 63U;
    return rotation == 0U
        ? value
        : (value << rotation) |
              (value >> (64U - rotation));
}

static int service_peer_connections(
    const leir_peer_config_t *config,
    llam_fd_t *peer_fds,
    leir_peer_wire_result_t *result) {
    leir_peer_connection_t *connections = NULL;
    leir_peer_pollfd_t *poll_fds = NULL;
    unsigned char *buffers = NULL;
    uint64_t checksum = 0U;
    uint64_t completed = 0U;
    unsigned active = 0U;
    unsigned i;
    int status = 0;

    if (config == NULL || peer_fds == NULL || result == NULL) {
        return EINVAL;
    }
    connections = calloc(
        config->concurrency, sizeof(connections[0]));
    poll_fds = calloc(
        config->concurrency, sizeof(poll_fds[0]));
    if (config->payload >
        SIZE_MAX / (2U * config->concurrency)) {
        status = EOVERFLOW;
        goto cleanup;
    }
    buffers = calloc(
        2U * config->concurrency, config->payload);
    if (connections == NULL || poll_fds == NULL ||
        buffers == NULL) {
        status = ENOMEM;
        goto cleanup;
    }

    for (i = 0U; i < config->concurrency; i += 1U) {
        leir_peer_connection_t *connection =
            &connections[i];

        if (peer_socket_set_nonblocking(peer_fds[i]) != 0) {
            status = errno != 0 ? errno : EIO;
            goto cleanup;
        }
        connection->transaction_count =
            transactions_for_connection(config, i);
        if (connection->transaction_count == UINT64_MAX) {
            status = EOVERFLOW;
            goto cleanup;
        }
        connection->request =
            buffers + (size_t)i * config->payload;
        connection->response =
            buffers +
            ((size_t)config->concurrency + i) *
                config->payload;
        if (connection->transaction_count == 0U) {
            connection->stage = LEIR_PEER_STAGE_DONE;
            leir_test_close(&peer_fds[i]);
        } else {
            connection->stage = LEIR_PEER_STAGE_SEND;
            leir_test_prepare_payload(
                connection->request,
                config->payload,
                i,
                0U);
            active += 1U;
        }
    }

    while (active != 0U) {
        if (active > INT_MAX) {
            status = EOVERFLOW;
            goto cleanup;
        }
        for (i = 0U; i < config->concurrency; i += 1U) {
            leir_peer_connection_t *connection =
                &connections[i];

            memset(&poll_fds[i], 0, sizeof(poll_fds[i]));
            if (connection->stage == LEIR_PEER_STAGE_DONE) {
#if LLAM_PLATFORM_WINDOWS
                poll_fds[i].fd = INVALID_SOCKET;
#else
                poll_fds[i].fd = -1;
#endif
                continue;
            }
            poll_fds[i].fd = peer_fds[i];
            poll_fds[i].events =
                connection->stage == LEIR_PEER_STAGE_SEND
                    ? POLLOUT
                    : POLLIN;
        }
        if (peer_poll(poll_fds, config->concurrency) <= 0) {
            status = errno != 0 ? errno : EIO;
            goto cleanup;
        }

        for (i = 0U; i < config->concurrency; i += 1U) {
            leir_peer_connection_t *connection =
                &connections[i];
            ssize_t io_result;

            if (connection->stage == LEIR_PEER_STAGE_DONE ||
                poll_fds[i].revents == 0) {
                continue;
            }
            if ((poll_fds[i].revents &
                 (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                status = EPIPE;
                goto cleanup;
            }
            if (connection->stage == LEIR_PEER_STAGE_SEND) {
                if ((poll_fds[i].revents & POLLOUT) == 0) {
                    continue;
                }
                io_result = peer_socket_send(
                    peer_fds[i],
                    connection->request + connection->offset,
                    config->payload - connection->offset);
            } else {
                if ((poll_fds[i].revents & POLLIN) == 0) {
                    continue;
                }
                io_result = peer_socket_receive(
                    peer_fds[i],
                    connection->response + connection->offset,
                    config->payload - connection->offset);
            }
            if (io_result < 0) {
                if (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                    continue;
                }
                status = errno != 0 ? errno : EIO;
                goto cleanup;
            }
            if (io_result == 0 ||
                (size_t)io_result >
                    config->payload - connection->offset) {
                status = io_result == 0 ? EPIPE : EPROTO;
                goto cleanup;
            }
            connection->offset += (size_t)io_result;
            if (connection->offset != config->payload) {
                continue;
            }
            connection->offset = 0U;
            if (connection->stage == LEIR_PEER_STAGE_SEND) {
                connection->stage =
                    LEIR_PEER_STAGE_RECEIVE;
                continue;
            }

            if (config->workload ==
                LEIR_BENCH_WORKLOAD_GRAPH_BREAK) {
                leir_test_transform_payload(
                    connection->request, config->payload);
            }
            if (memcmp(
                    connection->request,
                    connection->response,
                    config->payload) != 0) {
                status = EPROTO;
                goto cleanup;
            }
            checksum ^= rotate_checksum(
                leir_test_payload_checksum(
                    connection->response,
                    config->payload,
                    i,
                    connection->sequence),
                (unsigned)((i + connection->sequence) & 63U));
            completed += 1U;
            connection->sequence += 1U;
            if (connection->sequence ==
                connection->transaction_count) {
                connection->stage = LEIR_PEER_STAGE_DONE;
                leir_test_close(&peer_fds[i]);
                active -= 1U;
            } else {
                connection->stage = LEIR_PEER_STAGE_SEND;
                leir_test_prepare_payload(
                    connection->request,
                    config->payload,
                    i,
                    connection->sequence);
            }
        }
    }

cleanup:
    result->magic = LEIR_PEER_RESULT_MAGIC;
    result->checksum = checksum;
    result->completed_transactions = completed;
    result->status = status;
    result->reserved = 0U;
    close_fd_array(peer_fds, config->concurrency);
    free(buffers);
    free(poll_fds);
    free(connections);
    return status;
}

#if LLAM_PLATFORM_WINDOWS

static DWORD WINAPI peer_thread_entry(void *opaque) {
    leir_peer_process_t *peer = opaque;

    while (atomic_load_explicit(
               &peer->thread_start, memory_order_acquire) == 0U) {
        Sleep(1U);
    }
    (void)service_peer_connections(
        &peer->config,
        peer->peer_fds,
        &peer->thread_result);
    return 0U;
}

#else

static int write_all_fd(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0U;

    while (offset < size) {
        ssize_t result = write(fd, bytes + offset, size - offset);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

static int read_all_fd(int fd, void *data, size_t size) {
    unsigned char *bytes = data;
    size_t offset = 0U;

    while (offset < size) {
        ssize_t result = read(fd, bytes + offset, size - offset);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

static void peer_child_main(
    const leir_peer_config_t *config,
    llam_fd_t *peer_fds,
    int control_read_fd,
    int result_write_fd) {
    leir_peer_wire_result_t result;
    unsigned char start_byte = 0U;
    int status;

    (void)signal(SIGPIPE, SIG_IGN);
    memset(&result, 0, sizeof(result));
    if (read_all_fd(
            control_read_fd, &start_byte, sizeof(start_byte)) != 0 ||
        start_byte != 1U) {
        result.magic = LEIR_PEER_RESULT_MAGIC;
        result.status = EPROTO;
        status = EPROTO;
    } else {
        status = service_peer_connections(
            config, peer_fds, &result);
    }
    (void)write_all_fd(
        result_write_fd, &result, sizeof(result));
    (void)close(control_read_fd);
    (void)close(result_write_fd);
    _exit(status == 0 ? 0 : 1);
}

#endif

static void destroy_peer_storage(leir_peer_process_t *peer) {
    if (peer == NULL) {
        return;
    }
    close_fd_array(peer->server_fds, peer->config.concurrency);
    close_fd_array(peer->peer_fds, peer->config.concurrency);
    free(peer->server_fds);
    free(peer->peer_fds);
#if LLAM_PLATFORM_WINDOWS
    if (peer->winsock_started) {
        (void)WSACleanup();
    }
#endif
    free(peer);
}

int leir_peer_process_start(
    const leir_peer_config_t *config,
    leir_peer_process_t **out) {
    leir_peer_process_t *peer = NULL;
    unsigned i;

    if (out == NULL || validate_peer_config(config) != 0) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    peer = calloc(1U, sizeof(*peer));
    if (peer == NULL) {
        return -1;
    }
    peer->config = *config;
#if LLAM_PLATFORM_WINDOWS
    {
        WSADATA winsock_data;

        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            free(peer);
            errno = EIO;
            return -1;
        }
        peer->winsock_started = true;
    }
#endif
    peer->server_fds = malloc(
        config->concurrency * sizeof(peer->server_fds[0]));
    peer->peer_fds = malloc(
        config->concurrency * sizeof(peer->peer_fds[0]));
    if (peer->server_fds == NULL || peer->peer_fds == NULL) {
        errno = ENOMEM;
        destroy_peer_storage(peer);
        return -1;
    }
    for (i = 0U; i < config->concurrency; i += 1U) {
        peer->server_fds[i] = LLAM_INVALID_FD;
        peer->peer_fds[i] = LLAM_INVALID_FD;
    }
    for (i = 0U; i < config->concurrency; i += 1U) {
        llam_fd_t pair[2] = {
            LLAM_INVALID_FD,
            LLAM_INVALID_FD,
        };

        if (leir_test_socketpair(pair) != 0) {
            destroy_peer_storage(peer);
            return -1;
        }
        peer->server_fds[i] = pair[0];
        peer->peer_fds[i] = pair[1];
    }

#if LLAM_PLATFORM_WINDOWS
    atomic_init(&peer->thread_start, 0U);
    peer->thread = CreateThread(
        NULL, 0U, peer_thread_entry, peer, 0U, NULL);
    if (peer->thread == NULL) {
        errno = EIO;
        destroy_peer_storage(peer);
        return -1;
    }
#else
    {
        int control_pipe[2] = {-1, -1};
        int result_pipe[2] = {-1, -1};
        pid_t pid;

        peer->control_write_fd = -1;
        peer->result_read_fd = -1;
        if (pipe(control_pipe) != 0 || pipe(result_pipe) != 0) {
            int saved_errno = errno;

            if (control_pipe[0] >= 0) {
                (void)close(control_pipe[0]);
                (void)close(control_pipe[1]);
            }
            if (result_pipe[0] >= 0) {
                (void)close(result_pipe[0]);
                (void)close(result_pipe[1]);
            }
            destroy_peer_storage(peer);
            errno = saved_errno;
            return -1;
        }
        pid = fork();
        if (pid < 0) {
            int saved_errno = errno;

            (void)close(control_pipe[0]);
            (void)close(control_pipe[1]);
            (void)close(result_pipe[0]);
            (void)close(result_pipe[1]);
            destroy_peer_storage(peer);
            errno = saved_errno;
            return -1;
        }
        if (pid == 0) {
            (void)close(control_pipe[1]);
            (void)close(result_pipe[0]);
            close_fd_array(
                peer->server_fds, config->concurrency);
            peer_child_main(
                config,
                peer->peer_fds,
                control_pipe[0],
                result_pipe[1]);
        }
        peer->pid = pid;
        peer->control_write_fd = control_pipe[1];
        peer->result_read_fd = result_pipe[0];
        (void)close(control_pipe[0]);
        (void)close(result_pipe[1]);
        close_fd_array(peer->peer_fds, config->concurrency);
    }
#endif
    *out = peer;
    return 0;
}

size_t leir_peer_process_connection_count(
    const leir_peer_process_t *peer) {
    return peer != NULL ? peer->config.concurrency : 0U;
}

llam_fd_t leir_peer_process_server_fd(
    const leir_peer_process_t *peer,
    size_t index) {
    if (peer == NULL || index >= peer->config.concurrency) {
        return LLAM_INVALID_FD;
    }
    return peer->server_fds[index];
}

int leir_peer_process_signal_start(leir_peer_process_t *peer) {
    if (peer == NULL || peer->start_signaled) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    atomic_store_explicit(
        &peer->thread_start, 1U, memory_order_release);
#else
    {
        unsigned char start_byte = 1U;

        if (write_all_fd(
                peer->control_write_fd,
                &start_byte,
                sizeof(start_byte)) != 0) {
            return -1;
        }
        (void)close(peer->control_write_fd);
        peer->control_write_fd = -1;
    }
#endif
    peer->start_signaled = true;
    return 0;
}

int leir_peer_process_finish(
    leir_peer_process_t *peer,
    leir_peer_result_t *result_out) {
    leir_peer_wire_result_t wire_result;
    int status = 0;

    if (peer == NULL || result_out == NULL ||
        !peer->start_signaled) {
        errno = EINVAL;
        return -1;
    }
    memset(&wire_result, 0, sizeof(wire_result));
#if LLAM_PLATFORM_WINDOWS
    if (WaitForSingleObject(peer->thread, INFINITE) !=
        WAIT_OBJECT_0) {
        status = EIO;
    } else {
        wire_result = peer->thread_result;
    }
    (void)CloseHandle(peer->thread);
    peer->thread = NULL;
#else
    {
        int child_status = 0;

        if (read_all_fd(
                peer->result_read_fd,
                &wire_result,
                sizeof(wire_result)) != 0) {
            status = errno != 0 ? errno : EIO;
        }
        (void)close(peer->result_read_fd);
        peer->result_read_fd = -1;
        if (waitpid(peer->pid, &child_status, 0) != peer->pid ||
            !WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != 0) {
            if (status == 0) {
                status = EPROTO;
            }
        }
        peer->pid = 0;
    }
#endif
    if (status == 0 &&
        wire_result.magic != LEIR_PEER_RESULT_MAGIC) {
        status = EPROTO;
    }
    result_out->checksum = wire_result.checksum;
    result_out->completed_transactions =
        wire_result.completed_transactions;
    result_out->status = wire_result.status;
    if (status == 0 && wire_result.status != 0) {
        status = wire_result.status;
    }
    destroy_peer_storage(peer);
    if (status != 0) {
        errno = status;
        return -1;
    }
    return 0;
}

void leir_peer_process_abort(leir_peer_process_t *peer) {
    if (peer == NULL) {
        return;
    }
    close_fd_array(peer->server_fds, peer->config.concurrency);
#if LLAM_PLATFORM_WINDOWS
    if (!peer->start_signaled) {
        atomic_store_explicit(
            &peer->thread_start, 1U, memory_order_release);
    }
    if (peer->thread != NULL) {
        (void)WaitForSingleObject(peer->thread, INFINITE);
        (void)CloseHandle(peer->thread);
        peer->thread = NULL;
    }
#else
    if (peer->control_write_fd >= 0) {
        (void)close(peer->control_write_fd);
        peer->control_write_fd = -1;
    }
    if (peer->result_read_fd >= 0) {
        (void)close(peer->result_read_fd);
        peer->result_read_fd = -1;
    }
    if (peer->pid > 0) {
        (void)kill(peer->pid, SIGTERM);
        (void)waitpid(peer->pid, NULL, 0);
        peer->pid = 0;
    }
#endif
    destroy_peer_storage(peer);
}

const char *leir_peer_kind_name(void) {
#if LLAM_PLATFORM_WINDOWS
    return "thread";
#else
    return "process";
#endif
}

const char *leir_peer_cpu_scope_name(void) {
#if LLAM_PLATFORM_WINDOWS
    return "combined";
#else
    return "server";
#endif
}
