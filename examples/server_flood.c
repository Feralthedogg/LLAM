/**
 * @file examples/server_flood.c
 * @brief Native high-throughput stress driver for the LLAM chat server.
 *
 * @details
 * Pushes nonblocking TCP clients as hard as possible and reports both inbound
 * message rate and observed fanout delivery rate.  The Python stress script
 * covers exact delivery checks; this tool focuses on throughput/accounting.
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

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server_flood_stats.h"

#if defined(_WIN32)
int main(void) { fprintf(stderr, "server_flood requires POSIX sockets/fork in this build\n"); return 2; }
#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#define FLOOD_HAVE_AVX512 1
#else
#define FLOOD_HAVE_AVX512 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define FLOOD_DEFAULT_CLIENTS 4U
#define FLOOD_DEFAULT_DURATION_SEC 3.0
#define FLOOD_DEFAULT_DRAIN_SEC 1.0
#define FLOOD_DEFAULT_MESSAGE_BYTES 8U
#define FLOOD_DEFAULT_BATCH 128U
#define FLOOD_MAX_MESSAGE_BYTES 1024U
#define FLOOD_RECV_BUF_BYTES 65536U

typedef struct flood_opts {
    const char *server_path;
    const char *host;
    unsigned clients;
    double duration_sec;
    double drain_sec;
    unsigned message_bytes;
    unsigned batch;
    double target_mps;
    double min_delivery_mps;
    double min_delivery_ratio;
    double shutdown_timeout_sec;
    bool fail_on_forced_stop;
    bool fail_on_missing_stats;
    bool server_lossless;
} flood_opts_t;

typedef struct flood_client {
    int fd;
    size_t write_off;
    uint64_t sent_messages;
    uint64_t recv_lines;
    uint64_t recv_bytes;
    bool closed;
} flood_client_t;

static uint64_t flood_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool flood_parse_unsigned_value(const char *value, unsigned *out) {
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = (unsigned)parsed;
    return true;
}

static bool flood_parse_double_value(const char *value, double *out) {
    char *end = NULL;
    double parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

static unsigned flood_env_unsigned(const char *name, unsigned fallback) {
    const char *value = getenv(name);
    unsigned parsed;

    if (value == NULL || value[0] == '\0') { return fallback; }
    if (!flood_parse_unsigned_value(value, &parsed)) { return fallback; }
    return parsed;
}

static double flood_env_double(const char *name, double fallback) {
    const char *value = getenv(name);
    double parsed;

    if (value == NULL || value[0] == '\0') { return fallback; }
    if (!flood_parse_double_value(value, &parsed)) { return fallback; }
    return parsed;
}

static void flood_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--server ./server] [--host 127.0.0.1] [--clients N]\n"
            "          [--duration SEC] [--drain-sec SEC] [--message-bytes N]\n"
            "          [--batch N] [--target-mps M] [--min-delivery-mps M]\n"
            "          [--min-delivery-ratio R] [--shutdown-timeout SEC]\n"
            "          [--server-lossless] [--server-best-effort]\n"
            "          [--allow-forced-stop] [--fail-on-forced-stop]\n"
            "          [--allow-missing-stats] [--fail-on-missing-stats]\n",
            argv0);
}

static int flood_parse_args(int argc, char **argv, flood_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->server_path = "./server";
    opts->host = "127.0.0.1";
    opts->clients = flood_env_unsigned("LLAM_SERVER_FLOOD_CLIENTS", FLOOD_DEFAULT_CLIENTS);
    opts->duration_sec = flood_env_double("LLAM_SERVER_FLOOD_DURATION", FLOOD_DEFAULT_DURATION_SEC);
    opts->drain_sec = flood_env_double("LLAM_SERVER_FLOOD_DRAIN_SEC", FLOOD_DEFAULT_DRAIN_SEC);
    opts->message_bytes = flood_env_unsigned("LLAM_SERVER_FLOOD_MESSAGE_BYTES", FLOOD_DEFAULT_MESSAGE_BYTES);
    opts->batch = flood_env_unsigned("LLAM_SERVER_FLOOD_BATCH", FLOOD_DEFAULT_BATCH);
    opts->target_mps = flood_env_double("LLAM_SERVER_FLOOD_TARGET_MPS", 0.0);
    opts->min_delivery_mps = flood_env_double("LLAM_SERVER_FLOOD_MIN_DELIVERY_MPS", 0.0);
    opts->min_delivery_ratio = flood_env_double("LLAM_SERVER_FLOOD_MIN_DELIVERY_RATIO", 0.0);
    opts->shutdown_timeout_sec = flood_env_double("LLAM_SERVER_FLOOD_SHUTDOWN_TIMEOUT", 30.0);
    opts->server_lossless = flood_env_unsigned("LLAM_SERVER_FLOOD_SERVER_LOSSLESS", 0U) != 0U;
    opts->fail_on_forced_stop = flood_env_unsigned("LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP", 0U) == 0U;
    if (flood_env_unsigned("LLAM_SERVER_FLOOD_FAIL_ON_FORCED_STOP", 0U) != 0U) {
        opts->fail_on_forced_stop = true;
    }
    opts->fail_on_missing_stats = flood_env_unsigned("LLAM_SERVER_FLOOD_ALLOW_MISSING_STATS", 0U) == 0U;
    if (flood_env_unsigned("LLAM_SERVER_FLOOD_FAIL_ON_MISSING_STATS", 0U) != 0U) {
        opts->fail_on_missing_stats = true;
    }

#define FLOOD_PARSE_ARG(expr)             \
    do {                                  \
        if (!(expr)) {                    \
            flood_usage(argv[0]);         \
            return -1;                    \
        }                                 \
    } while (0)

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            opts->server_path = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            opts->host = argv[++i];
        } else if (strcmp(argv[i], "--clients") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_unsigned_value(argv[++i], &opts->clients));
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->duration_sec));
        } else if (strcmp(argv[i], "--drain-sec") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->drain_sec));
        } else if (strcmp(argv[i], "--message-bytes") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_unsigned_value(argv[++i], &opts->message_bytes));
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_unsigned_value(argv[++i], &opts->batch));
        } else if (strcmp(argv[i], "--target-mps") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->target_mps));
        } else if (strcmp(argv[i], "--min-delivery-mps") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->min_delivery_mps));
        } else if (strcmp(argv[i], "--min-delivery-ratio") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->min_delivery_ratio));
        } else if (strcmp(argv[i], "--shutdown-timeout") == 0 && i + 1 < argc) {
            FLOOD_PARSE_ARG(flood_parse_double_value(argv[++i], &opts->shutdown_timeout_sec));
        } else if (strcmp(argv[i], "--server-lossless") == 0) {
            opts->server_lossless = true;
        } else if (strcmp(argv[i], "--server-best-effort") == 0) {
            opts->server_lossless = false;
        } else if (strcmp(argv[i], "--allow-forced-stop") == 0) {
            opts->fail_on_forced_stop = false;
        } else if (strcmp(argv[i], "--fail-on-forced-stop") == 0) {
            opts->fail_on_forced_stop = true;
        } else if (strcmp(argv[i], "--allow-missing-stats") == 0) {
            opts->fail_on_missing_stats = false;
        } else if (strcmp(argv[i], "--fail-on-missing-stats") == 0) {
            opts->fail_on_missing_stats = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            flood_usage(argv[0]);
            exit(0);
        } else {
            flood_usage(argv[0]);
            return -1;
        }
    }

#undef FLOOD_PARSE_ARG

    if (opts->clients < 2U || opts->duration_sec <= 0.0 || opts->drain_sec < 0.0 ||
        opts->message_bytes < 2U || opts->message_bytes > FLOOD_MAX_MESSAGE_BYTES ||
        opts->batch == 0U || opts->target_mps < 0.0 || opts->min_delivery_mps < 0.0 ||
        opts->min_delivery_ratio < 0.0 || opts->min_delivery_ratio > 1.0 ||
        opts->shutdown_timeout_sec <= 0.0) {
        flood_usage(argv[0]);
        return -1;
    }
    return 0;
}

static int flood_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int flood_find_free_port(const char *host) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int port;

    if (fd < 0) { return -1; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (bind(fd, (struct sockaddr *)(void *)&addr, sizeof(addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)(void *)&addr, &len) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static pid_t flood_start_server(const char *server_path,
                                int port,
                                const char *stats_path,
                                const char *dump_path,
                                bool lossless) {
    char port_arg[32];
    const char *mode_arg = lossless ? "--lossless" : "--best-effort";
    pid_t pid;

    snprintf(port_arg, sizeof(port_arg), "%d", port);
    pid = fork();
    if (pid != 0) {
        return pid;
    }

    int devnull = open("/dev/null", O_WRONLY);

    if (devnull >= 0) {
        (void)dup2(devnull, STDOUT_FILENO);
        (void)dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    if (stats_path != NULL && stats_path[0] != '\0') {
        (void)setenv("LLAM_CHAT_STATS_PATH", stats_path, 1);
    }
    if (dump_path != NULL && dump_path[0] != '\0') {
        (void)setenv("LLAM_CHAT_DUMP_ON_STOP", dump_path, 1);
    }
    /*
     * Pass the server mode explicitly.  "lossless" and "throughput" flood
     * phases have different pass/fail semantics, so inheriting the example's
     * default would make benchmark logs ambiguous.
     */
    execl(server_path, server_path, mode_arg, port_arg, (char *)NULL);
    _exit(127);
}

