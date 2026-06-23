/**
 * @file examples/server.c
 * @brief Minimal LLAM-backed TCP chat server.
 *
 * @details
 * This example keeps the network API blocking-looking while LLAM parks tasks
 * behind @c llam_accept, @c llam_read, @c llam_write, mutexes, and channels.
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
#include "server_support.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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
#include <sys/uio.h>
#include <unistd.h>
#endif

#define CHAT_DEFAULT_PORT 7777U
#define CHAT_BACKLOG 128
#define CHAT_OUTBOX_CAP 1024U
#define CHAT_WRITE_BATCH 64U
#define CHAT_WRITE_IOV_CAP 128U
#define CHAT_READ_BUF 2048U
#define CHAT_INPUT_CAP 4096U
#define CHAT_MAX_BROADCAST_TARGETS 1024U
#define CHAT_PREFIX_CAP 64U
#define CHAT_SHUTDOWN_DRAIN_MS 5000U
#ifdef SIGUSR2
#define CHAT_SIGNAL_THREAD_WAKE SIGUSR2
#else
#define CHAT_SIGNAL_THREAD_WAKE SIGTERM
#endif

#ifndef LLAM_CHAT_LOSSLESS_DEFAULT
#define LLAM_CHAT_LOSSLESS_DEFAULT 0
#endif

#if !LLAM_PLATFORM_POSIX
int main(void) {
    fprintf(stderr, "server example requires a POSIX socket backend in this build\n");
    return 2;
}
#else

typedef struct chat_message {
    atomic_uint refs;
    size_t prefix_len;
    size_t data_len;
    char prefix[CHAT_PREFIX_CAP];
    char data[];
} chat_message_t;

typedef struct chat_outbox {
    /* Use LLAM sync primitives so outbox contention parks tasks, not workers. */
    llam_mutex_t *lock;
    llam_channel_t *wake;
    atomic_uint_fast64_t *full_drop_counter;
    atomic_uint_fast64_t *closed_drop_counter;
    chat_message_t *ring[CHAT_OUTBOX_CAP];
    size_t head;
    size_t tail;
    size_t count;
    atomic_bool closed;
    bool wake_pending;
    bool lossless;
} chat_outbox_t;

typedef struct chat_client chat_client_t;

typedef struct chat_server {
    atomic_int listener_fd;
    sigset_t stop_signals;
    pthread_t signal_thread;
    bool signal_thread_started;
    atomic_bool signal_thread_stop;
    atomic_bool signal_thread_done;
    bool quiet;
    bool lossless_outbox;
    llam_cancel_token_t *stop_token;
    pthread_rwlock_t clients_lock;
    bool clients_lock_initialized;
    chat_client_t *clients;
    atomic_uint next_client_id;
    atomic_uint_fast64_t outbox_full_drops;
    atomic_uint_fast64_t outbox_closed_drops;
    atomic_uint_fast64_t broadcast_messages_created;
    atomic_uint_fast64_t broadcast_deliveries_attempted;
    atomic_uint_fast64_t broadcast_deliveries_enqueued;
    atomic_uint live_clients;
} chat_server_t;

struct chat_client {
    chat_server_t *server;
    atomic_int fd;
    unsigned id;
    char peer[96];
    char prefix[CHAT_PREFIX_CAP];
    size_t prefix_len;
    char input[CHAT_INPUT_CAP];
    size_t input_len;
    chat_outbox_t outbox;
    atomic_uint refs;
    atomic_uint closing;
    /* close waits for snapshot enqueues before closing the outbox. */
    atomic_uint enqueue_refs;
    chat_client_t *next;
};

static atomic_bool g_stop_requested;

static void chat_client_close_fd(chat_client_t *client);
static void chat_client_shutdown_fd(chat_client_t *client);
static void chat_outbox_destroy(chat_outbox_t *outbox);
static void chat_dump_runtime_if_requested(const char *phase);
static void chat_print_stats(FILE *stream, const chat_server_t *server);
static void chat_write_stats_file(const chat_server_t *server);

static void chat_client_retain(chat_client_t *client) {
    (void)atomic_fetch_add_explicit(&client->refs, 1U, memory_order_relaxed);
}

static void chat_client_release(chat_client_t *client) {
    chat_server_t *server;

    if (atomic_fetch_sub_explicit(&client->refs, 1U, memory_order_acq_rel) != 1U) {
        return;
    }
    server = client->server;
    chat_client_close_fd(client);
    chat_outbox_destroy(&client->outbox);
    (void)atomic_fetch_sub_explicit(&server->live_clients, 1U, memory_order_acq_rel);
    free(client);
}

static bool chat_client_begin_enqueue(chat_client_t *client) {
    if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) { return false; }
    (void)atomic_fetch_add_explicit(&client->enqueue_refs, 1U, memory_order_acq_rel);
    if (atomic_load_explicit(&client->closing, memory_order_acquire) == 0U) { return true; }
    (void)atomic_fetch_sub_explicit(&client->enqueue_refs, 1U, memory_order_acq_rel);
    return false;
}

static void chat_client_end_enqueue(chat_client_t *client) {
    (void)atomic_fetch_sub_explicit(&client->enqueue_refs, 1U, memory_order_acq_rel);
}

