// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_aot_integration_linux.h"

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_linux_metadata_test.h"
#include "leir_aot_linux.h"
#include "leir_test_support.h"

#include "llam/runtime.h"
#include "runtime_internal.h"

#include <errno.h>
#include <stdatomic.h>
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
#include <sched.h>
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

typedef struct cancel_rebind_case {
    linux_candidate_t candidate;
    leir_phase0_value_t
        cancelled_values_out[LINUX_INTEGRATION_SLOT_COUNT];
    leir_aot_resume_result_v1_t cancelled_resume;
    leir_aot_linux_metrics_t cancelled_metrics;
    atomic_uint cancellation_done;
    int cancelled_bind_result;
    int cancelled_bind_errno;
    int cancelled_run_result;
    int cancelled_run_errno;
    int rebind_error;
} cancel_rebind_case_t;

typedef struct native_queue_gate {
    atomic_uint locked;
    atomic_uint *release;
} native_queue_gate_t;

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

static int candidate_destroy(linux_candidate_t *candidate) {
    int result = 0;

    if (candidate->ticket_initialized) {
        result = leir_aot_linux_ticket_destroy(candidate->ticket);
    }
    candidate->ticket_initialized = false;
    leir_test_close(&candidate->client);
    return result;
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

static void cancelled_candidate_task(void *argument) {
    cancel_rebind_case_t *test_case = argument;
    linux_candidate_t *candidate = &test_case->candidate;

    errno = 0;
    test_case->cancelled_bind_result = leir_aot_linux_ticket_bind(
        candidate->ticket, candidate->values,
        LINUX_INTEGRATION_SLOT_COUNT);
    test_case->cancelled_bind_errno = errno;
    if (test_case->cancelled_bind_result == 0) {
        errno = 0;
        test_case->cancelled_run_result = leir_aot_linux_ticket_run(
            candidate->ticket, test_case->cancelled_values_out,
            LINUX_INTEGRATION_SLOT_COUNT,
            &test_case->cancelled_resume,
            &test_case->cancelled_metrics);
        test_case->cancelled_run_errno = errno;
    }
    atomic_store_explicit(
        &test_case->cancellation_done, 1U, memory_order_release);
}

static void rebound_candidate_task(void *argument) {
    cancel_rebind_case_t *test_case = argument;
    linux_candidate_t *candidate = &test_case->candidate;

    while (atomic_load_explicit(
               &test_case->cancellation_done,
               memory_order_acquire) == 0U) {
        llam_yield();
    }
    leir_test_close(&candidate->client);
    if (leir_test_tcp_client(&candidate->client) != 0 ||
        leir_test_connect_write_values(
            candidate->values, LINUX_INTEGRATION_SLOT_COUNT,
            candidate->client, &candidate->address,
            candidate->address_length, candidate->payload,
            sizeof(candidate->payload)) != 0) {
        test_case->rebind_error = errno != 0 ? errno : EIO;
        return;
    }
    memset(candidate->values_out, 0, sizeof(candidate->values_out));
    memset(&candidate->resume, 0, sizeof(candidate->resume));
    memset(&candidate->metrics, 0, sizeof(candidate->metrics));
    candidate_task(candidate);
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

static void *native_queue_gate_main(void *argument) {
    native_queue_gate_t *gate = argument;

    llam_fd_watch_lifecycle_lock();
    atomic_store_explicit(&gate->locked, 1U, memory_order_release);
    while (atomic_load_explicit(
               gate->release, memory_order_acquire) == 0U) {
        sched_yield();
    }
    llam_fd_watch_lifecycle_unlock();
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

static bool cancelled_run_is_valid(
    const cancel_rebind_case_t *test_case) {
    const leir_aot_linux_metrics_t *metrics =
        &test_case->cancelled_metrics;

    return test_case->cancelled_bind_result == 0 &&
           test_case->cancelled_run_result == 0 &&
           test_case->cancelled_resume.action ==
               LEIR_AOT_RESUME_FAIL &&
           test_case->cancelled_resume.error_code == ECANCELED &&
           test_case->cancelled_values_out[3].i64 == -1 &&
           test_case->cancelled_values_out[6].i64 == -1 &&
           metrics->terminal_publications == 1U &&
           metrics->normalizations == 1U &&
           metrics->site_lookups == 1U &&
           metrics->interpreter_dispatches == 0U &&
           metrics->activations == 1U &&
           metrics->logical_operations == 2U &&
           metrics->queue_publications == 1U &&
           metrics->prepared_sqes == 0U &&
           metrics->observed_cqes == 0U &&
           metrics->suppressed_success_cqes == 0U &&
           metrics->task_parks == 1U &&
           metrics->terminal_wakes == 0U &&
           metrics->hot_allocations == 0U &&
           metrics->first_error_operation == UINT32_MAX &&
           metrics->resumed_continuation ==
               LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR;
}

static int run_cancel_rebind_case(void) {
    cancel_rebind_case_t test_case;
    native_queue_gate_t queue_gate;
    linux_peer_t peer;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_spawn_opts_t spawn_options;
    llam_cancel_token_t *token = NULL;
    llam_task_t *cancel_task = NULL;
    llam_task_t *rebind_task = NULL;
    pthread_t peer_thread;
    pthread_t queue_gate_thread;
    bool runtime_started = false;
    bool peer_started = false;
    bool queue_gate_started = false;
    bool run_finished = false;
    int result = 1;

    memset(&test_case, 0, sizeof(test_case));
    memset(&peer, 0, sizeof(peer));
    memset(&queue_gate, 0, sizeof(queue_gate));
    memset(&options, 0, sizeof(options));
    test_case.candidate.client = LLAM_INVALID_FD;
    test_case.cancelled_bind_result = -2;
    test_case.cancelled_run_result = -2;
    peer.listener = LLAM_INVALID_FD;
    atomic_init(&test_case.cancellation_done, 0U);
    atomic_init(&queue_gate.locked, 0U);
    queue_gate.release = &test_case.cancellation_done;
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        if (leir_test_backend_is_unavailable(errno)) {
            fprintf(stderr,
                    "SKIP: Linux cancel/rebind runtime unavailable: %s\n",
                    strerror(errno));
            return 0;
        }
        perror("Linux AOT cancel/rebind runtime");
        return 1;
    }
    runtime_started = true;
    if (leir_test_tcp_listener(
            &peer.listener, &address, &address_length) != 0 ||
        candidate_init(
            &test_case.candidate, &address, address_length,
            UINT64_C(0x43414e43454c)) != 0) {
        perror("Linux AOT cancel/rebind fixtures");
        goto cleanup;
    }
    token = llam_cancel_token_create();
    if (token == NULL || llam_cancel_token_cancel(token) != 0 ||
        llam_spawn_opts_init(
            &spawn_options, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        perror("Linux AOT cancel/rebind token");
        goto cleanup;
    }
    spawn_options.cancel_token = token;
    if (pthread_create(
            &queue_gate_thread, NULL,
            native_queue_gate_main, &queue_gate) != 0) {
        perror("Linux AOT cancel/rebind queue gate");
        goto cleanup;
    }
    queue_gate_started = true;
    while (atomic_load_explicit(
               &queue_gate.locked, memory_order_acquire) == 0U) {
        sched_yield();
    }
    if (pthread_create(&peer_thread, NULL, peer_main, &peer) != 0) {
        perror("Linux AOT cancel/rebind peer");
        goto cleanup;
    }
    peer_started = true;
    cancel_task = llam_spawn(
        cancelled_candidate_task, &test_case, &spawn_options);
    rebind_task = llam_spawn(rebound_candidate_task, &test_case, NULL);
    if (cancel_task == NULL || rebind_task == NULL || llam_run() != 0) {
        perror("Linux AOT cancel/rebind run");
        goto cleanup;
    }
    run_finished = true;
    if (llam_join(cancel_task) != 0 || llam_join(rebind_task) != 0) {
        perror("Linux AOT cancel/rebind join");
        goto cleanup;
    }
    cancel_task = NULL;
    rebind_task = NULL;
    if (test_case.cancelled_run_result != 0 &&
        leir_test_backend_is_unavailable(
            test_case.cancelled_run_errno)) {
        fprintf(stderr,
                "SKIP: Linux cancel/rebind segment unavailable: %s\n",
                strerror(test_case.cancelled_run_errno));
        result = 0;
        goto cleanup;
    }
    if (!cancelled_run_is_valid(&test_case) ||
        test_case.rebind_error != 0 ||
        !success_is_valid(&test_case.candidate) ||
        test_case.candidate.metrics.terminal_publications != 1U ||
        test_case.candidate.metrics.normalizations != 1U ||
        test_case.candidate.metrics.site_lookups != 1U ||
        test_case.candidate.metrics.interpreter_dispatches != 0U ||
        test_case.candidate.metrics.prepared_sqes != 2U ||
        test_case.candidate.metrics.observed_cqes != 1U ||
        test_case.candidate.metrics.suppressed_success_cqes != 1U) {
        fputs("Linux AOT cancel/rebind lifecycle mismatch\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (queue_gate_started) {
        atomic_store_explicit(
            &test_case.cancellation_done, 1U, memory_order_release);
        (void)pthread_join(queue_gate_thread, NULL);
        queue_gate_started = false;
    }
    if (runtime_started && !run_finished) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (cancel_task != NULL) {
        (void)llam_join(cancel_task);
    }
    if (rebind_task != NULL) {
        (void)llam_join(rebind_task);
    }
    if (result != 0 && peer.listener >= 0) {
        (void)shutdown((int)peer.listener, SHUT_RDWR);
    }
    if (peer_started) {
        (void)pthread_join(peer_thread, NULL);
    }
    if (result == 0 && test_case.candidate.run_result == 0 &&
        (peer.error_code != 0 ||
         peer.observed_size != sizeof(test_case.candidate.payload) ||
         memcmp(peer.observed, test_case.candidate.payload,
                sizeof(test_case.candidate.payload)) != 0)) {
        fputs("Linux AOT cancel/rebind peer mismatch\n", stderr);
        result = 1;
    }
    leir_test_close(&peer.listener);
    if (candidate_destroy(&test_case.candidate) != 0) {
        fputs("Linux AOT cancel/rebind destroy mismatch\n", stderr);
        result = 1;
    }
    if (token != NULL && llam_cancel_token_destroy(token) != 0) {
        fputs("Linux AOT cancel/rebind token destroy mismatch\n", stderr);
        result = 1;
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    return result;
}

static int run_success_refusal_case(void) {
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
        success.metrics.terminal_publications != 1U ||
        success.metrics.normalizations != 1U ||
        success.metrics.site_lookups != 1U ||
        success.metrics.interpreter_dispatches != 0U ||
        success.metrics.prepared_sqes != 2U ||
        success.metrics.observed_cqes != 1U ||
        success.metrics.suppressed_success_cqes != 1U ||
        success.metrics.task_parks != 1U ||
        success.metrics.terminal_wakes != 1U ||
        success.metrics.hot_allocations != 0U ||
        success.metrics.resumed_continuation !=
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT ||
        refusal.metrics.terminal_publications != 1U ||
        refusal.metrics.normalizations != 1U ||
        refusal.metrics.site_lookups != 1U ||
        refusal.metrics.interpreter_dispatches != 0U ||
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
    if (candidate_destroy(&refusal) != 0 ||
        candidate_destroy(&success) != 0) {
        fputs("Linux AOT ticket destroy mismatch\n", stderr);
        result = 1;
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    return result;
}

int leir_aot_linux_integration_run(void) {
    if (leir_aot_linux_metadata_test_run() != 0) {
        return 1;
    }
    if (run_cancel_rebind_case() != 0) {
        return 1;
    }
    return run_success_refusal_case();
}

#endif
