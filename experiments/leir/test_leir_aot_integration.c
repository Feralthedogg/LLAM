// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_integration_linux.h"
#include "leir_aot_portable.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"

#include "llam/runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    AOT_INTEGRATION_PAYLOAD_SIZE = 64,
    AOT_INTEGRATION_SLOT_COUNT = 7,
    AOT_INTEGRATION_STORAGE_SIZE = 32768,
    AOT_INTEGRATION_SKIP = 77,
};

typedef union aot_storage {
    leir_aot_storage_align_t alignment;
    unsigned char bytes[AOT_INTEGRATION_STORAGE_SIZE];
} aot_storage_t;

typedef enum aot_candidate_kind {
    AOT_CANDIDATE_ORACLE = 0,
    AOT_CANDIDATE_PORTABLE = 1,
} aot_candidate_kind_t;

typedef struct aot_candidate {
    aot_candidate_kind_t kind;
    const leir_phase0_program_t *program;
    llam_fd_t client;
    struct sockaddr_storage address;
    socklen_t address_length;
    unsigned char payload[AOT_INTEGRATION_PAYLOAD_SIZE];
    leir_phase0_value_t values[AOT_INTEGRATION_SLOT_COUNT];
    leir_phase0_value_t values_out[AOT_INTEGRATION_SLOT_COUNT];
    leir_phase0_instance_t oracle_instance;
    aot_storage_t ticket_storage;
    aot_storage_t module_storage;
    leir_aot_portable_ticket_t *portable_ticket;
    leir_aot_resume_result_v1_t resume;
    leir_phase0_metrics_t oracle_metrics;
    leir_aot_portable_metrics_t portable_metrics;
    int bind_result;
    int bind_errno;
    int run_result;
    int run_errno;
    bool ticket_initialized;
} aot_candidate_t;

typedef struct aot_peer {
    llam_fd_t listener;
    unsigned char observed[AOT_INTEGRATION_PAYLOAD_SIZE];
    size_t observed_size;
    int error_code;
} aot_peer_t;

typedef struct ownership_state {
    aot_candidate_t first;
    aot_candidate_t second;
    aot_peer_t peer;
    leir_aot_portable_metrics_t rebound_metrics;
    leir_aot_resume_result_v1_t rebound_resume;
    leir_phase0_value_t rebound_outputs[AOT_INTEGRATION_SLOT_COUNT];
    leir_phase0_value_t second_snapshot[AOT_INTEGRATION_SLOT_COUNT];
    int error_code;
} ownership_state_t;

static int candidate_init(
    aot_candidate_t *candidate,
    aot_candidate_kind_t kind,
    const leir_phase0_program_t *program,
    const struct sockaddr_storage *address,
    socklen_t address_length,
    uint64_t seed) {
    size_t module_size =
        leir_aot_connect_write_module_v1.instance_size;

    memset(candidate, 0, sizeof(*candidate));
    candidate->kind = kind;
    candidate->program = program;
    candidate->client = LLAM_INVALID_FD;
    candidate->bind_result = INT_MIN;
    candidate->run_result = INT_MIN;
    if (address == NULL || address_length == 0U ||
        (size_t)address_length > sizeof(candidate->address) ||
        module_size > sizeof(candidate->module_storage.bytes) ||
        leir_test_tcp_client(&candidate->client) != 0) {
        return -1;
    }
    memcpy(&candidate->address, address, (size_t)address_length);
    candidate->address_length = address_length;
    leir_test_fill_pattern(
        candidate->payload, sizeof(candidate->payload), seed);
    if (leir_test_connect_write_values(
            candidate->values, AOT_INTEGRATION_SLOT_COUNT,
            candidate->client, &candidate->address,
            candidate->address_length, candidate->payload,
            sizeof(candidate->payload)) != 0) {
        return -1;
    }

    if (kind == AOT_CANDIDATE_PORTABLE) {
        size_t ticket_size = leir_aot_portable_ticket_size();

        if (ticket_size > sizeof(candidate->ticket_storage.bytes) ||
            leir_aot_portable_ticket_alignment() >
                _Alignof(aot_storage_t) ||
            leir_aot_portable_ticket_init(
                candidate->ticket_storage.bytes,
                sizeof(candidate->ticket_storage.bytes),
                &leir_aot_connect_write_module_v1,
                candidate->module_storage.bytes,
                sizeof(candidate->module_storage.bytes), NULL) != 0) {
            return -1;
        }
        candidate->portable_ticket =
            (leir_aot_portable_ticket_t *)(void *)
                candidate->ticket_storage.bytes;
        candidate->ticket_initialized = true;
    }
    return 0;
}

