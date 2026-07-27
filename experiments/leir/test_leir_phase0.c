#include "runtime_internal.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const leir_phase0_slot_kind_t valid_slots[] = {
    LEIR_PHASE0_SLOT_FD,
    LEIR_PHASE0_SLOT_MUT_BUFFER,
    LEIR_PHASE0_SLOT_U64,
    LEIR_PHASE0_SLOT_I64,
};

static const leir_phase0_node_desc_t valid_nodes[] = {
    {
        .opcode = LEIR_PHASE0_OP_READ_EXACT,
        .fd_slot = 0U,
        .buffer_slot = 1U,
        .length_slot = 2U,
        .result_slot = 3U,
        .on_success = 1U,
        .on_eof = 2U,
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

static leir_phase0_program_desc_t valid_program_desc(void) {
    leir_phase0_program_desc_t desc = {
        .nodes = valid_nodes,
        .slot_kinds = valid_slots,
        .node_count = sizeof(valid_nodes) / sizeof(valid_nodes[0]),
        .slot_count = sizeof(valid_slots) / sizeof(valid_slots[0]),
        .entry_node = 0U,
    };

    return desc;
}

static int expect_program_error(const leir_phase0_program_desc_t *desc,
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

static int test_program_copies_valid_descriptor(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_slot_kind_t slots[4];
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;
    int failed;

    memcpy(nodes, valid_nodes, sizeof(nodes));
    memcpy(slots, valid_slots, sizeof(slots));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    if (leir_phase0_program_create(&desc, &program) != 0 ||
        program == NULL) {
        return 1;
    }

    nodes[0].opcode = LEIR_PHASE0_OP_FAIL;
    slots[0] = LEIR_PHASE0_SLOT_I64;
    failed = program->nodes[0].opcode != LEIR_PHASE0_OP_READ_EXACT ||
             program->slot_kinds[0] != LEIR_PHASE0_SLOT_FD ||
             program->node_count != 3U ||
             program->slot_count != 4U ||
             program->io_node_count != 1U;
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_program_rejects_size_and_entry_bounds(void) {
    leir_phase0_program_desc_t desc = valid_program_desc();

    if (expect_program_error(NULL, EINVAL) != 0) {
        return 1;
    }
    errno = 0;
    if (leir_phase0_program_create(&desc, NULL) != -1 ||
        errno != EINVAL) {
        return 1;
    }
    desc.node_count = 0U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.node_count = LEIR_PHASE0_MAX_NODES + 1U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.slot_count = 0U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.slot_count = LEIR_PHASE0_MAX_SLOTS + 1U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.entry_node = 3U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.nodes = NULL;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    desc = valid_program_desc();
    desc.slot_kinds = NULL;
    return expect_program_error(&desc, EINVAL);
}

static int test_program_rejects_slot_type_mismatches(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_slot_kind_t slots[4];
    leir_phase0_program_desc_t desc = valid_program_desc();

    memcpy(nodes, valid_nodes, sizeof(nodes));
    memcpy(slots, valid_slots, sizeof(slots));
    desc.nodes = nodes;
    desc.slot_kinds = slots;

    slots[1] = LEIR_PHASE0_SLOT_CONST_BUFFER;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[1] = LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[3] = LEIR_PHASE0_SLOT_U64;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[3] = LEIR_PHASE0_SLOT_I64;
    slots[0] = LEIR_PHASE0_SLOT_U64;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[0] = (leir_phase0_slot_kind_t)99;
    return expect_program_error(&desc, EINVAL);
}

static int test_program_accepts_const_write_buffer(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_slot_kind_t slots[4];
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;

    memcpy(nodes, valid_nodes, sizeof(nodes));
    memcpy(slots, valid_slots, sizeof(slots));
    nodes[0].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    slots[1] = LEIR_PHASE0_SLOT_CONST_BUFFER;
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    if (leir_phase0_program_create(&desc, &program) != 0 ||
        program == NULL) {
        return 1;
    }
    leir_phase0_program_destroy(program);
    return 0;
}

static int test_program_rejects_invalid_opcode_and_terminal_shape(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_program_desc_t desc = valid_program_desc();

    memcpy(nodes, valid_nodes, sizeof(nodes));
    desc.nodes = nodes;

    nodes[0].opcode = 99U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[0] = valid_nodes[0];
    nodes[1].fd_slot = 0U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[1] = valid_nodes[1];
    nodes[1].result_slot = 2U;
    return expect_program_error(&desc, EINVAL);
}

static int test_program_rejects_invalid_edges(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_program_desc_t desc = valid_program_desc();

    memcpy(nodes, valid_nodes, sizeof(nodes));
    desc.nodes = nodes;

    nodes[0].on_success = 3U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[0].on_success = LEIR_PHASE0_NODE_NONE;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[0].on_success = 1U;
    nodes[1].on_success = 0U;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    nodes[1].on_success = LEIR_PHASE0_NODE_NONE;
    nodes[0].on_eof = 1U;
    nodes[0].on_error = 1U;
    return expect_program_error(&desc, EINVAL);
}

static int test_program_accepts_await_capable_cycle(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;

    memcpy(nodes, valid_nodes, sizeof(nodes));
    nodes[0].on_success = 0U;
    nodes[0].on_eof = 1U;
    desc.nodes = nodes;
    if (leir_phase0_program_create(&desc, &program) != 0 ||
        program == NULL) {
        return 1;
    }
    leir_phase0_program_destroy(program);
    return 0;
}

static void *fail_calloc(size_t count, size_t size) {
    (void)count;
    (void)size;
    return NULL;
}

static int test_program_allocation_failure_clears_output(void) {
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program =
        (leir_phase0_program_t *)(uintptr_t)1U;

    errno = 0;
    if (leir_phase0_program_create_with_allocator(
            &desc, &program, fail_calloc) != -1 ||
        errno != ENOMEM || program != NULL) {
        leir_phase0_program_destroy(program);
        return 1;
    }
    return 0;
}

static int test_instance_rejects_invalid_storage_and_bindings(void) {
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;
    leir_phase0_instance_t *instance;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = true,
    };
    leir_phase0_value_t values[4] = {0};
    unsigned char buffer[64];
    void *storage;
    size_t storage_size;
    int failed = 1;

    if (leir_phase0_program_create(&desc, &program) != 0) {
        return 1;
    }
    storage_size = leir_phase0_instance_size();
    if (storage_size == 0U) {
        goto cleanup_program;
    }
    storage = malloc(storage_size);
    if (storage == NULL) {
        goto cleanup_program;
    }

    errno = 0;
    if (leir_phase0_instance_init(NULL, storage_size, program) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    errno = 0;
    if (leir_phase0_instance_init(
            storage, storage_size - 1U, program) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    errno = 0;
    if (leir_phase0_instance_init(storage, storage_size, NULL) != -1 ||
        errno != EINVAL ||
        leir_phase0_instance_init(storage, storage_size, program) != 0) {
        goto cleanup_storage;
    }

    instance = storage;
    values[0].fd = (llam_fd_t)0;
    values[1].buffer.data = buffer;
    values[1].buffer.size = sizeof(buffer);
    values[2].u64 = sizeof(buffer);
    values[3].i64 = 0;

    errno = 0;
    if (leir_phase0_instance_bind(
            NULL, values, 4U, &opts) != -1 ||
        errno != EINVAL ||
        leir_phase0_instance_bind(
            instance, NULL, 4U, &opts) != -1 ||
        errno != EINVAL ||
        leir_phase0_instance_bind(
            instance, values, 3U, &opts) != -1 ||
        errno != EINVAL ||
        leir_phase0_instance_bind(
            instance, values, 4U, NULL) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }

    values[0].fd = LLAM_INVALID_FD;
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    values[0].fd = (llam_fd_t)0;
    values[1].buffer.data = NULL;
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    values[1].buffer.data = buffer;
    values[1].buffer.size = sizeof(buffer) - 1U;
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    values[1].buffer.size = sizeof(buffer);
    values[2].u64 = sizeof(buffer) + 1U;
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != -1 ||
        errno != EINVAL) {
        goto cleanup_storage;
    }
    values[2].u64 = sizeof(buffer);
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != 0) {
        goto cleanup_storage;
    }
    failed = 0;

cleanup_storage:
    free(storage);
cleanup_program:
    leir_phase0_program_destroy(program);
    return failed;
}

#define LEIR_PENDING_TEST_BYTES 64U

typedef struct pending_read_state {
    llam_fd_t pair[2];
    leir_phase0_program_t *program;
    unsigned char expected[LEIR_PENDING_TEST_BYTES];
    unsigned char received[LEIR_PENDING_TEST_BYTES];
    leir_phase0_metrics_t metrics;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} pending_read_state_t;

static void pending_read_fail(
    pending_read_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void pending_read_peer_task(void *arg) {
    pending_read_state_t *state = arg;

    if (llam_sleep_ns(UINT64_C(2) * 1000U * 1000U) != 0) {
        pending_read_fail(state, "peer sleep", errno);
        return;
    }
    if (leir_test_write_all(
            state->pair[1],
            state->expected,
            sizeof(state->expected)) != 0) {
        pending_read_fail(state, "peer write", errno);
    }
}

static void pending_read_instance_task(void *arg) {
    pending_read_state_t *state = arg;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = true,
    };
    leir_phase0_value_t values[4] = {0};
    leir_phase0_value_t values_out[4] = {0};
    leir_phase0_metrics_t metrics;
    leir_phase0_instance_t storage;
    leir_phase0_instance_t *instance = &storage;

    if (leir_phase0_instance_init(
            &storage, sizeof(storage), state->program) != 0) {
        pending_read_fail(state, "instance init", errno);
        return;
    }

    values[0].fd = state->pair[0];
    values[1].buffer.data = state->received;
    values[1].buffer.size = sizeof(state->received);
    values[2].u64 = sizeof(state->received);
    values[3].i64 = -1;
    if (leir_phase0_instance_bind(
            instance, values, 4U, &opts) != 0) {
        pending_read_fail(state, "instance bind", errno);
        return;
    }
    memset(&metrics, 0, sizeof(metrics));
    if (leir_phase0_instance_run(
            instance, values_out, 4U, &metrics) != 0) {
        pending_read_fail(state, "instance run", errno);
        return;
    }

    state->metrics = metrics;
    if (values_out[3].i64 != (int64_t)sizeof(state->received) ||
        memcmp(
            state->received,
            state->expected,
            sizeof(state->received)) != 0 ||
        metrics.activations != 1U ||
        metrics.effect_completions != 1U ||
        metrics.backend_submits != 1U ||
        metrics.direct_completions != 0U ||
        metrics.task_parks != 1U ||
        metrics.terminal_publications != 1U ||
        metrics.task_resumes_avoided != 0U ||
        metrics.heap_requests != 0U ||
        metrics.hot_allocations != 0U) {
        pending_read_fail(state, "instance result or metrics", EPROTO);
    }
}

static int test_pending_read_terminal_uses_one_park(void) {
    pending_read_state_t state;
    leir_phase0_program_desc_t desc = valid_program_desc();
    llam_runtime_opts_t opts;
    llam_task_t *peer = NULL;
    llam_task_t *instance_task = NULL;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.pair[0] = LLAM_INVALID_FD;
    state.pair[1] = LLAM_INVALID_FD;
    atomic_init(&state.failures, 0U);
    leir_test_fill_pattern(
        state.expected, sizeof(state.expected), UINT64_C(0x4c454952));
    if (leir_phase0_program_create(&desc, &state.program) != 0 ||
        leir_test_socketpair(state.pair) != 0) {
        goto cleanup;
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        goto cleanup;
    }
    peer = llam_spawn(pending_read_peer_task, &state, NULL);
    if (peer != NULL) {
        instance_task =
            llam_spawn(pending_read_instance_task, &state, NULL);
    }
    if (peer == NULL || instance_task == NULL ||
        llam_run() != 0) {
        goto shutdown;
    }
    if (llam_join(peer) != 0 || llam_join(instance_task) != 0) {
        peer = NULL;
        instance_task = NULL;
        goto shutdown;
    }
    peer = NULL;
    instance_task = NULL;
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "pending read failed at %s: errno=%d\n",
            state.first_case,
            state.first_errno);
        goto shutdown;
    }
    failed = 0;

shutdown:
    llam_runtime_shutdown();
cleanup:
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

static bool test_sink(llam_node_t *node,
                      llam_io_req_t *req,
                      unsigned completion_owner,
                      llam_wait_reason_t *wake_reason,
                      void *context) {
    unsigned *calls = context;

    (void)node;
    if (req == NULL || completion_owner != 7U ||
        wake_reason == NULL || *wake_reason != LLAM_WAIT_IO) {
        return false;
    }
    *calls += 1U;
    return true;
}

static int test_completion_sink_dispatch(void) {
    llam_io_req_t req;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned calls = 0U;

    memset(&req, 0, sizeof(req));
    req.completion_sink = test_sink;
    req.completion_sink_context = &calls;
    if (!llam_io_dispatch_completion_sink(
            NULL, &req, 7U, &wake_reason) ||
        calls != 1U) {
        return 1;
    }
    req.completion_sink = NULL;
    return llam_io_dispatch_completion_sink(
               NULL, &req, 7U, &wake_reason)
               ? 1
               : 0;
}

int main(void) {
    if (test_completion_sink_dispatch() != 0) {
        fputs("test_completion_sink_dispatch failed\n", stderr);
        return 1;
    }
    if (test_program_copies_valid_descriptor() != 0) {
        fputs("test_program_copies_valid_descriptor failed\n", stderr);
        return 1;
    }
    if (test_program_rejects_size_and_entry_bounds() != 0) {
        fputs("test_program_rejects_size_and_entry_bounds failed\n", stderr);
        return 1;
    }
    if (test_program_rejects_slot_type_mismatches() != 0) {
        fputs("test_program_rejects_slot_type_mismatches failed\n", stderr);
        return 1;
    }
    if (test_program_accepts_const_write_buffer() != 0) {
        fputs("test_program_accepts_const_write_buffer failed\n", stderr);
        return 1;
    }
    if (test_program_rejects_invalid_opcode_and_terminal_shape() != 0) {
        fputs("test_program_rejects_invalid_opcode_and_terminal_shape "
              "failed\n",
              stderr);
        return 1;
    }
    if (test_program_rejects_invalid_edges() != 0) {
        fputs("test_program_rejects_invalid_edges failed\n", stderr);
        return 1;
    }
    if (test_program_accepts_await_capable_cycle() != 0) {
        fputs("test_program_accepts_await_capable_cycle failed\n", stderr);
        return 1;
    }
    if (test_program_allocation_failure_clears_output() != 0) {
        fputs("test_program_allocation_failure_clears_output failed\n",
              stderr);
        return 1;
    }
    if (test_instance_rejects_invalid_storage_and_bindings() != 0) {
        fputs("test_instance_rejects_invalid_storage_and_bindings failed\n",
              stderr);
        return 1;
    }
    if (test_pending_read_terminal_uses_one_park() != 0) {
        fputs("test_pending_read_terminal_uses_one_park failed\n", stderr);
        return 1;
    }
    puts("LEIR Phase 0 tests passed");
    return 0;
}
