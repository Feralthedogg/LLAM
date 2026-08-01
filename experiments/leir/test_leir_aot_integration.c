// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_linux.h"

#include "llam/runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR AOT io_uring integration requires Linux");
    return 0;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    AOT_INTEGRATION_PAYLOAD_SIZE = 64,
    AOT_INTEGRATION_TIMEOUT_MS = 5000,
};

typedef struct aot_peer {
    int listener;
    unsigned char expected[AOT_INTEGRATION_PAYLOAD_SIZE];
    unsigned char observed[AOT_INTEGRATION_PAYLOAD_SIZE];
    size_t observed_size;
    int error_code;
} aot_peer_t;

typedef struct aot_task_state {
    leir_aot_linux_ticket_t *ticket;
    leir_phase0_value_t values[7];
    leir_phase0_value_t values_out[7];
    leir_aot_resume_result_v1_t resume;
    leir_aot_linux_metrics_t metrics;
    int bind_result;
    int bind_errno;
    int run_result;
    int run_errno;
} aot_task_state_t;

typedef struct aot_fixture {
    void *ticket_storage;
    void *module_storage;
    aot_task_state_t task;
    struct sockaddr_in address;
    unsigned char payload[AOT_INTEGRATION_PAYLOAD_SIZE];
    int client;
} aot_fixture_t;

static int normalized_alignment(size_t requested) {
    size_t alignment = requested;

    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if (alignment > (size_t)INT_MAX ||
        (alignment & (alignment - 1U)) != 0U) {
        errno = EINVAL;
        return -1;
    }
    return (int)alignment;
}

static void *allocate_aligned(
    size_t size,
    size_t requested_alignment) {
    void *allocation = NULL;
    int alignment = normalized_alignment(requested_alignment);
    int rc;

    if (alignment < 0) {
        return NULL;
    }
    rc = posix_memalign(&allocation, (size_t)alignment, size);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    memset(allocation, 0, size);
    return allocation;
}

static int create_listener(
    int *listener_out,
    struct sockaddr_in *address_out) {
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    int listener;
    int reuse = 1;

    listener = socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) {
        return -1;
    }
    (void)setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        (socklen_t)sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0U;
    if (bind(
            listener,
            (const struct sockaddr *)&address,
            (socklen_t)sizeof(address)) != 0 ||
        listen(listener, 16) != 0 ||
        getsockname(
            listener,
            (struct sockaddr *)&address,
            &address_length) != 0) {
        int saved_errno = errno;

        (void)close(listener);
        errno = saved_errno;
        return -1;
    }
    *listener_out = listener;
    *address_out = address;
    return 0;
}

static int reserve_refused_address(
    struct sockaddr_in *address_out) {
    int listener = -1;

    if (create_listener(&listener, address_out) != 0) {
        return -1;
    }
    return close(listener);
}