static void candidate_destroy(aot_candidate_t *candidate) {
    if (candidate->ticket_initialized) {
        (void)leir_aot_portable_ticket_destroy(
            candidate->portable_ticket);
    }
    candidate->ticket_initialized = false;
    leir_test_close(&candidate->client);
}

static void run_oracle(aot_candidate_t *candidate) {
    const leir_phase0_run_opts_t options = {
        .inline_budget = 8U,
        .force_backend = true,
    };

    errno = 0;
    candidate->bind_result = leir_phase0_instance_init(
        &candidate->oracle_instance,
        sizeof(candidate->oracle_instance), candidate->program);
    candidate->bind_errno = errno;
    if (candidate->bind_result == 0) {
        errno = 0;
        candidate->bind_result = leir_phase0_instance_bind(
            &candidate->oracle_instance, candidate->values,
            AOT_INTEGRATION_SLOT_COUNT, &options);
        candidate->bind_errno = errno;
    }
    if (candidate->bind_result != 0) {
        return;
    }
    errno = 0;
    candidate->run_result = leir_phase0_instance_run(
        &candidate->oracle_instance, candidate->values_out,
        AOT_INTEGRATION_SLOT_COUNT, &candidate->oracle_metrics);
    candidate->run_errno = errno;
    candidate->resume.action = candidate->run_result == 0
        ? LEIR_AOT_RESUME_RETURN
        : LEIR_AOT_RESUME_FAIL;
    candidate->resume.error_code = candidate->run_result == 0
        ? 0
        : candidate->run_errno;
}

static void run_portable(aot_candidate_t *candidate) {
    errno = 0;
    candidate->bind_result = leir_aot_portable_ticket_bind(
        candidate->portable_ticket, candidate->values,
        AOT_INTEGRATION_SLOT_COUNT);
    candidate->bind_errno = errno;
    if (candidate->bind_result != 0) {
        return;
    }
    errno = 0;
    candidate->run_result = leir_aot_portable_ticket_run(
        candidate->portable_ticket, candidate->values_out,
        AOT_INTEGRATION_SLOT_COUNT, &candidate->resume,
        &candidate->portable_metrics);
    candidate->run_errno = errno;
}

static void candidate_task(void *argument) {
    aot_candidate_t *candidate = argument;

    if (candidate->kind == AOT_CANDIDATE_ORACLE) {
        run_oracle(candidate);
    } else {
        run_portable(candidate);
    }
}

static void peer_task(void *argument) {
    aot_peer_t *peer = argument;
    llam_fd_t accepted = llam_accept(peer->listener, NULL, NULL);

    if (LLAM_FD_IS_INVALID(accepted)) {
        peer->error_code = errno != 0 ? errno : EIO;
        return;
    }
    if (leir_test_read_exact(
            accepted, peer->observed, sizeof(peer->observed)) != 0) {
        peer->error_code = errno != 0 ? errno : EIO;
    } else {
        peer->observed_size = sizeof(peer->observed);
    }
    leir_test_close(&accepted);
}

