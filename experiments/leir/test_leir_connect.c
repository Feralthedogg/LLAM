// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/test_leir_connect.c
 * @brief Portable LEIR CONNECT contract and runtime tests.
 */

#include "runtime_internal.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"
#include "llam/io.h"
#include "llam/runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static int expect_program_error(
    const leir_phase0_program_desc_t *desc,
    int expected_error) {
    leir_phase0_program_t *program =
        (leir_phase0_program_t *)(uintptr_t)1U;

    errno = 0;
    if (leir_phase0_program_create(desc, &program) != -1 ||
        errno != expected_error || program != NULL) {
        leir_phase0_program_destroy(program);
        return 1;
    }
    return 0;
}

static int test_program_accepts_typed_connect_descriptor(void) {
    static const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    static const leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = 6U,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = 2U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
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
    leir_phase0_program_t *program = NULL;

    if (leir_phase0_program_create(&desc, &program) != 0 ||
        program == NULL) {
        return 1;
    }
    leir_phase0_program_destroy(program);
    return 0;
}

static int test_program_rejects_malformed_connect_descriptor(void) {
    leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_CONNECT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = 2U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
    };
    leir_phase0_program_desc_t desc = {
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = sizeof(nodes) / sizeof(nodes[0]),
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };

    slots[1] = LEIR_PHASE0_SLOT_MUT_BUFFER;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[1] = LEIR_PHASE0_SLOT_CONST_BUFFER;
    slots[0] = LEIR_PHASE0_SLOT_U64;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[0] = LEIR_PHASE0_SLOT_FD;
    slots[2] = LEIR_PHASE0_SLOT_I64;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[2] = LEIR_PHASE0_SLOT_U64;
    slots[3] = LEIR_PHASE0_SLOT_U64;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[3] = LEIR_PHASE0_SLOT_I64;
    nodes[0].on_success = LEIR_PHASE0_NODE_NONE;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 1U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[0].on_eof = LEIR_PHASE0_NODE_NONE;
    nodes[0].on_error = LEIR_PHASE0_NODE_NONE;
    return expect_program_error(&desc, EINVAL);
}

typedef struct portable_connect_state {
    const leir_phase0_program_t *program;
    llam_fd_t listener;
    llam_fd_t client;
    struct sockaddr_storage address;
    socklen_t address_length;
    int candidate_error;
    int accept_error;
    int64_t result;
    leir_phase0_metrics_t metrics;
} portable_connect_state_t;

static int make_portable_connect_listener(
    portable_connect_state_t *state) {
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);

    state->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (LLAM_FD_IS_INVALID(state->listener)) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(0U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(state->listener,
             (const struct sockaddr *)(const void *)&address,
             (socklen_t)sizeof(address)) != 0 ||
        listen(state->listener, 4) != 0 ||
        getsockname(state->listener,
                    (struct sockaddr *)(void *)&address,
                    &address_length) != 0) {
        return -1;
    }
    memset(&state->address, 0, sizeof(state->address));
    memcpy(&state->address, &address, sizeof(address));
    state->address_length = address_length;
    state->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    return LLAM_FD_IS_INVALID(state->client) ? -1 : 0;
}

static void portable_connect_candidate_task(void *arg) {
    portable_connect_state_t *state = arg;
    leir_phase0_instance_t instance;
    leir_phase0_value_t values[4] = {0};
    leir_phase0_value_t values_out[4] = {0};
    const leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = true,
    };

    values[0].fd = state->client;
    values[1].buffer.data = &state->address;
    values[1].buffer.size = (size_t)state->address_length;
    values[2].u64 = (uint64_t)state->address_length;
    values[3].i64 = -1;
    if (leir_phase0_instance_init(
            &instance, sizeof(instance), state->program) != 0 ||
        leir_phase0_instance_bind(
            &instance, values, 4U, &opts) != 0 ||
        leir_phase0_instance_run(
            &instance, values_out, 4U, &state->metrics) != 0) {
        state->candidate_error = errno != 0 ? errno : EIO;
        (void)llam_runtime_request_stop();
        return;
    }
    state->result = values_out[3].i64;
}