static bool flood_read_server_stats_retry(const char *stats_path, flood_server_stats_t *stats) {
    uint64_t deadline = flood_now_ns() + 2000000000ULL;

    do {
        if (flood_read_server_stats(stats_path, stats)) { return true; }
        usleep(10000);
    } while (flood_now_ns() < deadline);

    return false;
}

static bool flood_server_running(pid_t pid) {
    int status;
    pid_t rc = waitpid(pid, &status, WNOHANG);

    if (rc == 0) {
        return true;
    }
    if (rc == pid) {
        if (WIFEXITED(status)) {
            fprintf(stderr, "server exited early with status %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "server exited early from signal %d\n", WTERMSIG(status));
        }
        return false;
    }
    return errno == EINTR;
}

static void flood_report_abnormal_server_status(const char *context, int status) {
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "server %s with status %d\n", context, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "server %s from signal %d\n", context, WTERMSIG(status));
    }
}

static bool flood_stop_server(pid_t pid, double timeout_sec) {
    uint64_t deadline;
    int status;

    if (pid <= 0) { return false; }
    if (waitpid(pid, &status, WNOHANG) == pid) {
        flood_report_abnormal_server_status("exited before cleanup", status);
        return false;
    }
    (void)kill(pid, SIGINT);
    deadline = flood_now_ns() + (uint64_t)(timeout_sec * 1000000000.0);
    while (flood_now_ns() < deadline) {
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            flood_report_abnormal_server_status("stopped during cleanup", status);
            return false;
        }
        if (rc < 0 && errno != EINTR) {
            return false;
        }
        usleep(10000);
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &status, 0);
    return true;
}