static bool semantic_success_is_valid(
    const aot_candidate_t *candidate) {
    return candidate->bind_result == 0 &&
           candidate->run_result == 0 &&
           candidate->resume.action == LEIR_AOT_RESUME_RETURN &&
           candidate->resume.error_code == 0 &&
           candidate->values_out[2].u64 ==
               (uint64_t)candidate->address_length &&
           candidate->values_out[3].i64 == 0 &&
           candidate->values_out[5].u64 ==
               AOT_INTEGRATION_PAYLOAD_SIZE &&
           candidate->values_out[6].i64 ==
               AOT_INTEGRATION_PAYLOAD_SIZE;
}

static bool semantic_refusal_is_valid(
    const aot_candidate_t *candidate) {
    return candidate->bind_result == 0 &&
           candidate->resume.action == LEIR_AOT_RESUME_FAIL &&
           candidate->resume.error_code == ECONNREFUSED &&
           candidate->values_out[3].i64 == -1 &&
           candidate->values_out[6].i64 == -1;
}

static bool portable_metrics_are_valid(
    const leir_aot_portable_metrics_t *metrics,
    uint64_t effect_calls,
    uint32_t continuation) {
    return metrics->activations == 1U &&
           metrics->logical_operations == 2U &&
           metrics->effect_calls == effect_calls &&
           metrics->terminal_publications == 1U &&
           metrics->interpreter_dispatches == 0U &&
           metrics->normalizations == 1U &&
           metrics->site_lookups == 1U &&
           metrics->hot_allocations == 0U &&
           metrics->generation != 0U &&
           metrics->resumed_continuation == continuation;
}

static int probe_oracle_backend(
    const leir_phase0_program_t *program) {
    aot_candidate_t probe;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_task_t *task = NULL;
    bool runtime_started = false;
    bool run_finished = false;
    int result = 1;

    memset(&probe, 0, sizeof(probe));
    probe.client = LLAM_INVALID_FD;
    if (leir_test_tcp_refused_address(
            &address, &address_length) != 0 ||
        candidate_init(
            &probe, AOT_CANDIDATE_ORACLE, program,
            &address, address_length, UINT64_C(0x50524f4245)) != 0) {
        perror("portable integration backend probe fixtures");
        goto cleanup;
    }
    memset(&options, 0, sizeof(options));
    options.deterministic = 1U;
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        if (leir_test_backend_is_unavailable(errno)) {
            fprintf(stderr,
                    "SKIP: phase0 backend unavailable: %s\n",
                    strerror(errno));
            result = AOT_INTEGRATION_SKIP;
            goto cleanup;
        }
        perror("portable integration backend probe runtime");
        goto cleanup;
    }
    runtime_started = true;
    task = llam_spawn(candidate_task, &probe, NULL);
    if (task == NULL || llam_run() != 0) {
        perror("portable integration backend probe run");
        goto cleanup;
    }
    run_finished = true;
    if (llam_join(task) != 0) {
        perror("portable integration backend probe join");
        goto cleanup;
    }
    task = NULL;
    if (probe.run_result != 0 &&
        leir_test_backend_is_unavailable(probe.run_errno)) {
        fprintf(stderr,
                "SKIP: phase0 backend unavailable: %s\n",
                strerror(probe.run_errno));
        result = AOT_INTEGRATION_SKIP;
        goto cleanup;
    }
    if (!semantic_refusal_is_valid(&probe) ||
        probe.run_result != -1) {
        fputs("portable integration backend probe mismatch\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started && !run_finished) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (task != NULL) {
        (void)llam_join(task);
    }
    candidate_destroy(&probe);
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    return result;
}

static bool peers_match(
    const aot_peer_t *peer,
    const aot_candidate_t *candidate) {
    return peer->error_code == 0 &&
           peer->observed_size == sizeof(candidate->payload) &&
           memcmp(peer->observed, candidate->payload,
                  sizeof(candidate->payload)) == 0;
}