static void chat_client_wait_enqueues_drained(chat_client_t *client) {
    while (atomic_load_explicit(&client->enqueue_refs, memory_order_acquire) != 0U) {
        if (llam_current_task() != NULL) { llam_yield(); } else { usleep(1000U); }
    }
}

static chat_message_t *chat_message_create_prefixed(const char *prefix, size_t prefix_len, const char *data, size_t len) {
    chat_message_t *message;

    if (prefix_len > CHAT_PREFIX_CAP) {
        prefix_len = CHAT_PREFIX_CAP;
    }
    message = malloc(sizeof(*message) + len);
    if (message == NULL) {
        return NULL;
    }
    atomic_init(&message->refs, 1U);
    message->prefix_len = prefix_len;
    message->data_len = len;
    if (prefix_len > 0U) {
        memcpy(message->prefix, prefix, prefix_len);
    }
    if (len > 0U) {
        memcpy(message->data, data, len);
    }
    return message;
}

static chat_message_t *chat_message_create(const char *data, size_t len) {
    return chat_message_create_prefixed(NULL, 0U, data, len);
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

static void chat_message_retain(chat_message_t *message) {
    (void)atomic_fetch_add_explicit(&message->refs, 1U, memory_order_relaxed);
}

static void chat_message_release(chat_message_t *message) {
    if (message != NULL && atomic_fetch_sub_explicit(&message->refs, 1U, memory_order_acq_rel) == 1U) {
        free(message);
    }
}

static int chat_client_fd(const chat_client_t *client) {
    return atomic_load_explicit(&client->fd, memory_order_acquire);
}

static void chat_wake_listener(int listener_fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd;

    if (listener_fd < 0) {
        return;
    }
    memset(&addr, 0, sizeof(addr));
    if (getsockname(listener_fd, (struct sockaddr *)(void *)&addr, &addr_len) != 0 ||
        addr.sin_family != AF_INET) {
        return;
    }
    if (addr.sin_addr.s_addr == htonl(INADDR_ANY)) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    (void)connect(fd, (struct sockaddr *)(void *)&addr, sizeof(addr));
    close(fd);
}

static int chat_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void chat_client_close_fd(chat_client_t *client) {
    int fd = atomic_exchange_explicit(&client->fd, -1, memory_order_acq_rel);

    if (fd >= 0) { (void)shutdown(fd, SHUT_RDWR); (void)close(fd); }
}

static void chat_client_shutdown_fd(chat_client_t *client) {
    int fd = chat_client_fd(client);

    /*
     * Shutdown wakes any blocked read/write without releasing the descriptor
     * number yet.  The actual close is deferred to final client release so an
     * in-flight writer cannot accidentally write to a recycled fd.
     */
    if (fd >= 0) { (void)shutdown(fd, SHUT_RDWR); }
}

static int chat_write_message(chat_client_t *client, const chat_message_t *message) {
    size_t total_len;
    size_t off = 0U;

    if (message == NULL) {
        return 0;
    }
    total_len = message->prefix_len + message->data_len;
    while (off < total_len) {
        int fd = chat_client_fd(client);
        llam_iovec_t iov[2];
        struct iovec native_iov[2];
        int iovcnt = 0;
        ssize_t nwritten;

        if (fd < 0) {
            return -1;
        }
        if (off < message->prefix_len) {
            iov[iovcnt].iov_base = message->prefix + off;
            iov[iovcnt].iov_len = message->prefix_len - off;
            iovcnt += 1;
            if (message->data_len > 0U) {
                iov[iovcnt].iov_base = message->data;
                iov[iovcnt].iov_len = message->data_len;
                iovcnt += 1;
            }
        } else {
            size_t data_off = off - message->prefix_len;

            iov[iovcnt].iov_base = message->data + data_off;
            iov[iovcnt].iov_len = message->data_len - data_off;
            iovcnt += 1;
        }
        for (int i = 0; i < iovcnt; ++i) {
            native_iov[i].iov_base = (void *)iov[i].iov_base;
            native_iov[i].iov_len = iov[i].iov_len;
        }
        nwritten = writev(fd, native_iov, iovcnt);
        if (nwritten > 0) {
            off += (size_t)nwritten;
            continue;
        }
        if (nwritten < 0 && errno == EINTR) {
            continue;
        }
        if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            short revents = 0;
            int poll_rc;

            if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U ||
                atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
                return -1;
            }
            poll_rc = llam_poll_fd(fd, POLLOUT, 100, &revents);
            if (poll_rc == 0 || (poll_rc < 0 && errno == EINTR)) {
                continue;
            }
            if (poll_rc < 0 || (revents & (POLLOUT | POLLHUP | POLLERR)) == 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

static size_t chat_write_iov_limit(void) {
#ifdef IOV_MAX
    return IOV_MAX < CHAT_WRITE_IOV_CAP ? (size_t)IOV_MAX : CHAT_WRITE_IOV_CAP;
#else
    return CHAT_WRITE_IOV_CAP;
#endif
}

static int chat_message_append_iov(const chat_message_t *message,
                                   size_t offset,
                                   struct iovec *iov,
                                   size_t iov_limit,
                                   size_t *iov_count) {
    if (message == NULL || *iov_count >= iov_limit) {
        return 0;
    }
    if (offset < message->prefix_len) {
        iov[*iov_count].iov_base = (void *)(message->prefix + offset);
        iov[*iov_count].iov_len = message->prefix_len - offset;
        *iov_count += 1U;
        if (message->data_len > 0U && *iov_count < iov_limit) {
            iov[*iov_count].iov_base = (void *)message->data;
            iov[*iov_count].iov_len = message->data_len;
            *iov_count += 1U;
        }
        return 0;
    }
    if (offset < message->prefix_len + message->data_len) {
        size_t data_offset = offset - message->prefix_len;

        iov[*iov_count].iov_base = (void *)(message->data + data_offset);
        iov[*iov_count].iov_len = message->data_len - data_offset;
        *iov_count += 1U;
    }
    return 0;
}

static void chat_advance_written(chat_message_t **messages,
                                 size_t count,
                                 size_t *index,
                                 size_t *offset,
                                 size_t written) {
    while (*index < count && written > 0U) {
        chat_message_t *message = messages[*index];
        size_t total_len = message->prefix_len + message->data_len;
        size_t remaining = total_len > *offset ? total_len - *offset : 0U;

        if (written < remaining) {
            *offset += written;
            return;
        }
        written -= remaining;
        *index += 1U;
        *offset = 0U;
    }
}

static int chat_write_batch(chat_client_t *client, chat_message_t **messages, size_t count) {
    size_t index = 0U;
    size_t offset = 0U;
    size_t iov_limit = chat_write_iov_limit();

    if (count == 0U) {
        return 0;
    }
    if (count == 1U) {
        return chat_write_message(client, messages[0]);
    }
    while (index < count) {
        struct iovec iov[CHAT_WRITE_IOV_CAP];
        size_t iov_count = 0U;
        ssize_t nwritten;
        int fd = chat_client_fd(client);

        if (fd < 0) {
            return -1;
        }
        for (size_t i = index; i < count && iov_count < iov_limit; ++i) {
            chat_message_append_iov(messages[i], i == index ? offset : 0U, iov, iov_limit, &iov_count);
        }
        if (iov_count == 0U) {
            return 0;
        }
        /* Coalesce ordered prefix/data slices to avoid one syscall per tiny delivery. */
        nwritten = writev(fd, iov, (int)iov_count);
        if (nwritten > 0) {
            chat_advance_written(messages, count, &index, &offset, (size_t)nwritten);
            continue;
        }
        if (nwritten < 0 && errno == EINTR) {
            continue;
        }
        if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            short revents = 0;
            int poll_rc;

            if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U ||
                atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
                return -1;
            }
            poll_rc = llam_poll_fd(fd, POLLOUT, 100, &revents);
            if (poll_rc == 0 || (poll_rc < 0 && errno == EINTR)) {
                continue;
            }
            if (poll_rc < 0 || (revents & (POLLOUT | POLLHUP | POLLERR)) == 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

static int chat_outbox_init(chat_outbox_t *outbox,
                            atomic_uint_fast64_t *full_drop_counter,
                            atomic_uint_fast64_t *closed_drop_counter,
                            bool lossless) {
    memset(outbox, 0, sizeof(*outbox));
    outbox->full_drop_counter = full_drop_counter;
    outbox->closed_drop_counter = closed_drop_counter;
    outbox->lossless = lossless;
    atomic_init(&outbox->closed, false);
    outbox->lock = llam_mutex_create();
    if (outbox->lock == NULL) {
        return -1;
    }
    outbox->wake = llam_channel_create(1U);
    if (outbox->wake == NULL) {
        int saved_errno = errno;

        (void)llam_mutex_destroy(outbox->lock);
        outbox->lock = NULL;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void chat_outbox_release_queued_locked(chat_outbox_t *outbox) {
    while (outbox->count > 0U) {
        chat_message_t *message = outbox->ring[outbox->head];

        outbox->ring[outbox->head] = NULL;
        outbox->head = (outbox->head + 1U) % CHAT_OUTBOX_CAP;
        outbox->count -= 1U;
        chat_message_release(message);
    }
    outbox->head = 0U;
    outbox->tail = 0U;
}

static void chat_outbox_lock(chat_outbox_t *outbox) {
    for (;;) {
        if (llam_mutex_trylock(outbox->lock) == 0) { return; }
        // Ring updates are tiny; yield instead of parking on short contention.
        if (llam_current_task() != NULL) { llam_yield(); } else { usleep(1000U); }
    }
}

static void chat_outbox_unlock(chat_outbox_t *outbox) { (void)llam_mutex_unlock(outbox->lock); }

static void chat_outbox_close(chat_outbox_t *outbox) {
    if (outbox == NULL) { return; }
    // Close is published atomically; the writer drops queued best-effort backlog.
    atomic_store_explicit(&outbox->closed, true, memory_order_release);
    if (outbox->wake != NULL) {
        (void)llam_channel_close(outbox->wake);
    }
}

static void chat_outbox_destroy(chat_outbox_t *outbox) {
    if (outbox == NULL) { return; }
    chat_outbox_close(outbox);
    /* Preserve ring-buffer lock discipline while shutdown tasks unwind. */
    if (outbox->lock != NULL) {
        chat_outbox_lock(outbox);
        chat_outbox_release_queued_locked(outbox);
        chat_outbox_unlock(outbox);
    } else {
        chat_outbox_release_queued_locked(outbox);
    }
    if (outbox->wake != NULL) {
        void *ignored = NULL;

        /* Host-side final release may run after runtime stop; drain nonblocking. */
        while (llam_channel_try_recv_result(outbox->wake, &ignored) == 0) {
            ignored = NULL;
        }
        (void)llam_channel_destroy(outbox->wake);
        outbox->wake = NULL;
    }
    if (outbox->lock != NULL) {
        (void)llam_mutex_destroy(outbox->lock);
        outbox->lock = NULL;
    }
}

static bool chat_outbox_push(chat_client_t *client, chat_message_t *message) {
    chat_outbox_t *outbox = &client->outbox;

    if (message == NULL) { return false; }
    for (;;) {
        bool need_wake = false;

        chat_outbox_lock(outbox);
        if (atomic_load_explicit(&outbox->closed, memory_order_acquire)) {
            if (outbox->closed_drop_counter != NULL) {
                (void)atomic_fetch_add_explicit(outbox->closed_drop_counter, 1U, memory_order_relaxed);
            }
            chat_outbox_unlock(outbox);
            chat_message_release(message);
            return false;
        }
        if (outbox->count < CHAT_OUTBOX_CAP) {
            if (outbox->count == 0U && !outbox->wake_pending) {
                outbox->wake_pending = true;
                need_wake = true;
            }
            outbox->ring[outbox->tail] = message;
            outbox->tail = (outbox->tail + 1U) % CHAT_OUTBOX_CAP;
            outbox->count += 1U;
            chat_outbox_unlock(outbox);

            if (need_wake && llam_channel_try_send(outbox->wake, outbox) != 0 && errno == EPIPE) {
                chat_outbox_close(outbox);
            }
            return true;
        }
        if (!outbox->lossless) {
            /*
             * Best-effort mode protects the server from slow receivers.  The
             * flood harness accounts these drops explicitly, so high-rate
             * benchmark loss is explainable rather than silent data loss.
             */
            if (outbox->full_drop_counter != NULL) {
                (void)atomic_fetch_add_explicit(outbox->full_drop_counter, 1U, memory_order_relaxed);
            }
            chat_outbox_unlock(outbox);
            chat_message_release(message);
            return false;
        }
        chat_outbox_unlock(outbox);

        if (atomic_load_explicit(&g_stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
            chat_message_release(message);
            return false;
        }
        // Lossless mode turns outbox capacity pressure into cooperative producer backpressure.
        llam_yield();
    }
}

static size_t chat_outbox_pop_batch(chat_outbox_t *outbox, chat_message_t **messages, size_t cap, bool *closed_out) {
    size_t count = 0U;

    chat_outbox_lock(outbox);
    outbox->wake_pending = false;
    while (count < cap && outbox->count > 0U) {
        messages[count] = outbox->ring[outbox->head];
        outbox->ring[outbox->head] = NULL;
        outbox->head = (outbox->head + 1U) % CHAT_OUTBOX_CAP;
        outbox->count -= 1U;
        count += 1U;
    }
    if (closed_out != NULL) {
        *closed_out = atomic_load_explicit(&outbox->closed, memory_order_acquire) && outbox->count == 0U;
    }
    chat_outbox_unlock(outbox);
    return count;
}

static size_t chat_outbox_wait_pop_batch(chat_outbox_t *outbox, chat_message_t **messages, size_t cap, bool *closed_out) {
    void *wake = NULL;

    if (llam_channel_recv_result(outbox->wake, &wake) != 0) {
        chat_outbox_lock(outbox);
        // Do not leave backlog for a writer that is already exiting.
        chat_outbox_release_queued_locked(outbox);
        if (closed_out != NULL) { *closed_out = true; }
        chat_outbox_unlock(outbox);
        return 0U;
    }
    return chat_outbox_pop_batch(outbox, messages, cap, closed_out);
}

static int chat_server_add_client(chat_server_t *server, chat_client_t *client) {
    if (pthread_rwlock_wrlock(&server->clients_lock) != 0) {
        return -1;
    }
    chat_client_retain(client);
    client->next = server->clients;
    server->clients = client;
    (void)pthread_rwlock_unlock(&server->clients_lock);
    return 0;
}

static void chat_server_remove_client(chat_server_t *server, chat_client_t *client) {
    chat_client_t **cursor;
    bool removed = false;

    if (pthread_rwlock_wrlock(&server->clients_lock) != 0) {
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
    (void)pthread_rwlock_unlock(&server->clients_lock);
    if (removed) {
        chat_client_release(client);
    }
}

static void chat_client_begin_close(chat_client_t *client) {
    if (atomic_exchange_explicit(&client->closing, 1U, memory_order_acq_rel) != 0U) {
        return;
    }
    chat_server_remove_client(client->server, client);
    chat_client_wait_enqueues_drained(client);
    chat_outbox_close(&client->outbox);
    // Defer close to final release so in-flight writers cannot hit a reused fd.
    chat_client_shutdown_fd(client);
}

static void chat_server_close_all(chat_server_t *server) {
    for (;;) {
        chat_client_t *client;

        if (pthread_rwlock_wrlock(&server->clients_lock) != 0) {
            return;
        }
        client = server->clients;
        if (client != NULL) {
            server->clients = client->next;
            client->next = NULL;
        }
        (void)pthread_rwlock_unlock(&server->clients_lock);

        if (client == NULL) {
            return;
        }
        if (atomic_exchange_explicit(&client->closing, 1U, memory_order_acq_rel) == 0U) {
            chat_client_wait_enqueues_drained(client);
            chat_outbox_close(&client->outbox);
            // Global stop has closed the listener, so immediate close cannot hit a new client fd.
            if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) { chat_client_close_fd(client); }
            else { chat_client_shutdown_fd(client); }
        }
        // Drop the list reference removed above.
        chat_client_release(client);
    }
}

static bool chat_wait_clients_drained(const chat_server_t *server, unsigned timeout_ms) {
    unsigned waited_ms = 0U;

    while (atomic_load_explicit(&server->live_clients, memory_order_acquire) != 0U) {
        if (waited_ms >= timeout_ms) {
            return false;
        }
        usleep(10000U);
        waited_ms += 10U;
    }
    return true;
}

static void chat_request_stop(chat_server_t *server) {
    int fd;

    if (atomic_exchange_explicit(&g_stop_requested, true, memory_order_acq_rel)) {
        return;
    }
    if (!server->quiet) {
        fprintf(stdout, "shutdown requested\n");
        fflush(stdout);
    }
    if (server->stop_token != NULL) {
        (void)llam_cancel_token_cancel(server->stop_token);
    }
    fd = atomic_load_explicit(&server->listener_fd, memory_order_acquire);
    chat_wake_listener(fd);
    fd = atomic_exchange_explicit(&server->listener_fd, -1, memory_order_acq_rel);
    if (fd >= 0) {
        (void)close(fd);
    }
    /*
     * Keep the signal helper non-blocking: it only publishes stop state and
     * wakes the runtime.  The accept task/main thread own client drain and
     * stats emission so pthread_join(signal_thread) cannot wait behind outbox
     * or client reference cleanup.
     */
    (void)llam_runtime_request_stop();
}

static void chat_print_stats(FILE *stream, const chat_server_t *server) {
    fprintf(stream,
            "server stopped; outbox_full_drops=%" PRIuFAST64
            " outbox_closed_drops=%" PRIuFAST64
            " broadcast_messages_created=%" PRIuFAST64
            " broadcast_deliveries_attempted=%" PRIuFAST64
            " broadcast_deliveries_enqueued=%" PRIuFAST64 "\n",
            atomic_load_explicit(&server->outbox_full_drops, memory_order_relaxed),
            atomic_load_explicit(&server->outbox_closed_drops, memory_order_relaxed),
            atomic_load_explicit(&server->broadcast_messages_created, memory_order_relaxed),
            atomic_load_explicit(&server->broadcast_deliveries_attempted, memory_order_relaxed),
            atomic_load_explicit(&server->broadcast_deliveries_enqueued, memory_order_relaxed));
}

static void chat_write_stats_file(const chat_server_t *server) {
    const char *path = getenv("LLAM_CHAT_STATS_PATH");
    int fd;
    FILE *file;

    if (path == NULL || path[0] == '\0') { return; }
    fd = chat_open_append_regular(path);
    if (fd < 0) { return; }
    file = fdopen(fd, "a");
    if (file == NULL) { close(fd); return; }
    chat_print_stats(file, server);
    fflush(file);
    (void)fsync(fileno(file));
    fclose(file);
}

static bool chat_queue_message(chat_client_t *client, chat_message_t *message) {
    bool queued;

    if (message == NULL) {
        return false;
    }
    if (!chat_client_begin_enqueue(client)) {
        chat_message_release(message);
        return false;
    }
    queued = chat_outbox_push(client, message);
    chat_client_end_enqueue(client);
    return queued;
}

static void chat_queue_text(chat_client_t *client, const char *text) {
    (void)chat_queue_message(client, chat_message_create(text, strlen(text)));
}

static void chat_broadcast_message(chat_server_t *server, const chat_client_t *sender, chat_message_t *message) {
    chat_client_t *targets[CHAT_MAX_BROADCAST_TARGETS];
    unsigned count = 0U;
    unsigned enqueued = 0U;
    unsigned i;
    int lock_rc;

    if (message == NULL) {
        return;
    }
    /*
     * Shutdown needs the write side of clients_lock.  Use tryrdlock + yield so
     * a flood of broadcasts cannot starve stop/cleanup behind fresh readers.
     */
    for (;;) {
        if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
            return;
        }
        lock_rc = pthread_rwlock_tryrdlock(&server->clients_lock);
        if (lock_rc == 0) {
            break;
        }
        if (lock_rc != EBUSY) {
            return;
        }
        llam_yield();
    }
    if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        (void)pthread_rwlock_unlock(&server->clients_lock);
        return;
    }
    for (chat_client_t *client = server->clients;
         client != NULL && count < CHAT_MAX_BROADCAST_TARGETS;
         client = client->next) {
        if (client == sender || atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
            continue;
        }
        chat_client_retain(client);
        if (!chat_client_begin_enqueue(client)) {
            chat_client_release(client);
            continue;
        }
        targets[count++] = client;
    }
    (void)pthread_rwlock_unlock(&server->clients_lock);

    (void)atomic_fetch_add_explicit(&server->broadcast_messages_created, 1U, memory_order_relaxed);
    if (count > 0U) {
        (void)atomic_fetch_add_explicit(&server->broadcast_deliveries_attempted, count, memory_order_relaxed);
    }
    for (i = 0U; i < count; ++i) {
        chat_message_retain(message);
        if (chat_outbox_push(targets[i], message)) {
            enqueued += 1U;
        }
        chat_client_end_enqueue(targets[i]);
        chat_client_release(targets[i]);
    }
    if (enqueued > 0U) {
        (void)atomic_fetch_add_explicit(&server->broadcast_deliveries_enqueued, enqueued, memory_order_relaxed);
    }
}

static void chat_broadcast_system(chat_server_t *server, const chat_client_t *client, const char *event) {
    chat_message_t *message = chat_message_printf("* client %u %s\n", client->id, event);

    if (message == NULL) {
        return;
    }
    chat_broadcast_message(server, client, message);
    if (!server->quiet) {
        fprintf(stdout, "%.*s", (int)message->data_len, message->data);
        fflush(stdout);
    }
    chat_message_release(message);
}

static void chat_client_broadcast_line(chat_client_t *client, const char *data, size_t len) {
    size_t copy_len;
    chat_message_t *message;

    if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        return;
    }
    copy_len = len;
    if (copy_len > CHAT_INPUT_CAP) {
        copy_len = CHAT_INPUT_CAP;
    }
    message = chat_message_create_prefixed(client->prefix, client->prefix_len, data, copy_len);
    if (message == NULL) {
        return;
    }
    chat_broadcast_message(client->server, client, message);
    chat_message_release(message);
}

static size_t chat_complete_line_span(const char *data, size_t len) {
    const void *newline = memchr(data, '\n', len);

    if (newline != NULL) {
        return (size_t)((const char *)newline - data) + 1U;
    }
    return 0U;
}

static void chat_client_process_input(chat_client_t *client, const char *data, size_t len) {
    size_t off = 0U;

    while (off < len && !atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        size_t span;
        size_t space;
        size_t copy_len;

        if (client->input_len == 0U) {
            span = chat_complete_line_span(data + off, len - off);
            if (span > 0U) {
                chat_client_broadcast_line(client, data + off, span);
                off += span;
                continue;
            }
        }

        space = sizeof(client->input) - client->input_len;
        copy_len = len - off;
        if (copy_len > space) {
            copy_len = space;
        }
        memcpy(client->input + client->input_len, data + off, copy_len);
        client->input_len += copy_len;
        off += copy_len;

        for (;;) {
            span = chat_complete_line_span(client->input, client->input_len);
            if (span == 0U) {
                break;
            }
            size_t tail_len = client->input_len - span;

            chat_client_broadcast_line(client, client->input, span);
            if (tail_len > 0U) {
                memmove(client->input, client->input + span, tail_len);
            }
            client->input_len = tail_len;
        }
        if (client->input_len == sizeof(client->input)) {
            chat_client_broadcast_line(client, client->input, client->input_len);
            client->input_len = 0U;
        }
    }
}

static void chat_writer_release_batch(chat_message_t **messages, size_t count) {
    for (size_t i = 0U; i < count; ++i) {
        chat_message_release(messages[i]);
    }
}

static void chat_writer_drop_backlog(chat_client_t *client) {
    for (;;) {
        chat_message_t *batch[CHAT_WRITE_BATCH];
        size_t count = chat_outbox_pop_batch(&client->outbox, batch, CHAT_WRITE_BATCH, NULL);

        if (count == 0U) {
            return;
        }
        chat_writer_release_batch(batch, count);
    }
}

static void chat_writer_task(void *arg) {
    chat_client_t *client = arg;
    chat_message_t *batch[CHAT_WRITE_BATCH];

    for (;;) {
        bool closed = false;
        size_t count;

        if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U) {
            chat_writer_drop_backlog(client);
            break;
        }
        count = chat_outbox_wait_pop_batch(&client->outbox, batch, CHAT_WRITE_BATCH, &closed);
        for (;;) {
            if (count == 0U) {
                if (closed) {
                    goto done;
                }
                break;
            }
            if (atomic_load_explicit(&client->closing, memory_order_acquire) != 0U ||
                chat_write_batch(client, batch, count) != 0) {
                chat_client_begin_close(client);
                chat_writer_release_batch(batch, count);
                chat_writer_drop_backlog(client);
                goto done;
            }
            chat_writer_release_batch(batch, count);
            count = chat_outbox_pop_batch(&client->outbox, batch, CHAT_WRITE_BATCH, &closed);
        }
    }

done:
    chat_client_release(client);
}

static void chat_reader_task(void *arg) {
    chat_client_t *client = arg;
    char buf[CHAT_READ_BUF];

    chat_broadcast_system(client->server, client, "joined");
    for (;;) {
        int fd = chat_client_fd(client);
        ssize_t nread;

        if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
            break;
        }
        if (fd < 0) {
            break;
        }
        nread = llam_read_when_ready(fd, buf, sizeof(buf), 100);

        if (nread > 0) {
            chat_client_process_input(client, buf, (size_t)nread);
            continue;
        }
        if (nread < 0 && (errno == EINTR || errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        break;
    }

    bool stopping = atomic_load_explicit(&g_stop_requested, memory_order_acquire);

    if (!stopping && client->input_len > 0U) {
        chat_client_broadcast_line(client, client->input, client->input_len);
        client->input_len = 0U;
    }
    if (!stopping) {
        chat_broadcast_system(client->server, client, "left");
    }
    chat_client_begin_close(client);
    chat_client_release(client);
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
    if (chat_outbox_init(&client->outbox,
                         &server->outbox_full_drops,
                         &server->outbox_closed_drops,
                         server->lossless_outbox) != 0) {
        free(client);
        return NULL;
    }
    (void)atomic_fetch_add_explicit(&server->live_clients, 1U, memory_order_relaxed);
    // The accept loop owns the initial local reference.
    atomic_init(&client->refs, 1U);
    atomic_init(&client->closing, 0U);
    atomic_init(&client->enqueue_refs, 0U);
    chat_peer_name(peer_addr, peer_len, client->peer, sizeof(client->peer));
    {
        int prefix_len = snprintf(client->prefix, sizeof(client->prefix), "[client %u] ", client->id);

        if (prefix_len < 0) {
            client->prefix_len = 0U;
        } else if ((size_t)prefix_len >= sizeof(client->prefix)) {
            client->prefix_len = sizeof(client->prefix) - 1U;
        } else {
            client->prefix_len = (size_t)prefix_len;
        }
    }
    return client;
}

static void chat_accept_task(void *arg) {
    chat_server_t *server = arg;

    while (!atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        int listener_fd = atomic_load_explicit(&server->listener_fd, memory_order_acquire);
        bool accepted_any = false;

        if (listener_fd < 0) {
            break;
        }

        for (;;) {
            struct sockaddr_storage peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            int fd;
            chat_client_t *client;
            llam_task_t *reader;
            llam_task_t *writer;
            llam_spawn_opts_t reader_opts;
            llam_spawn_opts_t writer_opts;

            if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
                break;
            }
            memset(&peer_addr, 0, sizeof(peer_addr));
            fd = accept(listener_fd, (struct sockaddr *)(void *)&peer_addr, &peer_len);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (atomic_load_explicit(&g_stop_requested, memory_order_acquire) ||
                    errno == EBADF ||
                    errno == ECANCELED) {
                    break;
                }
                perror("accept");
                break;
            }
            accepted_any = true;
            if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
                (void)close(fd);
                break;
            }
            if (chat_set_nonblocking(fd) != 0) {
                perror("chat_set_nonblocking client");
                (void)close(fd);
                continue;
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
            if (llam_spawn_opts_init(&writer_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0 ||
                llam_spawn_opts_init(&reader_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
                perror("llam_spawn_opts_init client");
                chat_client_begin_close(client);
                chat_client_release(client);
                continue;
            }
            writer_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_HUGE;
            reader_opts.cancel_token = server->stop_token;
            reader_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_HUGE;

            chat_client_retain(client);
            writer = llam_spawn_ex(chat_writer_task, client, &writer_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
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
                (void)llam_join(writer);
                chat_client_release(client);
                continue;
            }

            chat_client_retain(client);
            reader = llam_spawn_ex(chat_reader_task, client, &reader_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
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
                (void)llam_join(reader);
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
        if (!accepted_any) {
            (void)llam_sleep_ns(1000ULL * 1000ULL);
        }
    }

    chat_server_close_all(server);
    (void)llam_runtime_request_stop();
}

static void *chat_signal_thread_main(void *arg) {
    chat_server_t *server = arg;
    int signo = 0;

    for (;;) {
        if (sigwait(&server->stop_signals, &signo) != 0) {
            continue;
        }
        if (signo == CHAT_SIGNAL_THREAD_WAKE &&
            atomic_load_explicit(&server->signal_thread_stop, memory_order_acquire)) {
            break;
        }
        if (signo == SIGINT || signo == SIGTERM) {
            chat_request_stop(server);
            break;
        }
    }
    atomic_store_explicit(&server->signal_thread_done, true, memory_order_release);
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
    if (chat_set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int chat_start_signal_thread(chat_server_t *server) {
    int err;

    atomic_store_explicit(&server->signal_thread_stop, false, memory_order_release);
    atomic_store_explicit(&server->signal_thread_done, false, memory_order_release);
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
    atomic_store_explicit(&server->signal_thread_stop, true, memory_order_release);
    /* If SIGINT/SIGTERM already returned the helper, just join it. */
    if (!atomic_load_explicit(&server->signal_thread_done, memory_order_acquire)) {
        (void)pthread_kill(server->signal_thread, CHAT_SIGNAL_THREAD_WAKE);
    }
    (void)pthread_join(server->signal_thread, NULL);
    server->signal_thread_started = false;
    atomic_store_explicit(&server->signal_thread_stop, false, memory_order_release);
    atomic_store_explicit(&server->signal_thread_done, false, memory_order_release);
}

static void chat_dump_runtime_if_requested(const char *phase) {
    const char *path = getenv("LLAM_CHAT_DUMP_ON_STOP");
    int fd;
    FILE *file;

    if (path == NULL || path[0] == '\0') { return; }
    fd = chat_open_append_regular(path);
    if (fd < 0) { return; }
    file = fdopen(fd, "a");
    if (file == NULL) { close(fd); return; }
    fprintf(file, "chat dump phase=%s\n", phase != NULL ? phase : "unknown");
    fflush(file);
    llam_dump_runtime_state(fileno(file));
    fclose(file);
}

static void chat_destroy_clients_lock(chat_server_t *server) {
    if (!server->clients_lock_initialized) { return; }
    (void)pthread_rwlock_destroy(&server->clients_lock);
    server->clients_lock_initialized = false;
}

static void chat_cleanup_runtime_before_run(chat_server_t *server) {
    int listener_fd = atomic_exchange_explicit(&server->listener_fd, -1, memory_order_acq_rel);
    if (listener_fd >= 0) { close(listener_fd); }
    chat_stop_signal_thread(server);
    if (server->stop_token != NULL) {
        (void)llam_cancel_token_destroy(server->stop_token);
        server->stop_token = NULL;
    }
    chat_destroy_clients_lock(server);
    llam_runtime_shutdown();
}

int main(int argc, char **argv) {
    chat_server_t server;
    llam_runtime_opts_t opts;
    llam_task_t *accept_task;
    llam_spawn_opts_t accept_opts;
    const char *quiet_env;
    uint16_t port = CHAT_DEFAULT_PORT;
    bool public_bind = false;
    bool lossless_outbox = LLAM_CHAT_LOSSLESS_DEFAULT != 0;
    int rc = 0;
    int err;

    (void)signal(SIGPIPE, SIG_IGN);
    lossless_outbox = chat_env_enabled(getenv("LLAM_CHAT_LOSSLESS"), lossless_outbox);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--public") == 0) {
            public_bind = true;
        } else if (strcmp(argv[i], "--lossless") == 0) {
            lossless_outbox = true;
        } else if (strcmp(argv[i], "--best-effort") == 0) {
            lossless_outbox = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            chat_print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            chat_print_usage(argv[0]);
            return 2;
        } else {
            port = chat_parse_port(argv[i], CHAT_DEFAULT_PORT);
            if (port == 0U) { return 2; }
        }
    }

    memset(&server, 0, sizeof(server));
    atomic_init(&server.listener_fd, -1);
    atomic_init(&server.next_client_id, 0U);
    atomic_init(&server.outbox_full_drops, 0U);
    atomic_init(&server.outbox_closed_drops, 0U);
    atomic_init(&server.broadcast_messages_created, 0U);
    atomic_init(&server.broadcast_deliveries_attempted, 0U);
    atomic_init(&server.broadcast_deliveries_enqueued, 0U);
    atomic_init(&server.live_clients, 0U);
    atomic_init(&server.signal_thread_stop, false);
    atomic_init(&server.signal_thread_done, false);
    quiet_env = getenv("LLAM_CHAT_QUIET");
    server.quiet = chat_env_enabled(quiet_env, false);
    server.lossless_outbox = lossless_outbox;
    atomic_store_explicit(&g_stop_requested, false, memory_order_release);

    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    sigemptyset(&server.stop_signals);
    sigaddset(&server.stop_signals, SIGINT);
    sigaddset(&server.stop_signals, SIGTERM);
    sigaddset(&server.stop_signals, CHAT_SIGNAL_THREAD_WAKE);
    err = pthread_sigmask(SIG_BLOCK, &server.stop_signals, NULL);
    if (err != 0) {
        errno = err;
        perror("pthread_sigmask");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    memset(&opts, 0, sizeof(opts));
    opts.profile = LLAM_RUNTIME_PROFILE_IO_LATENCY;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        perror("llam_runtime_init");
        return 1;
    }

    if (pthread_rwlock_init(&server.clients_lock, NULL) != 0) {
        perror("pthread_rwlock_init");
        chat_cleanup_runtime_before_run(&server);
        return 1;
    }
    server.clients_lock_initialized = true;
    server.stop_token = llam_cancel_token_create();
    if (server.stop_token == NULL) {
        perror("llam_cancel_token_create");
        chat_cleanup_runtime_before_run(&server);
        return 1;
    }

    atomic_store_explicit(&server.listener_fd, chat_create_listener(port, public_bind), memory_order_release);
    if (atomic_load_explicit(&server.listener_fd, memory_order_acquire) < 0) {
        perror("chat_create_listener");
        chat_cleanup_runtime_before_run(&server);
        return 1;
    }

    if (!server.quiet) {
        fprintf(stdout,
                "LLAM chat server listening on %s:%u mode=%s\n",
                public_bind ? "0.0.0.0" : "127.0.0.1",
                (unsigned)port,
                server.lossless_outbox ? "lossless" : "best-effort");
        fflush(stdout);
    }

    if (chat_start_signal_thread(&server) != 0) {
        perror("chat_start_signal_thread");
        chat_cleanup_runtime_before_run(&server);
        return 1;
    }

    if (llam_spawn_opts_init(&accept_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_spawn_opts_init");
        chat_cleanup_runtime_before_run(&server);
        return 1;
    }
    accept_opts.cancel_token = server.stop_token;
    accept_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_HUGE;
    accept_task = llam_spawn_ex(chat_accept_task, &server, &accept_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (accept_task == NULL || llam_detach(accept_task) != 0) {
        perror("llam_spawn accept");
        chat_cleanup_runtime_before_run(&server);
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
    chat_server_close_all(&server);
    (void)chat_wait_clients_drained(&server, CHAT_SHUTDOWN_DRAIN_MS);
    chat_dump_runtime_if_requested("main-after-drain");
    chat_write_stats_file(&server);
    if (!server.quiet) {
        chat_print_stats(stdout, &server);
        fflush(stdout);
    }
    chat_dump_runtime_if_requested("main-before-signal-join");
    chat_stop_signal_thread(&server);
    (void)llam_cancel_token_destroy(server.stop_token);
    chat_destroy_clients_lock(&server);
    chat_dump_runtime_if_requested("main-before-runtime-shutdown");
    llam_runtime_shutdown();
    return rc;
}

#endif