static int flood_connect_client(const char *host, int port, uint64_t deadline_ns) {
    struct sockaddr_in addr;
    int fd = -1;
    int one = 1;
    int buf_size = 4 * 1024 * 1024;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    while (flood_now_ns() < deadline_ns) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        if (connect(fd, (struct sockaddr *)(void *)&addr, sizeof(addr)) == 0) {
            if (flood_set_nonblocking(fd) != 0) {
                int saved_errno = errno;

                close(fd);
                errno = saved_errno;
                return -1;
            }
            return fd;
        }
        close(fd);
        usleep(10000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static void flood_close_clients(flood_client_t *clients, unsigned count) {
    for (unsigned i = 0U; i < count; ++i) {
        if (clients[i].fd >= 0) {
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }
}

#if FLOOD_HAVE_AVX512
static bool flood_cpu_has_avx512f(void) {
    static int cached = -1;

    if (cached < 0) {
        cached = __builtin_cpu_supports("avx512f") != 0 ? 1 : 0;
    }
    return cached != 0;
}

static bool flood_cpu_has_avx512bw(void) {
    static int cached = -1;

    if (cached < 0) {
        cached = __builtin_cpu_supports("avx512bw") != 0 ? 1 : 0;
    }
    return cached != 0;
}

__attribute__((target("avx512f,avx512bw")))
static uint64_t flood_count_newlines_avx512(const unsigned char *bytes, size_t len, size_t *off_out) {
    const __m512i newline = _mm512_set1_epi8('\n');
    uint64_t count = 0U;
    size_t off = *off_out;

    while (off + 64U <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(bytes + off));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, newline);

        count += (uint64_t)__builtin_popcountll((unsigned long long)mask);
        off += 64U;
    }
    *off_out = off;
    return count;
}

__attribute__((target("avx512f")))
static size_t flood_fill_message_avx512(char *message,
                                        size_t payload_len,
                                        const unsigned char *pattern,
                                        size_t *phase_inout) {
    size_t off = 0U;
    size_t phase = *phase_inout;

    while (off + 64U <= payload_len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(pattern + phase));

        _mm512_storeu_si512((void *)(message + off), chunk);
        off += 64U;
        phase += 12U; // 64 bytes advances 12 positions in a 26-byte alphabet.
        if (phase >= 26U) {
            phase -= 26U;
        }
    }
    *phase_inout = phase;
    return off;
}
#endif