static bool success_results_match(
    const aot_candidate_t *oracle,
    const aot_candidate_t *portable) {
    return oracle->resume.action == portable->resume.action &&
           oracle->resume.error_code == portable->resume.error_code &&
           oracle->values_out[2].u64 == portable->values_out[2].u64 &&
           oracle->values_out[3].i64 == portable->values_out[3].i64 &&
           oracle->values_out[5].u64 == portable->values_out[5].u64 &&
           oracle->values_out[6].i64 == portable->values_out[6].i64 &&
           memcmp(oracle->payload, portable->payload,
                  sizeof(oracle->payload)) == 0;
}

static int run_portable_oracle_matrix(void) {
    leir_phase0_program_t *program = NULL;
    aot_candidate_t oracle_success;
    aot_candidate_t portable_success;
    aot_candidate_t oracle_refusal;
    aot_candidate_t portable_refusal;
    aot_peer_t oracle_peer;
    aot_peer_t portable_peer;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_task_t *tasks[6] = {0};
    bool runtime_started = false;
    bool run_finished = false;
    size_t i;
    int probe_result;
    int result = 1;

    memset(&oracle_success, 0, sizeof(oracle_success));
    memset(&portable_success, 0, sizeof(portable_success));
    memset(&oracle_refusal, 0, sizeof(oracle_refusal));
    memset(&portable_refusal, 0, sizeof(portable_refusal));
    oracle_success.client = LLAM_INVALID_FD;
    portable_success.client = LLAM_INVALID_FD;
    oracle_refusal.client = LLAM_INVALID_FD;
    portable_refusal.client = LLAM_INVALID_FD;
    memset(&oracle_peer, 0, sizeof(oracle_peer));
    memset(&portable_peer, 0, sizeof(portable_peer));
    oracle_peer.listener = LLAM_INVALID_FD;
    portable_peer.listener = LLAM_INVALID_FD;
    if (leir_test_connect_write_program_create(&program) != 0) {
        perror("portable integration program");
        goto cleanup;
    }
    probe_result = probe_oracle_backend(program);
    if (probe_result != 0) {
        result = probe_result;
        goto cleanup;
    }
    memset(&options, 0, sizeof(options));
    options.deterministic = 1U;
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        perror("portable integration runtime");
        goto cleanup;
    }
    runtime_started = true;

    if (leir_test_tcp_listener(
            &oracle_peer.listener, &address, &address_length) != 0 ||
        candidate_init(
            &oracle_success, AOT_CANDIDATE_ORACLE, program,
            &address, address_length, UINT64_C(0x31)) != 0 ||
        leir_test_tcp_listener(
            &portable_peer.listener, &address, &address_length) != 0 ||
        candidate_init(
            &portable_success, AOT_CANDIDATE_PORTABLE, program,
            &address, address_length, UINT64_C(0x31)) != 0 ||
        leir_test_tcp_refused_address(&address, &address_length) != 0 ||
        candidate_init(
            &oracle_refusal, AOT_CANDIDATE_ORACLE, program,
            &address, address_length, UINT64_C(0x91)) != 0 ||
        leir_test_tcp_refused_address(&address, &address_length) != 0 ||
        candidate_init(
            &portable_refusal, AOT_CANDIDATE_PORTABLE, program,
            &address, address_length, UINT64_C(0x91)) != 0) {
        perror("portable integration fixtures");
        goto cleanup;
    }

    tasks[0] = llam_spawn(peer_task, &oracle_peer, NULL);
    tasks[1] = llam_spawn(peer_task, &portable_peer, NULL);
    tasks[2] = llam_spawn(candidate_task, &oracle_success, NULL);
    tasks[3] = llam_spawn(candidate_task, &portable_success, NULL);
    tasks[4] = llam_spawn(candidate_task, &oracle_refusal, NULL);
    tasks[5] = llam_spawn(candidate_task, &portable_refusal, NULL);
    for (i = 0U; i < sizeof(tasks) / sizeof(tasks[0]); i += 1U) {
        if (tasks[i] == NULL) {
            perror("portable integration spawn");
            goto cleanup;
        }
    }
    if (llam_run() != 0) {
        perror("portable integration run");
        goto cleanup;
    }
    run_finished = true;
    for (i = 0U; i < sizeof(tasks) / sizeof(tasks[0]); i += 1U) {
        if (llam_join(tasks[i]) != 0) {
            perror("portable integration join");
            goto cleanup;
        }
        tasks[i] = NULL;
    }

    if (!semantic_success_is_valid(&oracle_success) ||
        !semantic_success_is_valid(&portable_success) ||
        !semantic_refusal_is_valid(&oracle_refusal) ||
        !semantic_refusal_is_valid(&portable_refusal) ||
        oracle_refusal.run_result != -1 ||
        portable_refusal.run_result != 0 ||
        !success_results_match(&oracle_success, &portable_success) ||
        !peers_match(&oracle_peer, &oracle_success) ||
        !peers_match(&portable_peer, &portable_success) ||
        oracle_success.oracle_metrics.terminal_publications != 1U ||
        oracle_refusal.oracle_metrics.terminal_publications != 1U ||
        !portable_metrics_are_valid(
            &portable_success.portable_metrics, 2U,
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) ||
        !portable_metrics_are_valid(
            &portable_refusal.portable_metrics, 1U,
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR)) {
        fputs("portable/oracle AOT semantic matrix mismatch\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started) {
        if (!run_finished) {
            (void)llam_runtime_request_stop();
            (void)llam_run();
        }
        for (i = 0U; i < sizeof(tasks) / sizeof(tasks[0]); i += 1U) {
            if (tasks[i] != NULL) {
                (void)llam_join(tasks[i]);
            }
        }
    }
    leir_test_close(&portable_peer.listener);
    leir_test_close(&oracle_peer.listener);
    candidate_destroy(&portable_refusal);
    candidate_destroy(&oracle_refusal);
    candidate_destroy(&portable_success);
    candidate_destroy(&oracle_success);
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_phase0_program_destroy(program);
    return result;
}

