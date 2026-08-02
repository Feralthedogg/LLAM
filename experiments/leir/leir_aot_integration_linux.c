// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_integration_linux.h"

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_linux.h"
#include "leir_test_support.h"

#include "llam/runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_LINUX

int leir_aot_linux_integration_run(void) {
    puts("SKIP: Linux AOT specialization is unavailable on this platform");
    return 0;
}

#else

#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    LINUX_INTEGRATION_PAYLOAD_SIZE = 64,
    LINUX_INTEGRATION_SLOT_COUNT = 7,
    LINUX_INTEGRATION_STORAGE_SIZE = 32768,
    LINUX_INTEGRATION_TIMEOUT_MS = 5000,
};

typedef union linux_integration_storage {
    max_align_t alignment;
    unsigned char bytes[LINUX_INTEGRATION_STORAGE_SIZE];
} linux_integration_storage_t;

typedef struct linux_candidate {
    llam_fd_t client;
    struct sockaddr_storage address;
    socklen_t address_length;
    unsigned char payload[LINUX_INTEGRATION_PAYLOAD_SIZE];
    leir_phase0_value_t values[LINUX_INTEGRATION_SLOT_COUNT];
    leir_phase0_value_t values_out[LINUX_INTEGRATION_SLOT_COUNT];
    linux_integration_storage_t ticket_storage;
    linux_integration_storage_t module_storage;
    leir_aot_linux_ticket_t *ticket;
    leir_aot_resume_result_v1_t resume;
    leir_aot_linux_metrics_t metrics;
    int bind_result;
    int bind_errno;
    int run_result;
    int run_errno;
    bool ticket_initialized;
} linux_candidate_t;

typedef struct linux_peer {
    llam_fd_t listener;
    unsigned char observed[LINUX_INTEGRATION_PAYLOAD_SIZE];
    size_t observed_size;
    int error_code;
} linux_peer_t;

static int candidate_init(
    linux_candidate_t *candidate,
    const struct sockaddr_storage *address,
    socklen_t address_length,
    uint64_t seed) {
    size_t module_size =
        leir_aot_connect_write_module_v1.instance_size;
    size_t ticket_size = leir_aot_linux_ticket_size();

    memset(candidate, 0, sizeof(*candidate));
    candidate->client = LLAM_INVALID_FD;
    if (address == NULL || address_length == 0U ||
        (size_t)address_length > sizeof(candidate->address) ||
        ticket_size > sizeof(candidate->ticket_storage.bytes) ||
        module_size > sizeof(candidate->module_storage.bytes) ||
        leir_aot_linux_ticket_alignment() >
            _Alignof(linux_integration_storage_t) ||
        leir_test_tcp_client(&candidate->client) != 0) {
        return -1;
    }
    memcpy(&candidate->address, address, (size_t)address_length);
    candidate->address_length = address_length;
    leir_test_fill_pattern(
        candidate->payload, sizeof(candidate->payload), seed);
    if (leir_test_connect_write_values(
            candidate->values, LINUX_INTEGRATION_SLOT_COUNT,
            candidate->client, &candidate->address,
            candidate->address_length, candidate->payload,
            sizeof(candidate->payload)) != 0 ||
        leir_aot_linux_ticket_init(
            candidate->ticket_storage.bytes,
            sizeof(candidate->ticket_storage.bytes),
            &leir_aot_connect_write_module_v1,
            candidate->module_storage.bytes,
            sizeof(candidate->module_storage.bytes)) != 0) {
        return -1;
    }
    candidate->ticket = (leir_aot_linux_ticket_t *)(void *)
        candidate->ticket_storage.bytes;
    candidate->ticket_initialized = true;
    return 0;
}

static void candidate_destroy(linux_candidate_t *candidate) {
    if (candidate->ticket_initialized) {
        (void)leir_aot_linux_ticket_destroy(candidate->ticket);
    }
    candidate->ticket_initialized = false;
    leir_test_close(&candidate->client);
}

static void candidate_task(void *argument) {
    linux_candidate_t *candidate = argument;

    errno = 0;
    candidate->bind_result = leir_aot_linux_ticket_bind(
        candidate->ticket, candidate->values,
        LINUX_INTEGRATION_SLOT_COUNT);
    candidate->bind_errno = errno;
    if (candidate->bind_result != 0) {
        return;
    }
    errno = 0;
    candidate->run_result = leir_aot_linux_ticket_run(
        candidate->ticket, candidate->values_out,
        LINUX_INTEGRATION_SLOT_COUNT, &candidate->resume,
        &candidate->metrics);
    candidate->run_errno = errno;
}

static void *peer_main(void *argument) {
    linux_peer_t *peer = argument;
    struct pollfd listener_poll = {
        .fd = peer->listener,
        .events = POLLIN,
    };
    int accepted;

    if (poll(&listener_poll, 1U, LINUX_INTEGRATION_TIMEOUT_MS) <= 0) {
        peer->error_code = errno != 0 ? errno : ETIMEDOUT;
        return NULL;
    }
    accepted = accept4(peer->listener, NULL, NULL,
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

        if (poll(&read_poll, 1U, LINUX_INTEGRATION_TIMEOUT_MS) <= 0) {
            peer->error_code = errno != 0 ? errno : ETIMEDOUT;
            break;
        }
        received = recv(
            accepted, peer->observed + peer->observed_size,
            sizeof(peer->observed) - peer->observed_size, 0);
        if (received <= 0) {
            peer->error_code = received == 0 ? ECONNRESET : errno;
            break;
        }
        peer->observed_size += (size_t)received;
    }
    (void)close(accepted);
    return NULL;
}