static uint64_t flood_count_newlines(const char *buf, size_t len) {
    const unsigned char *bytes = (const unsigned char *)(const void *)buf;
    uint64_t count = 0U;
    size_t off = 0U;

#if FLOOD_HAVE_AVX512
    if (flood_cpu_has_avx512bw()) {
        count += flood_count_newlines_avx512(bytes, len, &off);
    }
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    const uint8x16_t newline = vdupq_n_u8((uint8_t)'\n');

    while (off + 16U <= len) {
        uint8x16_t chunk = vld1q_u8(bytes + off);
        uint8x16_t matches = vceqq_u8(chunk, newline);
        uint8x16_t ones = vandq_u8(matches, vdupq_n_u8(1U));

        count += (uint64_t)vaddvq_u8(ones);
        off += 16U;
    }
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
    const __m128i newline = _mm_set1_epi8('\n');

    while (off + 16U <= len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(bytes + off));
        __m128i matches = _mm_cmpeq_epi8(chunk, newline);
        unsigned mask = (unsigned)_mm_movemask_epi8(matches);

        count += (uint64_t)__builtin_popcount(mask);
        off += 16U;
    }
#endif

    while (off < len) {
        if (bytes[off] == (unsigned char)'\n') {
            count += 1U;
        }
        off += 1U;
    }
    return count;
}

static uint64_t flood_drain_client(flood_client_t *client) {
    char buf[FLOOD_RECV_BUF_BYTES];
    uint64_t lines = 0U;

    if (client->closed || client->fd < 0) {
        return 0U;
    }
    for (;;) {
        ssize_t nread = recv(client->fd, buf, sizeof(buf), 0);

        if (nread > 0) {
            uint64_t new_lines = flood_count_newlines(buf, (size_t)nread);

            client->recv_bytes += (uint64_t)nread;
            client->recv_lines += new_lines;
            lines += new_lines;
            continue;
        }
        if (nread == 0) {
            client->closed = true;
            return lines;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return lines;
        }
        client->closed = true;
        return lines;
    }
}

static uint64_t flood_send_client(flood_client_t *client, const char *message, size_t message_len, unsigned batch) {
    uint64_t completed = 0U;

    if (client->closed || client->fd < 0) {
        return 0U;
    }
    while (completed < batch) {
        ssize_t nwritten = send(client->fd,
                                message + client->write_off,
                                message_len - client->write_off,
                                MSG_NOSIGNAL);

        if (nwritten > 0) {
            client->write_off += (size_t)nwritten;
            if (client->write_off == message_len) {
                client->write_off = 0U;
                client->sent_messages += 1U;
                completed += 1U;
            }
            continue;
        }
        if (nwritten < 0 && errno == EINTR) {
            continue;
        }
        if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return completed;
        }
        client->closed = true;
        return completed;
    }
    return completed;
}