static void ownership_task(void *argument) {
    ownership_state_t *state = argument;
    leir_aot_portable_metrics_t first_metrics;
    leir_aot_resume_result_v1_t first_resume;
    leir_phase0_value_t first_outputs[AOT_INTEGRATION_SLOT_COUNT];

    errno = 0;
    state->first.bind_result = leir_aot_portable_ticket_bind(
        state->first.portable_ticket, state->first.values,
        AOT_INTEGRATION_SLOT_COUNT);
    state->first.bind_errno = errno;
    errno = 0;
    state->second.bind_result = leir_aot_portable_ticket_bind(
        state->second.portable_ticket, state->second.values,
        AOT_INTEGRATION_SLOT_COUNT);
    state->second.bind_errno = errno;
    if (state->first.bind_result != 0 ||
        state->second.bind_result != 0) {
        state->error_code = errno != 0 ? errno : EIO;
        return;
    }
    errno = 0;
    state->second.run_result = leir_aot_portable_ticket_run(
        state->second.portable_ticket, state->second.values_out,
        AOT_INTEGRATION_SLOT_COUNT, &state->second.resume,
        &state->second.portable_metrics);
    state->second.run_errno = errno;
    if (state->second.run_result != 0) {
        state->error_code = state->second.run_errno != 0
            ? state->second.run_errno
            : EIO;
        return;
    }
    memcpy(state->second_snapshot, state->second.values_out,
           sizeof(state->second_snapshot));
    if (leir_aot_portable_ticket_cancel(
            state->first.portable_ticket) != 0 ||
        leir_aot_portable_ticket_run(
            state->first.portable_ticket, first_outputs,
            AOT_INTEGRATION_SLOT_COUNT, &first_resume,
            &first_metrics) != 0) {
        state->error_code = errno != 0 ? errno : EIO;
        return;
    }
    state->first.resume = first_resume;
    state->first.portable_metrics = first_metrics;
    memcpy(state->first.values_out, first_outputs,
           sizeof(state->first.values_out));
    if (leir_aot_portable_ticket_bind(
            state->first.portable_ticket, state->first.values,
            AOT_INTEGRATION_SLOT_COUNT) != 0 ||
        leir_aot_portable_ticket_cancel(
            state->first.portable_ticket) != 0 ||
        leir_aot_portable_ticket_run(
            state->first.portable_ticket, state->rebound_outputs,
            AOT_INTEGRATION_SLOT_COUNT, &state->rebound_resume,
            &state->rebound_metrics) != 0) {
        state->error_code = errno != 0 ? errno : EIO;
    }
}