static void portable_connect_accept_task(void *arg) {
    portable_connect_state_t *state = arg;
    llam_fd_t accepted = llam_accept(state->listener, NULL, NULL);

    if (LLAM_FD_IS_INVALID(accepted)) {
        state->accept_error = errno != 0 ? errno : EIO;
        return;
    }
    leir_test_close(&accepted);
}

static int test_portable_connect_success_semantics(void) {
    static const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
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
            .on_error = 2U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 3U,
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
    portable_connect_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_task_t *candidate_task = NULL;
    llam_task_t *accept_task = NULL;
    leir_phase0_program_t *program = NULL;
    bool runtime_started = false;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.listener = LLAM_INVALID_FD;
    state.client = LLAM_INVALID_FD;
    state.result = -1;
    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    if (leir_phase0_program_create(&desc, &program) != 0) {
        fprintf(stderr, "portable connect program: errno=%d\n", errno);
        goto cleanup;
    }
    if (llam_runtime_init(&runtime_opts) != 0) {
        fprintf(stderr, "portable connect runtime: errno=%d\n", errno);
        goto cleanup;
    }
    runtime_started = true;
    state.program = program;
    if (make_portable_connect_listener(&state) != 0) {
        fprintf(stderr, "portable connect listener: errno=%d\n", errno);
        goto cleanup;
    }
    accept_task = llam_spawn(
        portable_connect_accept_task, &state, NULL);
    candidate_task = llam_spawn(
        portable_connect_candidate_task, &state, NULL);
    if (accept_task == NULL || candidate_task == NULL) {
        fprintf(stderr, "portable connect spawn: errno=%d\n", errno);
        (void)llam_runtime_request_stop();
        goto cleanup;
    }
    if (llam_run() != 0) {
        fprintf(stderr, "portable connect run: errno=%d\n", errno);
        goto cleanup;
    }
    if (llam_join(candidate_task) != 0 ||
        llam_join(accept_task) != 0) {
        fprintf(stderr, "portable connect join: errno=%d\n", errno);
        candidate_task = NULL;
        accept_task = NULL;
        goto cleanup;
    }
    candidate_task = NULL;
    accept_task = NULL;
    failed = state.candidate_error != 0 ||
             state.accept_error != 0 ||
             state.result != 0 ||
             state.metrics.effect_completions != 1U ||
             state.metrics.terminal_publications != 1U;
    if (failed) {
        fprintf(stderr,
                "portable connect result: candidate=%d accept=%d "
                "result=%lld completions=%llu publications=%llu\n",
                state.candidate_error,
                state.accept_error,
                (long long)state.result,
                (unsigned long long)state.metrics.effect_completions,
                (unsigned long long)state.metrics.terminal_publications);
    }

cleanup:
    if (runtime_started) {
        if (candidate_task != NULL || accept_task != NULL) {
            (void)llam_runtime_request_stop();
            (void)llam_run();
        }
        leir_test_close(&state.client);
        leir_test_close(&state.listener);
        llam_runtime_shutdown();
    } else {
        leir_test_close(&state.client);
        leir_test_close(&state.listener);
    }
    leir_phase0_program_destroy(program);
    return failed;
}

int main(void) {
    if (test_program_accepts_typed_connect_descriptor() != 0) {
        fputs("test_program_accepts_typed_connect_descriptor failed\n",
              stderr);
        return 1;
    }
    if (test_program_rejects_malformed_connect_descriptor() != 0) {
        fputs("test_program_rejects_malformed_connect_descriptor failed\n",
              stderr);
        return 1;
    }
    if (test_portable_connect_success_semantics() != 0) {
        fputs("test_portable_connect_success_semantics failed\n", stderr);
        return 1;
    }
    puts("LEIR portable CONNECT tests passed");
    return 0;
}