static void flood_poll_once(flood_client_t *clients, struct pollfd *fds, unsigned count) {
    nfds_t nfds = 0;

    for (unsigned i = 0U; i < count; ++i) {
        if (!clients[i].closed && clients[i].fd >= 0) {
            fds[nfds].fd = clients[i].fd;
            fds[nfds].events = POLLIN | POLLOUT;
            fds[nfds].revents = 0;
            nfds += 1U;
        }
    }
    if (nfds > 0U) {
        (void)poll(fds, nfds, 1);
    }
}

static void flood_build_message(char *message, size_t len) {
    static const unsigned char pattern[] =
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
    size_t payload_len = len - 1U;
    size_t off = 0U;
    size_t phase = 0U;

#if FLOOD_HAVE_AVX512
    if (flood_cpu_has_avx512f()) {
        off = flood_fill_message_avx512(message, payload_len, pattern, &phase);
    }
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    while (off + 16U <= payload_len) {
        uint8x16_t chunk = vld1q_u8(pattern + phase);

        vst1q_u8((uint8_t *)(void *)(message + off), chunk);
        off += 16U;
        phase += 16U;
        if (phase >= 26U) {
            phase -= 26U;
        }
    }
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
    while (off + 16U <= payload_len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(pattern + phase));

        _mm_storeu_si128((__m128i *)(void *)(message + off), chunk);
        off += 16U;
        phase += 16U;
        if (phase >= 26U) {
            phase -= 26U;
        }
    }
#endif

    while (off < payload_len) {
        message[off] = (char)('a' + (int)phase);
        off += 1U;
        phase += 1U;
        if (phase >= 26U) {
            phase = 0U;
        }
    }
    message[payload_len] = '\n';
}