static int run_portable_ownership_case(void) {
    ownership_state_t state;
    struct sockaddr_storage address;
    socklen_t address_length;
    llam_runtime_opts_t options;
    llam_task_t *peer = NULL;
    llam_task_t *owner = NULL;
    bool runtime_started = false;
    bool run_finished = false;
    int result = 1;

    memset(&state, 0, sizeof(state));
    state.first.client = LLAM_INVALID_FD;
    state.second.client = LLAM_INVALID_FD;
    state.peer.listener = LLAM_INVALID_FD;
    memset(&options, 0, sizeof(options));
    options.deterministic = 1U;
    options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&options) != 0) {
        perror("portable ownership runtime");
        goto cleanup;
    }
    runtime_started = true;
    if (leir_test_tcp_listener(
            &state.peer.listener, &address, &address_length) != 0 ||
        candidate_init(
            &state.first, AOT_CANDIDATE_PORTABLE, NULL,
            &address, address_length, UINT64_C(0x51)) != 0 ||
        candidate_init(
            &state.second, AOT_CANDIDATE_PORTABLE, NULL,
            &address, address_length, UINT64_C(0x71)) != 0) {
        perror("portable ownership fixtures");
        goto cleanup;
    }
    peer = llam_spawn(peer_task, &state.peer, NULL);
    owner = llam_spawn(ownership_task, &state, NULL);
    if (peer == NULL || owner == NULL) {
        perror("portable ownership spawn");
        goto cleanup;
    }
    if (llam_run() != 0) {
        perror("portable ownership run");
        goto cleanup;
    }
    run_finished = true;
    if (llam_join(owner) != 0 || llam_join(peer) != 0) {
        perror("portable ownership join");
        goto cleanup;
    }
    owner = NULL;
    peer = NULL;
    if (state.error_code != 0 ||
        !semantic_success_is_valid(&state.second) ||
        !peers_match(&state.peer, &state.second) ||
        state.first.resume.action != LEIR_AOT_RESUME_FAIL ||
        state.first.resume.error_code != ECANCELED ||
        state.rebound_resume.action != LEIR_AOT_RESUME_FAIL ||
        state.rebound_resume.error_code != ECANCELED ||
        !portable_metrics_are_valid(
            &state.second.portable_metrics, 2U,
            LEIR_AOT_CONNECT_WRITE_WRITE_RESULT) ||
        !portable_metrics_are_valid(
            &state.first.portable_metrics, 0U,
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) ||
        !portable_metrics_are_valid(
            &state.rebound_metrics, 0U,
            LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR) ||
        state.rebound_metrics.generation <=
            state.first.portable_metrics.generation ||
        memcmp(state.second_snapshot, state.second.values_out,
               sizeof(state.second_snapshot)) != 0 ||
        state.first.portable_ticket == state.second.portable_ticket) {
        fputs("portable AOT ownership isolation mismatch\n", stderr);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started && !run_finished) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (owner != NULL) {
        (void)llam_join(owner);
    }
    if (peer != NULL) {
        (void)llam_join(peer);
    }
    leir_test_close(&state.peer.listener);
    candidate_destroy(&state.second);
    candidate_destroy(&state.first);
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    return result;
}

int main(void) {
    int result = run_portable_oracle_matrix();

    if (result != 0) {
        return result;
    }
    if (run_portable_ownership_case() != 0) {
        return 1;
    }
    if (leir_aot_linux_integration_run() != 0) {
        return 1;
    }
    puts("LEIR AOT portable integration passed");
    return 0;
}