static bool success_is_valid(const linux_candidate_t *candidate) {
    return candidate->bind_result == 0 &&
           candidate->run_result == 0 &&
           candidate->resume.action == LEIR_AOT_RESUME_RETURN &&
           candidate->resume.error_code == 0 &&
           candidate->values_out[3].i64 == 0 &&
           candidate->values_out[6].i64 ==
               LINUX_INTEGRATION_PAYLOAD_SIZE;
}

static bool refusal_is_valid(const linux_candidate_t *candidate) {
    return candidate->bind_result == 0 &&
           candidate->run_result == 0 &&
           candidate->resume.action == LEIR_AOT_RESUME_FAIL &&
           candidate->resume.error_code == ECONNREFUSED &&
           candidate->values_out[3].i64 == -1 &&
           candidate->values_out[6].i64 == -1;
}

int leir_aot_linux_integration_run(void) {
    linux_candidate_t success;
    linux_candidate_t refusal;
    linux_peer_t peer;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_task_t *success_task = NULL;
    llam_task_t *refusal_task = NULL;
    pthread_t peer_thread;
    bool runtime_started = false;
    bool peer_started = false;
    bool run_finished = false;
    int result = 1;

    memset(&success, 0, sizeof(success));
    memset(&refusal, 0, sizeof(refusal));
    memset(&peer, 0, sizeof(peer));
    success.client = LLAM_INVALID_FD;
    refusal.client = LLAM_INVALID_FD;
    peer.listener = LLAM_INVALID_FD;
    memset(&options, 0, sizeof(options));
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        if (leir_test_backend_is_unavailable(errno)) {
            fprintf(stderr, "SKIP: Linux AOT runtime unavailable: %s\n",
                    strerror(errno));
            return 0;
        }
        perror("Linux AOT integration runtime");
        goto cleanup;
    }
    runtime_started = true;
    if (leir_test_tcp_listener(
            &peer.listener, &address, &address_length) != 0 ||
        candidate_init(
            &success, &address, address_length, UINT64_C(0x31)) != 0 ||
        leir_test_tcp_refused_address(&address, &address_length) != 0 ||
        candidate_init(
            &refusal, &address, address_length, UINT64_C(0x91)) != 0) {
        perror("Linux AOT integration fixtures");
        goto cleanup;
    }
    if (pthread_create(&peer_thread, NULL, peer_main, &peer) != 0) {
        perror("Linux AOT integration peer");
        goto cleanup;
    }
    peer_started = true;
    success_task = llam_spawn(candidate_task, &success, NULL);
    refusal_task = llam_spawn(candidate_task, &refusal, NULL);
    if (success_task == NULL || refusal_task == NULL ||
        llam_run() != 0) {
        perror("Linux AOT integration run");
        goto cleanup;
    }
    run_finished = true;
    if (llam_join(success_task) != 0 || llam_join(refusal_task) != 0) {
        perror("Linux AOT integration join");
        goto cleanup;
    }
    success_task = NULL;
    refusal_task = NULL;
    if ((success.run_result != 0 &&
         leir_test_backend_is_unavailable(success.run_errno)) ||
        (refusal.run_result != 0 &&
         leir_test_backend_is_unavailable(refusal.run_errno))) {
        int unavailable_error = success.run_result != 0
            ? success.run_errno
            : refusal.run_errno;

        fprintf(stderr, "SKIP: Linux AOT segment unavailable: %s\n",
                strerror(unavailable_error));
        result = 0;
        goto cleanup;
    }
    if (!success_is_valid(&success) || !refusal_is_valid(&refusal) ||
        success.metrics.activations != 1U ||
        success.metrics.logical_operations != 2U ||
        success.metrics.prepared_sqes != 2U ||
        success.metrics.observed_cqes != 1U ||
        success.metrics.suppressed_success_cqes != 1U ||
        success.metrics.task_parks != 1U ||
        success.metrics.terminal_wakes != 1U ||
        success.metrics.hot_allocations != 0U ||
        success.metrics.resumed_continuation !=
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT ||
        refusal.metrics.first_error_operation != 0U ||
        refusal.metrics.resumed_continuation !=
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR ||
        refusal.metrics.terminal_wakes != 1U) {
        fputs("Linux AOT semantic or CQE matrix mismatch\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started && !run_finished) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (success_task != NULL) {
        (void)llam_join(success_task);
    }
    if (refusal_task != NULL) {
        (void)llam_join(refusal_task);
    }
    leir_test_close(&peer.listener);
    if (peer_started) {
        (void)pthread_join(peer_thread, NULL);
    }
    if (result == 0 && success.run_result == 0 &&
        (peer.error_code != 0 ||
         peer.observed_size != sizeof(success.payload) ||
         memcmp(peer.observed, success.payload,
                sizeof(success.payload)) != 0)) {
        fputs("Linux AOT peer payload mismatch\n", stderr);
        result = 1;
    }
    candidate_destroy(&refusal);
    candidate_destroy(&success);
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    return result;
}

#endif