int main(int argc, char **argv) {
    flood_opts_t opts;
    flood_client_t *clients = NULL;
    struct pollfd *fds = NULL;
    char message[FLOOD_MAX_MESSAGE_BYTES];
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t active_end_ns;
    uint64_t drain_end_ns;
    uint64_t total_sent = 0U;
    uint64_t total_recv_lines = 0U;
    uint64_t total_recv_bytes = 0U;
    uint64_t expected_deliveries = 0U;
    uint64_t missing_deliveries = 0U;
    uint64_t send_cursor = 0U;
    uint64_t loops_without_progress = 0U;
    pid_t server_pid = -1;
    char stats_dir[256] = "";
    char stats_path[256] = "";
    char dump_path[512] = "";
    const char *tmp_root;
    const char *dump_dir;
    int path_len;
    int port;
    int rc = 1;

    signal(SIGPIPE, SIG_IGN);
    if (flood_parse_args(argc, argv, &opts) != 0) {
        return 2;
    }

    clients = calloc(opts.clients, sizeof(*clients));
    fds = calloc(opts.clients, sizeof(*fds));
    if (clients == NULL || fds == NULL) {
        perror("calloc");
        goto done;
    }
    for (unsigned i = 0U; i < opts.clients; ++i) {
        clients[i].fd = -1;
    }

    port = flood_find_free_port(opts.host);
    if (port < 0) {
        perror("find free port");
        goto done;
    }
    tmp_root = getenv("TMPDIR");
    if (tmp_root == NULL || tmp_root[0] == '\0') { tmp_root = "/tmp"; }
    path_len = snprintf(stats_dir, sizeof(stats_dir), "%s/llam_server_flood_XXXXXX", tmp_root);
    if (path_len < 0 || path_len >= (int)sizeof(stats_dir)) {
        errno = ENAMETOOLONG;
        perror("stats dir path");
        goto done;
    }
    if (mkdtemp(stats_dir) == NULL) {
        perror("mkdtemp stats dir");
        goto done;
    }
    /* Private 0700 dir prevents untrusted path replacement before stats write. */
    path_len = snprintf(stats_path, sizeof(stats_path), "%s/stats.txt", stats_dir);
    if (path_len < 0 || path_len >= (int)sizeof(stats_path)) {
        errno = ENAMETOOLONG;
        perror("stats path");
        goto done;
    }
    dump_dir = getenv("LLAM_SERVER_FLOOD_DUMP_DIR");
    if (dump_dir == NULL || dump_dir[0] == '\0') {
        dump_dir = getenv("LLAM_SERVER_COMPOSITE_DUMP_DIR");
    }
    if (dump_dir == NULL || dump_dir[0] == '\0') {
        dump_dir = getenv("OUT_DIR");
    }
    if (dump_dir != NULL && dump_dir[0] != '\0') {
        path_len = snprintf(dump_path, sizeof(dump_path), "%s/server-flood-runtime-dump-%ld-%d.log", dump_dir, (long)getpid(), port);
        if (path_len < 0 || path_len >= (int)sizeof(dump_path)) {
            errno = ENAMETOOLONG;
            perror("dump path");
            goto done;
        }
    }
    server_pid = flood_start_server(opts.server_path, port, stats_path, dump_path, opts.server_lossless);
    if (server_pid < 0) {
        perror("fork server");
        goto done;
    }

    uint64_t connect_deadline = flood_now_ns() + 10000000000ULL;

    for (unsigned i = 0U; i < opts.clients; ++i) {
        clients[i].fd = flood_connect_client(opts.host, port, connect_deadline);
        if (clients[i].fd < 0) {
            perror("connect client");
            goto done;
        }
    }

    usleep(250000);
    for (unsigned pass = 0U; pass < 16U; ++pass) {
        for (unsigned i = 0U; i < opts.clients; ++i) {
            (void)flood_drain_client(&clients[i]);
        }
        usleep(1000);
    }
    for (unsigned i = 0U; i < opts.clients; ++i) {
        clients[i].recv_lines = 0U;
        clients[i].recv_bytes = 0U;
    }

    flood_build_message(message, opts.message_bytes);
    start_ns = flood_now_ns();
    end_ns = start_ns + (uint64_t)(opts.duration_sec * 1000000000.0);

    while (flood_now_ns() < end_ns) {
        bool progressed = false;
        uint64_t now_ns;
        uint64_t send_budget = UINT64_MAX;

        if (!flood_server_running(server_pid)) {
            goto done;
        }
        for (unsigned i = 0U; i < opts.clients; ++i) {
            if (flood_drain_client(&clients[i]) != 0U) {
                progressed = true;
            }
        }

        now_ns = flood_now_ns();
        if (opts.target_mps > 0.0) {
            double elapsed = (double)(now_ns - start_ns) / 1000000000.0;
            double allowed = opts.target_mps * 1000000.0 * elapsed - (double)total_sent;

            if (allowed <= 0.0) {
                send_budget = 0U;
            } else if (allowed < (double)UINT32_MAX) {
                send_budget = (uint64_t)allowed;
            }
        }

        while (send_budget > 0U && flood_now_ns() < end_ns) {
            flood_client_t *client = &clients[send_cursor % opts.clients];
            uint64_t sent;

            send_cursor += 1U;
            sent = flood_send_client(client, message, opts.message_bytes, opts.batch);
            if (sent != 0U) {
                total_sent += sent;
                progressed = true;
                if (send_budget != UINT64_MAX) {
                    send_budget = sent >= send_budget ? 0U : send_budget - sent;
                }
            } else {
                if (send_cursor % opts.clients == 0U) {
                    break;
                }
            }
            if (send_cursor % opts.clients == 0U && opts.target_mps > 0.0) {
                break;
            }
        }

        if (!progressed) {
            loops_without_progress += 1U;
            if ((loops_without_progress & 15ULL) == 0ULL) {
                flood_poll_once(clients, fds, opts.clients);
            }
        } else {
            loops_without_progress = 0U;
        }
    }

    active_end_ns = flood_now_ns();
    drain_end_ns = flood_now_ns() + (uint64_t)(opts.drain_sec * 1000000000.0);
    while (flood_now_ns() < drain_end_ns) {
        bool progressed = false;

        for (unsigned i = 0U; i < opts.clients; ++i) {
            if (flood_drain_client(&clients[i]) != 0U) {
                progressed = true;
            }
        }
        if (!progressed) {
            flood_poll_once(clients, fds, opts.clients);
        }
    }

    for (unsigned i = 0U; i < opts.clients; ++i) {
        total_recv_lines += clients[i].recv_lines;
        total_recv_bytes += clients[i].recv_bytes;
    }

    double active_sec = (double)(active_end_ns - start_ns) / 1000000000.0;
    double measured_sec = (double)(flood_now_ns() - start_ns) / 1000000000.0;
    double sent_mps = active_sec > 0.0 ? (double)total_sent / active_sec / 1000000.0 : 0.0;
    double sent_total_mps = measured_sec > 0.0 ? (double)total_sent / measured_sec / 1000000.0 : 0.0;
    double delivery_mps = measured_sec > 0.0 ? (double)total_recv_lines / measured_sec / 1000000.0 : 0.0;
    expected_deliveries = total_sent * (uint64_t)(opts.clients - 1U);
    missing_deliveries = expected_deliveries > total_recv_lines ? expected_deliveries - total_recv_lines : 0U;
    double delivery_ratio = expected_deliveries > 0U ? (double)total_recv_lines / (double)expected_deliveries : 0.0;

    printf("server flood ok: server_mode=%s clients=%u active_sec=%.3f total_sec=%.3f sent=%" PRIu64
           " inbound_mps=%.3f inbound_total_mps=%.3f expected_deliveries=%" PRIu64
           " observed_deliveries=%" PRIu64 " missing_deliveries=%" PRIu64
           " delivery_mps=%.3f delivery_ratio=%.9f recv_bytes=%" PRIu64 "\n",
           opts.server_lossless ? "lossless" : "best-effort", opts.clients, active_sec, measured_sec,
           total_sent, sent_mps, sent_total_mps, expected_deliveries, total_recv_lines, missing_deliveries,
           delivery_mps, delivery_ratio, total_recv_bytes);

    if (total_sent == 0U || total_recv_lines == 0U) {
        fprintf(stderr, "flood produced no measurable traffic\n");
        goto done;
    }
    if (opts.min_delivery_mps > 0.0 && delivery_mps < opts.min_delivery_mps) {
        fprintf(stderr, "delivery_mps %.3f below required %.3f\n", delivery_mps, opts.min_delivery_mps);
        goto done;
    }
    if (opts.min_delivery_ratio > 0.0 && expected_deliveries > 0U && delivery_ratio < opts.min_delivery_ratio) {
        fprintf(stderr, "delivery_ratio %.9f below required %.9f; missing_deliveries=%" PRIu64 "\n",
                delivery_ratio, opts.min_delivery_ratio, missing_deliveries);
        goto done;
    }
    rc = 0;

done:
    if (clients != NULL) {
        flood_close_clients(clients, opts.clients);
    }
    if (server_pid > 0) {
        bool killed = flood_stop_server(server_pid, opts.shutdown_timeout_sec);
        flood_server_stats_t stats;

        if (flood_read_server_stats_retry(stats_path, &stats)) {
            intmax_t accounting_gap;
            intmax_t tolerance;

            flood_print_server_stats(&stats);
            accounting_gap = flood_print_accounting(&stats, expected_deliveries, total_recv_lines, missing_deliveries);
            tolerance = flood_accounting_tolerance(expected_deliveries);
            if (stats.outbox_closed_drops != 0U && rc == 0) {
                fprintf(stderr,
                        "outbox_closed_drops=%" PRIu64 " indicates cleanup race or late enqueue to closed clients\n",
                        stats.outbox_closed_drops);
                rc = 1;
            }
            if (flood_abs_imax(accounting_gap) > tolerance && rc == 0) {
                fprintf(stderr, "accounting_gap=%" PRIdMAX " exceeds tolerance=%" PRIdMAX "\n",
                        accounting_gap, tolerance);
                rc = 1;
            }
        } else {
            printf("server flood stats: unavailable\n");
            if (opts.fail_on_missing_stats && rc == 0) {
                fprintf(stderr, "server stats unavailable\n");
                rc = 1;
            }
        }
        if (killed) {
            fprintf(stderr, "server did not stop within %.1fs after SIGINT; killed\n", opts.shutdown_timeout_sec);
            if (dump_path[0] != '\0') {
                fprintf(stderr, "server runtime dump path: %s\n", dump_path);
            }
            if (opts.fail_on_forced_stop && rc == 0) {
                rc = 1;
            }
        }
    }
    if (stats_path[0] != '\0') { (void)unlink(stats_path); }
    if (stats_dir[0] != '\0') { (void)rmdir(stats_dir); }
    free(fds);
    free(clients);
    return rc;
}

#endif