static int create_client(void) {
    return socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

static void *peer_main(void *argument) {
    aot_peer_t *peer = argument;
    struct pollfd listener_poll = {
        .fd = peer->listener,
        .events = POLLIN,
    };
    int accepted;

    if (poll(
            &listener_poll,
            1U,
            AOT_INTEGRATION_TIMEOUT_MS) <= 0) {
        peer->error_code = errno != 0 ? errno : ETIMEDOUT;
        return NULL;
    }
    accepted = accept4(
        peer->listener,
        NULL,
        NULL,
        SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (accepted < 0) {
        peer->error_code = errno;
        return NULL;
    }
    while (peer->observed_size < sizeof(peer->observed)) {
        struct pollfd read_poll = {
            .fd = accepted,
            .events = POLLIN,
        };
        ssize_t received;

        if (poll(
                &read_poll,
                1U,
                AOT_INTEGRATION_TIMEOUT_MS) <= 0) {
            peer->error_code = errno != 0 ? errno : ETIMEDOUT;
            break;
        }
        received = recv(
            accepted,
            peer->observed + peer->observed_size,
            sizeof(peer->observed) - peer->observed_size,
            0);
        if (received <= 0) {
            peer->error_code = received == 0 ? ECONNRESET : errno;
            break;
        }
        peer->observed_size += (size_t)received;
    }
    (void)close(accepted);
    return NULL;
}

static int fixture_init(
    aot_fixture_t *fixture,
    const struct sockaddr_in *address,
    uint64_t seed) {
    size_t ticket_size = leir_aot_linux_ticket_size();
    size_t module_size =
        leir_aot_connect_write_module_v1.instance_size;
    size_t i;

    memset(fixture, 0, sizeof(*fixture));
    fixture->client = -1;
    fixture->ticket_storage = allocate_aligned(
        ticket_size, leir_aot_linux_ticket_alignment());
    fixture->module_storage = allocate_aligned(
        module_size,
        leir_aot_connect_write_module_v1.instance_alignment);
    fixture->client = create_client();
    if (fixture->ticket_storage == NULL ||
        fixture->module_storage == NULL ||
        fixture->client < 0) {
        return -1;
    }
    fixture->address = *address;
    for (i = 0U; i < sizeof(fixture->payload); i += 1U) {
        fixture->payload[i] = (unsigned char)(seed + i * 17U);
    }
    if (leir_aot_linux_ticket_init(
            fixture->ticket_storage,
            ticket_size,
            &leir_aot_connect_write_module_v1,
            fixture->module_storage,
            module_size) != 0) {
        return -1;
    }
    fixture->task.ticket = fixture->ticket_storage;
    fixture->task.values[0].fd = fixture->client;
    fixture->task.values[1].buffer.data = &fixture->address;
    fixture->task.values[1].buffer.size = sizeof(fixture->address);
    fixture->task.values[2].u64 = sizeof(fixture->address);
    fixture->task.values[3].i64 = -1;
    fixture->task.values[4].buffer.data = fixture->payload;
    fixture->task.values[4].buffer.size = sizeof(fixture->payload);
    fixture->task.values[5].u64 = sizeof(fixture->payload);
    fixture->task.values[6].i64 = -1;
    return 0;
}

static void fixture_destroy(aot_fixture_t *fixture) {
    if (fixture->task.ticket != NULL) {
        (void)leir_aot_linux_ticket_destroy(fixture->task.ticket);
        fixture->task.ticket = NULL;
    }
    if (fixture->client >= 0) {
        (void)close(fixture->client);
        fixture->client = -1;
    }
    free(fixture->module_storage);
    free(fixture->ticket_storage);
    fixture->module_storage = NULL;
    fixture->ticket_storage = NULL;
}

static void aot_task_main(void *argument) {
    aot_task_state_t *state = argument;

    errno = 0;
    state->bind_result = leir_aot_linux_ticket_bind(
        state->ticket, state->values, 7U);
    state->bind_errno = errno;
    if (state->bind_result != 0) {
        return;
    }
    errno = 0;
    state->run_result = leir_aot_linux_ticket_run(
        state->ticket,
        state->values_out,
        7U,
        &state->resume,
        &state->metrics);
    state->run_errno = errno;
}

static int native_unavailable(int error_code) {
    return error_code == ENOTSUP || error_code == EAGAIN ||
           error_code == ENOSYS || error_code == EPERM ||
           error_code == EACCES;
}

int main(void) {
    aot_fixture_t success;
    aot_fixture_t refusal;
    aot_peer_t peer;
    struct sockaddr_in success_address;
    struct sockaddr_in refusal_address;
    llam_runtime_opts_t options;
    llam_task_t *success_task = NULL;
    llam_task_t *refusal_task = NULL;
    pthread_t peer_thread;
    int listener = -1;
    int peer_started = 0;
    int runtime_started = 0;
    int result = 1;

    memset(&success, 0, sizeof(success));
    memset(&refusal, 0, sizeof(refusal));
    success.client = -1;
    refusal.client = -1;
    memset(&peer, 0, sizeof(peer));
    if (create_listener(&listener, &success_address) != 0 ||
        reserve_refused_address(&refusal_address) != 0 ||
        fixture_init(
            &success, &success_address, UINT64_C(0x31)) != 0 ||
        fixture_init(
            &refusal, &refusal_address, UINT64_C(0x91)) != 0) {
        perror("AOT integration fixture init");
        goto cleanup;
    }
    peer.listener = listener;
    memcpy(peer.expected, success.payload, sizeof(peer.expected));
    if (pthread_create(&peer_thread, NULL, peer_main, &peer) != 0) {
        perror("AOT integration peer create");
        goto cleanup;
    }
    peer_started = 1;

    memset(&options, 0, sizeof(options));
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        if (native_unavailable(errno)) {
            fprintf(
                stderr,
                "SKIP: io_uring runtime unavailable: %s\n",
                strerror(errno));
            result = 77;
        } else {
            perror("AOT integration runtime init");
        }
        goto cleanup;
    }
    runtime_started = 1;
    success_task = llam_spawn(
        aot_task_main, &success.task, NULL);
    refusal_task = llam_spawn(
        aot_task_main, &refusal.task, NULL);
    if (success_task == NULL || refusal_task == NULL ||
        llam_run() != 0) {
        perror("AOT integration runtime run");
        goto cleanup;
    }
    if (llam_join(success_task) != 0 ||
        llam_join(refusal_task) != 0) {
        perror("AOT integration task join");
        goto cleanup;
    }
    success_task = NULL;
    refusal_task = NULL;

    if ((success.task.run_result != 0 &&
         native_unavailable(success.task.run_errno)) ||
        (refusal.task.run_result != 0 &&
         native_unavailable(refusal.task.run_errno))) {
        int unavailable_error = success.task.run_result != 0
            ? success.task.run_errno
            : refusal.task.run_errno;

        fprintf(
            stderr,
            "SKIP: native AOT segment unavailable: %s\n",
            strerror(unavailable_error));
        result = 77;
        goto cleanup;
    }
    if (success.task.bind_result != 0 ||
        success.task.run_result != 0 ||
        success.task.resume.action != LEIR_AOT_RESUME_RETURN ||
        success.task.resume.error_code != 0 ||
        success.task.values_out[3].i64 != 0 ||
        success.task.values_out[6].i64 !=
            AOT_INTEGRATION_PAYLOAD_SIZE ||
        success.task.metrics.activations != 1U ||
        success.task.metrics.logical_operations != 2U ||
        success.task.metrics.prepared_sqes != 2U ||
        success.task.metrics.observed_cqes != 1U ||
        success.task.metrics.suppressed_success_cqes != 1U ||
        success.task.metrics.task_parks != 1U ||
        success.task.metrics.terminal_wakes != 1U ||
        success.task.metrics.hot_allocations != 0U ||
        success.task.metrics.resumed_continuation !=
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) {
        fprintf(stderr, "successful AOT result or counters mismatch\n");
        goto cleanup;
    }
    if (refusal.task.bind_result != 0 ||
        refusal.task.run_result != 0 ||
        refusal.task.resume.action != LEIR_AOT_RESUME_FAIL ||
        refusal.task.resume.error_code != ECONNREFUSED ||
        refusal.task.values_out[3].i64 != -1 ||
        refusal.task.values_out[6].i64 != -1 ||
        refusal.task.metrics.first_error_operation != 0U ||
        refusal.task.metrics.resumed_continuation !=
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR ||
        refusal.task.metrics.terminal_wakes != 1U) {
        fprintf(stderr, "refused AOT result or ownership mismatch\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started) {
        (void)llam_runtime_request_stop();
        llam_runtime_shutdown();
    }
    if (listener >= 0) {
        (void)shutdown(listener, SHUT_RDWR);
    }
    if (peer_started) {
        (void)pthread_join(peer_thread, NULL);
    }
    if (listener >= 0) {
        (void)close(listener);
    }
    if (result == 0 &&
        (peer.error_code != 0 ||
         peer.observed_size != sizeof(peer.expected) ||
         memcmp(
             peer.observed,
             peer.expected,
             sizeof(peer.expected)) != 0)) {
        fprintf(stderr, "AOT integration peer payload mismatch\n");
        result = 1;
    }
    fixture_destroy(&refusal);
    fixture_destroy(&success);
    if (result == 0) {
        puts("LEIR AOT io_uring integration passed");
    }
    return result;
}

#endif
