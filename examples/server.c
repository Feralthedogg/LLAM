/**
 * @file examples/server.c
 * @brief Minimal LLAM-backed TCP chat server.
 *
 * @details
 * This example keeps the network API blocking-looking while LLAM parks tasks
 * behind @c llam_accept, @c llam_read, @c llam_write, mutexes, and channels.
 * Run it with:
 *
 *     ./server 7777
 *
 * Then connect from multiple terminals:
 *
 *     nc 127.0.0.1 7777
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

#include "llam/runtime.h"

#include <stdio.h>

#if LLAM_PLATFORM_POSIX
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CHAT_DEFAULT_PORT 7777U
#define CHAT_BACKLOG 128
#define CHAT_OUTBOX_CAP 64U
#define CHAT_READ_BUF 2048U
#define CHAT_MAX_BROADCAST_TARGETS 1024U
// Broadcast fanout should shed slow receivers instead of letting one client
// build a shutdown-sized backlog.
#define CHAT_SEND_TIMEOUT_NS 0ULL

#if !LLAM_PLATFORM_POSIX
int main(void) {
    fprintf(stderr, "server example requires a POSIX socket backend in this build\n");
    return 2;
}
#else

typedef struct chat_message {
    size_t len;
    char data[];
} chat_message_t;

typedef struct chat_client chat_client_t;

typedef struct chat_server {
    atomic_int listener_fd;
    sigset_t stop_signals;
    pthread_t signal_thread;
    bool signal_thread_started;
    bool quiet;
    llam_cancel_token_t *stop_token;
    pthread_mutex_t clients_lock;
    bool clients_lock_initialized;
    chat_client_t *clients;
    atomic_uint next_client_id;
} chat_server_t;

struct chat_client {
    chat_server_t *server;
    atomic_int fd;
    unsigned id;
    char peer[96];
    llam_channel_t *outbox;
    atomic_uint refs;
    atomic_uint closing;
    chat_client_t *next;
};

static atomic_bool g_stop_requested;

static void chat_client_close_fd(chat_client_t *client);

static void chat_client_retain(chat_client_t *client) {
    (void)atomic_fetch_add_explicit(&client->refs, 1U, memory_order_relaxed);
}

static void chat_client_release(chat_client_t *client) {
    if (atomic_fetch_sub_explicit(&client->refs, 1U, memory_order_acq_rel) != 1U) {
        return;
    }
    chat_client_close_fd(client);
    if (client->outbox != NULL) {
        (void)llam_channel_destroy(client->outbox);
    }
    free(client);
}

static chat_message_t *chat_message_create(const char *data, size_t len) {
    chat_message_t *message;

    message = malloc(sizeof(*message) + len);
    if (message == NULL) {
        return NULL;
    }
    message->len = len;
    if (len > 0U) {
        memcpy(message->data, data, len);
    }
    return message;
}

static chat_message_t *chat_message_printf(const char *prefix, unsigned id, const char *suffix) {
    char stack_buf[256];
    int len;

    len = snprintf(stack_buf, sizeof(stack_buf), prefix, id, suffix);
    if (len < 0) {
        return NULL;
    }
    if ((size_t)len >= sizeof(stack_buf)) {
        len = (int)sizeof(stack_buf) - 1;
    }
    return chat_message_create(stack_buf, (size_t)len);
}

static int chat_client_fd(const chat_client_t *client) {
    return atomic_load_explicit(&client->fd, memory_order_acquire);
}

static void chat_client_shutdown_fd(chat_client_t *client) {
    int fd = chat_client_fd(client);

    if (fd >= 0) {
        (void)shutdown(fd, SHUT_RDWR);
    }
}

static void chat_client_close_fd(chat_client_t *client) {
    int fd = atomic_exchange_explicit(&client->fd, -1, memory_order_acq_rel);

    if (fd >= 0) {
        (void)shutdown(fd, SHUT_RDWR);
        (void)close(fd);
    }
}

static int chat_write_all(chat_client_t *client, const char *data, size_t len) {
    size_t off = 0U;

    while (off < len) {
        int fd = chat_client_fd(client);
        ssize_t nwritten;

        if (fd < 0) {
            return -1;
        }
        nwritten = llam_write(fd, data + off, len - off);
        if (nwritten > 0) {
            off += (size_t)nwritten;
            continue;
        }
        if (nwritten < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int chat_server_add_client(chat_server_t *server, chat_client_t *client) {
    if (pthread_mutex_lock(&server->clients_lock) != 0) {
        return -1;
    }
    chat_client_retain(client);
    client->next = server->clients;
    server->clients = client;
    (void)pthread_mutex_unlock(&server->clients_lock);
    return 0;
}

static void chat_server_remove_client(chat_server_t *server, chat_client_t *client) {
    chat_client_t **cursor;
    bool removed = false;

    if (pthread_mutex_lock(&server->clients_lock) != 0) {
        return;
    }
    cursor = &server->clients;
    while (*cursor != NULL) {
        if (*cursor == client) {
            *cursor = client->next;
            client->next = NULL;
            removed = true;
            break;
        }
        cursor = &(*cursor)->next;
    }
    (void)pthread_mutex_unlock(&server->clients_lock);
    if (removed) {
        chat_client_release(client);
    }
}

static void chat_client_begin_close(chat_client_t *client) {
    if (atomic_exchange_explicit(&client->closing, 1U, memory_order_acq_rel) != 0U) {
        return;
    }
    chat_server_remove_client(client->server, client);
    (void)llam_channel_close(client->outbox);
    chat_client_shutdown_fd(client);
}

static void chat_server_close_all(chat_server_t *server) {
    for (;;) {
        chat_client_t *client;

        if (pthread_mutex_lock(&server->clients_lock) != 0) {
            return;
        }
        client = server->clients;
        if (client != NULL) {
            server->clients = client->next;
            client->next = NULL;
        }
        (void)pthread_mutex_unlock(&server->clients_lock);

        if (client == NULL) {
            return;
        }
        if (atomic_exchange_explicit(&client->closing, 1U, memory_order_acq_rel) == 0U) {
            (void)llam_channel_close(client->outbox);
            chat_client_shutdown_fd(client);
        }
        // Drop the list reference removed above.
        chat_client_release(client);
    }
}

static void chat_request_stop(chat_server_t *server) {
    int fd;

    atomic_store_explicit(&g_stop_requested, true, memory_order_release);
    if (!server->quiet) {
        fprintf(stdout, "shutdown requested\n");
        fflush(stdout);
    }
    if (server->stop_token != NULL) {
        (void)llam_cancel_token_cancel(server->stop_token);
    }
    fd = atomic_exchange_explicit(&server->listener_fd, -1, memory_order_acq_rel);
    if (fd >= 0) {
        (void)close(fd);
    }
    chat_server_close_all(server);
    (void)llam_runtime_request_stop();
}

static void chat_queue_message(chat_client_t *client, chat_message_t *message) {
    uint64_t deadline;

    if (message == NULL) {
        return;
    }
    if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
        free(message);
        return;
    }
    deadline = llam_now_ns() + CHAT_SEND_TIMEOUT_NS;
    if (llam_channel_send_until(client->outbox, message, deadline) != 0) {
        free(message);
    }
}

static void chat_queue_text(chat_client_t *client, const char *text) {
    chat_queue_message(client, chat_message_create(text, strlen(text)));
}

static void chat_broadcast(chat_server_t *server, const chat_client_t *sender, const char *data, size_t len) {
    chat_client_t *targets[CHAT_MAX_BROADCAST_TARGETS];
    unsigned count = 0U;
    unsigned i;

    if (pthread_mutex_lock(&server->clients_lock) != 0) {
        return;
    }
    for (chat_client_t *client = server->clients;
         client != NULL && count < CHAT_MAX_BROADCAST_TARGETS;
         client = client->next) {
        if (client == sender || atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
            continue;
        }
        chat_client_retain(client);
        targets[count++] = client;
    }
    (void)pthread_mutex_unlock(&server->clients_lock);

    for (i = 0U; i < count; ++i) {
        chat_message_t *message = chat_message_create(data, len);

        chat_queue_message(targets[i], message);
        chat_client_release(targets[i]);
    }
}

static void chat_broadcast_system(chat_server_t *server, const chat_client_t *client, const char *event) {
    chat_message_t *message = chat_message_printf("* client %u %s\n", client->id, event);

    if (message == NULL) {
        return;
    }
    chat_broadcast(server, client, message->data, message->len);
    if (!server->quiet) {
        fprintf(stdout, "%.*s", (int)message->len, message->data);
        fflush(stdout);
    }
    free(message);
}

static void chat_writer_drop_backlog(chat_client_t *client) {
    for (;;) {
        chat_message_t *message = NULL;

        if (llam_channel_recv_until_result(client->outbox, 0U, (void **)&message) != 0) {
            return;
        }
        free(message);
    }
}

static void chat_writer_task(void *arg) {
    chat_client_t *client = arg;
    bool writable = true;

    for (;;) {
        chat_message_t *message = NULL;

        if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
            chat_writer_drop_backlog(client);
            break;
        }
        if (llam_channel_recv_result(client->outbox, (void **)&message) != 0) {
            break;
        }
        if (message != NULL) {
            if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
                free(message);
                chat_writer_drop_backlog(client);
                break;
            }
            if (writable && chat_write_all(client, message->data, message->len) != 0) {
                writable = false;
                chat_client_begin_close(client);
            }
            free(message);
        }
    }

    chat_client_release(client);
}

static void chat_reader_task(void *arg) {
    chat_client_t *client = arg;
    char buf[CHAT_READ_BUF];

    chat_broadcast_system(client->server, client, "joined");
    for (;;) {
        int fd = chat_client_fd(client);
        ssize_t nread;

        if (fd < 0) {
            break;
        }
        nread = llam_read(fd, buf, sizeof(buf));

        if (nread > 0) {
            char prefix[64];
            char line[sizeof(prefix) + CHAT_READ_BUF];
            int prefix_len = snprintf(prefix, sizeof(prefix), "[client %u] ", client->id);
            size_t copy_len;

            if (prefix_len < 0) {
                continue;
            }
            if ((size_t)prefix_len >= sizeof(prefix)) {
                prefix_len = (int)sizeof(prefix) - 1;
            }
            copy_len = (size_t)nread;
            if ((size_t)prefix_len + copy_len > sizeof(line)) {
                copy_len = sizeof(line) - (size_t)prefix_len;
            }
            memcpy(line, prefix, (size_t)prefix_len);
            memcpy(line + prefix_len, buf, copy_len);
            chat_broadcast(client->server, client, line, (size_t)prefix_len + copy_len);
            continue;
        }
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    chat_broadcast_system(client->server, client, "left");
    chat_client_begin_close(client);
    chat_client_release(client);
}

static void chat_peer_name(const struct sockaddr_storage *addr, socklen_t addrlen, char *out, size_t out_size) {
    const void *src = NULL;
    unsigned port = 0U;
    char ip[INET6_ADDRSTRLEN];

    (void)addrlen;
    if (out_size == 0U) {
        return;
    }
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)addr;

        src = &in->sin_addr;
        port = ntohs(in->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)addr;

        src = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
    }
    if (src == NULL || inet_ntop(addr->ss_family, src, ip, sizeof(ip)) == NULL) {
        (void)snprintf(out, out_size, "unknown");
        return;
    }
    (void)snprintf(out, out_size, "%s:%u", ip, port);
}

static chat_client_t *chat_client_create(chat_server_t *server,
                                         int fd,
                                         const struct sockaddr_storage *peer_addr,
                                         socklen_t peer_len) {
    chat_client_t *client = calloc(1U, sizeof(*client));

    if (client == NULL) {
        return NULL;
    }
    client->server = server;
    atomic_init(&client->fd, fd);
    client->id = atomic_fetch_add_explicit(&server->next_client_id, 1U, memory_order_relaxed) + 1U;
    client->outbox = llam_channel_create(CHAT_OUTBOX_CAP);
    // The accept loop owns the initial local reference. The client list and
    // spawned reader/writer tasks take explicit additional references.
    atomic_init(&client->refs, 1U);
    atomic_init(&client->closing, 0U);
    chat_peer_name(peer_addr, peer_len, client->peer, sizeof(client->peer));
    if (client->outbox == NULL) {
        free(client);
        return NULL;
    }
    return client;
}

static void chat_accept_task(void *arg) {
    chat_server_t *server = arg;

    while (!atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int listener_fd = atomic_load_explicit(&server->listener_fd, memory_order_acquire);
        int fd;
        chat_client_t *client;
        llam_task_t *reader;
        llam_task_t *writer;
        llam_spawn_opts_t client_opts;

        if (listener_fd < 0) {
            break;
        }
        memset(&peer_addr, 0, sizeof(peer_addr));
        fd = llam_accept(listener_fd, NULL, NULL);
        if (LLAM_FD_IS_INVALID(fd)) {
            if (atomic_load_explicit(&g_stop_requested, memory_order_acquire) ||
                errno == EBADF ||
                errno == ECANCELED) {
                break;
            }
            perror("llam_accept");
            (void)llam_sleep_ns(10ULL * 1000ULL * 1000ULL);
            continue;
        }
        if (getpeername(fd, (struct sockaddr *)(void *)&peer_addr, &peer_len) != 0) {
            memset(&peer_addr, 0, sizeof(peer_addr));
            peer_len = 0;
        }

        client = chat_client_create(server, fd, &peer_addr, peer_len);
        if (client == NULL) {
            perror("chat_client_create");
            (void)close(fd);
            continue;
        }
        if (chat_server_add_client(server, client) != 0) {
            perror("chat_server_add_client");
            chat_client_release(client);
            continue;
        }
        if (llam_spawn_opts_init(&client_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
            perror("llam_spawn_opts_init client");
            chat_client_begin_close(client);
            chat_client_release(client);
            continue;
        }
        client_opts.cancel_token = server->stop_token;
        client_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_HUGE;

        chat_client_retain(client);
        writer = llam_spawn_ex(chat_writer_task, client, &client_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
        if (writer == NULL) {
            perror("llam_spawn writer");
            chat_client_begin_close(client);
            chat_client_release(client);
            chat_client_release(client);
            continue;
        }
        if (llam_detach(writer) != 0) {
            perror("llam_detach writer");
            chat_client_begin_close(client);
            chat_client_release(client);
            continue;
        }

        chat_client_retain(client);
        reader = llam_spawn_ex(chat_reader_task, client, &client_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
        if (reader == NULL) {
            perror("llam_spawn reader");
            chat_client_begin_close(client);
            chat_client_release(client);
            chat_client_release(client);
            continue;
        }
        if (llam_detach(reader) != 0) {
            perror("llam_detach reader");
            chat_client_begin_close(client);
            chat_client_release(client);
            continue;
        }

        if (!server->quiet) {
            fprintf(stdout, "client %u connected from %s\n", client->id, client->peer);
            fflush(stdout);
        }
        chat_queue_text(client, "Welcome to LLAM chat. Type and press enter.\n");
        chat_client_release(client);
    }

    chat_server_close_all(server);
    (void)llam_runtime_request_stop();
}

static void *chat_signal_thread_main(void *arg) {
    chat_server_t *server = arg;
    int signo = 0;

    if (sigwait(&server->stop_signals, &signo) == 0) {
        (void)signo;
        chat_request_stop(server);
    }
    return NULL;
}

static int chat_create_listener(uint16_t port, bool public_bind) {
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(public_bind ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)(void *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, CHAT_BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int chat_start_signal_thread(chat_server_t *server) {
    int err;

    err = pthread_create(&server->signal_thread, NULL, chat_signal_thread_main, server);
    if (err != 0) {
        errno = err;
        return -1;
    }
    server->signal_thread_started = true;
    return 0;
}

static void chat_stop_signal_thread(chat_server_t *server) {
    if (!server->signal_thread_started) {
        return;
    }
    (void)pthread_kill(server->signal_thread, SIGTERM);
    (void)pthread_join(server->signal_thread, NULL);
    server->signal_thread_started = false;
}

static void chat_destroy_clients_lock(chat_server_t *server) {
    if (!server->clients_lock_initialized) {
        return;
    }
    (void)pthread_mutex_destroy(&server->clients_lock);
    server->clients_lock_initialized = false;
}

static uint16_t chat_parse_port(const char *value) {
    char *end = NULL;
    unsigned long port;

    if (value == NULL || value[0] == '\0') {
        return CHAT_DEFAULT_PORT;
    }
    errno = 0;
    port = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || port == 0UL || port > 65535UL) {
        fprintf(stderr, "invalid port: %s\n", value);
        return 0U;
    }
    return (uint16_t)port;
}

int main(int argc, char **argv) {
    chat_server_t server;
    llam_runtime_opts_t opts;
    llam_task_t *accept_task;
    llam_spawn_opts_t accept_opts;
    const char *quiet_env;
    uint16_t port = CHAT_DEFAULT_PORT;
    bool public_bind = false;
    int rc = 0;
    int err;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--public") == 0) {
            public_bind = true;
        } else {
            port = chat_parse_port(argv[i]);
            if (port == 0U) {
                return 2;
            }
        }
    }

    memset(&server, 0, sizeof(server));
    atomic_init(&server.listener_fd, -1);
    atomic_init(&server.next_client_id, 0U);
    quiet_env = getenv("LLAM_CHAT_QUIET");
    server.quiet = quiet_env != NULL && strcmp(quiet_env, "0") != 0;
    atomic_store_explicit(&g_stop_requested, false, memory_order_release);

    sigemptyset(&server.stop_signals);
    sigaddset(&server.stop_signals, SIGINT);
    sigaddset(&server.stop_signals, SIGTERM);
    err = pthread_sigmask(SIG_BLOCK, &server.stop_signals, NULL);
    if (err != 0) {
        errno = err;
        perror("pthread_sigmask");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    memset(&opts, 0, sizeof(opts));
    opts.profile = LLAM_RUNTIME_PROFILE_IO_LATENCY;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS |
                              LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        perror("llam_runtime_init");
        return 1;
    }

    if (pthread_mutex_init(&server.clients_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        llam_runtime_shutdown();
        return 1;
    }
    server.clients_lock_initialized = true;
    server.stop_token = llam_cancel_token_create();
    if (server.stop_token == NULL) {
        perror("llam_cancel_token_create");
        chat_destroy_clients_lock(&server);
        llam_runtime_shutdown();
        return 1;
    }

    atomic_store_explicit(&server.listener_fd, chat_create_listener(port, public_bind), memory_order_release);
    if (atomic_load_explicit(&server.listener_fd, memory_order_acquire) < 0) {
        perror("chat_create_listener");
        (void)llam_cancel_token_destroy(server.stop_token);
        chat_destroy_clients_lock(&server);
        llam_runtime_shutdown();
        return 1;
    }

    if (!server.quiet) {
        fprintf(stdout,
                "LLAM chat server listening on %s:%u\n",
                public_bind ? "0.0.0.0" : "127.0.0.1",
                (unsigned)port);
        fflush(stdout);
    }

    if (chat_start_signal_thread(&server) != 0) {
        perror("chat_start_signal_thread");
        int listener_fd = atomic_exchange_explicit(&server.listener_fd, -1, memory_order_acq_rel);

        if (listener_fd >= 0) {
            close(listener_fd);
        }
        (void)llam_cancel_token_destroy(server.stop_token);
        chat_destroy_clients_lock(&server);
        llam_runtime_shutdown();
        return 1;
    }

    if (llam_spawn_opts_init(&accept_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_spawn_opts_init");
        int listener_fd = atomic_exchange_explicit(&server.listener_fd, -1, memory_order_acq_rel);

        if (listener_fd >= 0) {
            close(listener_fd);
        }
        chat_stop_signal_thread(&server);
        (void)llam_cancel_token_destroy(server.stop_token);
        chat_destroy_clients_lock(&server);
        llam_runtime_shutdown();
        return 1;
    }
    accept_opts.cancel_token = server.stop_token;
    accept_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_HUGE;
    accept_task = llam_spawn_ex(chat_accept_task, &server, &accept_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (accept_task == NULL || llam_detach(accept_task) != 0) {
        perror("llam_spawn accept");
        int listener_fd = atomic_exchange_explicit(&server.listener_fd, -1, memory_order_acq_rel);

        if (listener_fd >= 0) {
            close(listener_fd);
        }
        chat_stop_signal_thread(&server);
        (void)llam_cancel_token_destroy(server.stop_token);
        chat_destroy_clients_lock(&server);
        llam_runtime_shutdown();
        return 1;
    }

    if (llam_run() != 0) {
        perror("llam_run");
        rc = 1;
    }

    int listener_fd = atomic_exchange_explicit(&server.listener_fd, -1, memory_order_acq_rel);

    if (listener_fd >= 0) {
        close(listener_fd);
    }
    chat_stop_signal_thread(&server);
    (void)llam_cancel_token_destroy(server.stop_token);
    chat_destroy_clients_lock(&server);
    llam_runtime_shutdown();
    return rc;
}

#endif
