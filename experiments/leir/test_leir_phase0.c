#include "runtime_internal.h"
#include "io/runtime_io_api_internal.h"
#include "leir_peer_process.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LEIR_TEST_THREAD_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define LEIR_TEST_THREAD_SANITIZER 1
#endif
#ifndef LEIR_TEST_THREAD_SANITIZER
#define LEIR_TEST_THREAD_SANITIZER 0
#endif

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

static int test_benchmark_option_parser(void) {
    char *argv[] = {
        "bench_leir_phase0",
        "--workload",
        "socket_relay",
        "--nodes",
        "4",
        "--concurrency",
        "64",
        "--payload",
        "1024",
        "--inline-budget",
        "8",
        "--activations",
        "128",
        "--min-mode-ms",
        "20",
        "--order",
        "ABBA",
    };
    static const char *const workloads[] = {
        "socket_relay",
        "framed_rpc",
        "graph_break",
    };
    static const char *const nodes[] = {
        "1", "2", "4", "8",
    };
    static const char *const orders[] = {
        "ABBA", "BAAB",
    };
    leir_bench_options_t options;
    size_t i;

    for (i = 0U;
         i < sizeof(workloads) / sizeof(workloads[0]);
         i += 1U) {
        argv[2] = (char *)workloads[i];
        if (leir_bench_parse_options(
                (int)(sizeof(argv) / sizeof(argv[0])),
                argv,
                &options) != 0 ||
            strcmp(
                leir_bench_workload_name(options.workload),
                workloads[i]) != 0) {
            return 1;
        }
    }
    argv[2] = "socket_relay";
    for (i = 0U; i < sizeof(nodes) / sizeof(nodes[0]); i += 1U) {
        argv[4] = (char *)nodes[i];
        if (leir_bench_parse_options(
                (int)(sizeof(argv) / sizeof(argv[0])),
                argv,
                &options) != 0 ||
            options.nodes != (unsigned)(1U << i)) {
            return 1;
        }
    }
    argv[4] = "4";
    for (i = 0U; i < sizeof(orders) / sizeof(orders[0]); i += 1U) {
        argv[16] = (char *)orders[i];
        if (leir_bench_parse_options(
                (int)(sizeof(argv) / sizeof(argv[0])),
                argv,
                &options) != 0 ||
            strcmp(
                leir_bench_order_name(options.order),
                orders[i]) != 0) {
            return 1;
        }
    }
    argv[16] = "ABBA";
    argv[6] = "1";
    argv[8] = "64";
    argv[10] = "1";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != 0 ||
        options.concurrency != 1U ||
        options.payload != 64U ||
        options.inline_budget != 1U ||
        options.activations != 128U ||
        options.min_mode_ns != UINT64_C(20000000)) {
        return 1;
    }
    argv[6] = "512";
    argv[8] = "16384";
    argv[10] = "32";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != 0 ||
        options.concurrency != 512U ||
        options.payload != 16384U ||
        options.inline_budget != 32U) {
        return 1;
    }

    if (leir_bench_parse_options(15, argv, &options) != EINVAL) {
        return 1;
    }
    argv[15] = "--nodes";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[15] = "--order";
    argv[4] = "3";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[4] = "4";
    argv[6] = "0";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[6] = "64";
    argv[8] = "0";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[8] = "64";
    argv[10] = "0";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[10] = "8";
    argv[12] = "0";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL) {
        return 1;
    }
    argv[12] = "128";
    argv[14] = "0";
    if (leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            &options) != EINVAL ||
        leir_bench_parse_options(
            (int)(sizeof(argv) / sizeof(argv[0])),
            argv,
            NULL) != EINVAL) {
        return 1;
    }
    return 0;
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
    slots[0] = LEIR_PHASE0_SLOT_FD;
    slots[2] = LEIR_PHASE0_SLOT_CONST_BUFFER;
    if (expect_program_error(&desc, EINVAL) != 0) {
        return 1;
    }
    slots[2] = LEIR_PHASE0_SLOT_U64;
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

static int test_program_accepts_result_length_dataflow(void) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;
    leir_phase0_instance_t instance;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
    };
    leir_phase0_value_t values[4] = {0};
    unsigned char buffer[64];
    int failed;

    memcpy(nodes, valid_nodes, sizeof(nodes));
    nodes[1] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_WRITE_ALL,
        .fd_slot = 0U,
        .buffer_slot = 1U,
        .length_slot = 3U,
        .result_slot = 3U,
        .on_success = 2U,
        .on_eof = 2U,
        .on_error = 2U,
    };
    desc.nodes = nodes;
    if (leir_phase0_program_create(&desc, &program) != 0 ||
        program == NULL) {
        return 1;
    }
    values[0].fd = (llam_fd_t)0;
    values[1].buffer.data = buffer;
    values[1].buffer.size = sizeof(buffer);
    values[2].u64 = sizeof(buffer);
    values[3].i64 = -1;
    failed = leir_phase0_instance_init(
                 &instance, sizeof(instance), program) != 0 ||
             leir_phase0_instance_bind(
                 &instance, values, 4U, &opts) != 0;
    leir_phase0_program_destroy(program);
    return failed;
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

static int test_instance_cancel_before_run_publishes_request(void) {
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;
    leir_phase0_instance_t instance;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
    };
    leir_phase0_value_t values[4] = {0};
    unsigned char buffer[64];
    int failed = 1;

    if (leir_phase0_program_create(&desc, &program) != 0 ||
        leir_phase0_instance_init(
            &instance, sizeof(instance), program) != 0) {
        goto cleanup;
    }
    values[0].fd = (llam_fd_t)0;
    values[1].buffer.data = buffer;
    values[1].buffer.size = sizeof(buffer);
    values[2].u64 = sizeof(buffer);
    values[3].i64 = -1;
    if (leir_phase0_instance_bind(
            &instance, values, 4U, &opts) != 0 ||
        leir_phase0_instance_cancel(&instance) != 0 ||
        atomic_load_explicit(
            &instance.cancel_requested, memory_order_acquire) == 0U) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    leir_phase0_program_destroy(program);
    return failed;
}

static int bind_injection_instance(
    leir_phase0_instance_t *instance,
    unsigned char *buffer,
    size_t buffer_size,
    int64_t initial_result) {
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
    };
    leir_phase0_value_t values[4] = {0};

    values[0].fd = (llam_fd_t)0;
    values[1].buffer.data = buffer;
    values[1].buffer.size = buffer_size;
    values[2].u64 = buffer_size;
    values[3].i64 = initial_result;
    return leir_phase0_instance_bind(
        instance, values, 4U, &opts);
}

static int test_stale_and_duplicate_completion_injection(void) {
    static const leir_phase0_node_desc_t intermediate_nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_READ_EXACT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_READ_EXACT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 2U,
            .on_eof = 3U,
            .on_error = 3U,
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
    leir_phase0_program_desc_t terminal_desc = valid_program_desc();
    leir_phase0_program_desc_t intermediate_desc = {
        .nodes = intermediate_nodes,
        .slot_kinds = valid_slots,
        .node_count =
            sizeof(intermediate_nodes) / sizeof(intermediate_nodes[0]),
        .slot_count = sizeof(valid_slots) / sizeof(valid_slots[0]),
        .entry_node = 0U,
    };
    leir_phase0_program_t *terminal_program = NULL;
    leir_phase0_program_t *intermediate_program = NULL;
    leir_phase0_instance_t terminal_instance;
    leir_phase0_instance_t intermediate_instance;
    unsigned char buffer[8] = {0};
    uint64_t old_activation;
    uint64_t current_activation;
    bool terminal_initialized = false;
    bool intermediate_initialized = false;
    int failed = 1;

    if (leir_phase0_program_create(
            &terminal_desc, &terminal_program) != 0 ||
        leir_phase0_program_create(
            &intermediate_desc, &intermediate_program) != 0) {
        goto cleanup;
    }
    if (leir_phase0_instance_init(
            &terminal_instance,
            sizeof(terminal_instance),
            terminal_program) != 0) {
        goto cleanup;
    }
    terminal_initialized = true;
    if (leir_phase0_instance_init(
            &intermediate_instance,
            sizeof(intermediate_instance),
            intermediate_program) != 0) {
        goto cleanup;
    }
    intermediate_initialized = true;
    if (bind_injection_instance(
            &terminal_instance, buffer, sizeof(buffer), -71) != 0) {
        goto cleanup;
    }

    atomic_store_explicit(
        &terminal_instance.running, 1U, memory_order_release);
    old_activation = (uint64_t)atomic_load_explicit(
        &terminal_instance.activation_generation,
        memory_order_acquire);
    if (!leir_phase0_test_inject_completion(
            &terminal_instance,
            old_activation,
            (ssize_t)sizeof(buffer),
            0) ||
        atomic_load_explicit(
            &terminal_instance.terminal, memory_order_acquire) == 0U ||
        terminal_instance.metrics.terminal_publications != 1U ||
        terminal_instance.slots[3].i64 != (int64_t)sizeof(buffer) ||
        leir_phase0_test_inject_completion(
            &terminal_instance,
            old_activation,
            (ssize_t)sizeof(buffer),
            0) ||
        terminal_instance.metrics.terminal_publications != 1U) {
        goto cleanup;
    }
    atomic_store_explicit(
        &terminal_instance.running, 0U, memory_order_release);

    if (bind_injection_instance(
            &terminal_instance, buffer, sizeof(buffer), -73) != 0) {
        goto cleanup;
    }
    current_activation = (uint64_t)atomic_load_explicit(
        &terminal_instance.activation_generation,
        memory_order_acquire);
    atomic_store_explicit(
        &terminal_instance.running, 1U, memory_order_release);
    if (current_activation == old_activation ||
        leir_phase0_test_inject_completion(
            &terminal_instance,
            old_activation,
            (ssize_t)sizeof(buffer),
            0) ||
        terminal_instance.slots[3].i64 != -73 ||
        terminal_instance.metrics.stale_completions != 1U ||
        terminal_instance.metrics.terminal_publications != 1U ||
        terminal_instance.terminal_error != EPROTO) {
        goto cleanup;
    }
    atomic_store_explicit(
        &terminal_instance.running, 0U, memory_order_release);

    if (bind_injection_instance(
            &intermediate_instance, buffer, sizeof(buffer), -79) != 0) {
        goto cleanup;
    }
    current_activation = (uint64_t)atomic_load_explicit(
        &intermediate_instance.activation_generation,
        memory_order_acquire);
    atomic_store_explicit(
        &intermediate_instance.running, 1U, memory_order_release);
    if (!leir_phase0_test_inject_completion(
            &intermediate_instance,
            current_activation,
            (ssize_t)sizeof(buffer),
            0) ||
        intermediate_instance.current_node != 1U ||
        intermediate_instance.metrics.terminal_publications != 0U ||
        leir_phase0_test_inject_completion(
            &intermediate_instance,
            current_activation,
            (ssize_t)sizeof(buffer),
            0) ||
        intermediate_instance.current_node != 1U ||
        intermediate_instance.metrics.backend_submits != 0U ||
        intermediate_instance.metrics.stale_completions != 1U) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (terminal_initialized) {
        atomic_store_explicit(
            &terminal_instance.running, 0U, memory_order_release);
    }
    if (intermediate_initialized) {
        atomic_store_explicit(
            &intermediate_instance.running, 0U, memory_order_release);
    }
    leir_phase0_program_destroy(intermediate_program);
    leir_phase0_program_destroy(terminal_program);
    return failed;
}

#define LEIR_CANCEL_BYTES 64U

typedef struct cancellation_state {
    leir_phase0_instance_t instance;
    leir_phase0_value_t values_out[4];
    leir_phase0_metrics_t metrics;
    unsigned char expected[LEIR_CANCEL_BYTES];
    unsigned char received[LEIR_CANCEL_BYTES];
    atomic_uint runner_done;
    atomic_uint canceller_ready;
    atomic_uint observed_mode;
    atomic_uint cancel_called;
    bool wait_for_mode;
    bool wait_for_inflight;
    unsigned delay_spins;
    int run_result;
    int run_errno;
    int cancel_result;
    int cancel_errno;
} cancellation_state_t;

static void cancellation_instance_task(void *arg) {
    cancellation_state_t *state = arg;

    errno = 0;
    state->run_result = leir_phase0_instance_run(
        &state->instance,
        state->values_out,
        4U,
        &state->metrics);
    state->run_errno = errno;
    atomic_store_explicit(
        &state->runner_done, 1U, memory_order_release);
}

static unsigned cancellation_observe_wait_mode(
    cancellation_state_t *state,
    bool *request_published_out) {
    llam_io_req_t *req;
    llam_task_t *task;
    unsigned mode = LLAM_IO_WAIT_MODE_NONE;

    (void)atomic_fetch_add_explicit(
        &state->instance.cancel_readers,
        1U,
        memory_order_acq_rel);
    task = atomic_load_explicit(
        &state->instance.task, memory_order_acquire);
    req = task != NULL
        ? atomic_load_explicit(
              &state->instance.req, memory_order_acquire)
        : NULL;
    if (task != NULL && req != NULL) {
        mode = atomic_load_explicit(
            &req->wait_mode, memory_order_acquire);
    }
    (void)atomic_fetch_sub_explicit(
        &state->instance.cancel_readers,
        1U,
        memory_order_acq_rel);
    *request_published_out = req != NULL;
    return mode;
}

static void *cancellation_thread(void *arg) {
    cancellation_state_t *state = arg;
    unsigned mode = LLAM_IO_WAIT_MODE_NONE;
    unsigned i;

    atomic_store_explicit(
        &state->canceller_ready, 1U, memory_order_release);
    for (;;) {
        bool request_published = false;

        mode = cancellation_observe_wait_mode(
            state, &request_published);
        if (request_published) {
            if (!state->wait_for_mode ||
                (state->wait_for_inflight
                     ? mode == LLAM_IO_WAIT_MODE_INFLIGHT
                     : mode != LLAM_IO_WAIT_MODE_NONE)) {
                break;
            }
        }
        if (atomic_load_explicit(
                &state->runner_done, memory_order_acquire) != 0U) {
            break;
        }
    }
    atomic_store_explicit(
        &state->observed_mode, mode, memory_order_release);
    for (i = 0U; i < state->delay_spins; i += 1U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    errno = 0;
    state->cancel_result =
        leir_phase0_instance_cancel(&state->instance);
    state->cancel_errno = errno;
    atomic_store_explicit(
        &state->cancel_called, 1U, memory_order_release);
    return NULL;
}

static bool runtime_pending_ops_are_zero(void) {
    unsigned i;

    for (i = 0U; i < g_llam_runtime.active_nodes; i += 1U) {
        if (atomic_load_explicit(
                &g_llam_runtime.nodes[i].pending_ops,
                memory_order_acquire) != 0U) {
            return false;
        }
    }
    return true;
}

static int bind_cancellation_instance(
    cancellation_state_t *state,
    llam_fd_t fd) {
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = true,
    };
    leir_phase0_value_t values[4] = {0};

    memset(state->received, 0, sizeof(state->received));
    memset(state->values_out, 0, sizeof(state->values_out));
    memset(&state->metrics, 0, sizeof(state->metrics));
    state->run_result = -2;
    state->run_errno = 0;
    state->cancel_result = -2;
    state->cancel_errno = 0;
    atomic_store_explicit(
        &state->runner_done, 0U, memory_order_release);
    atomic_store_explicit(
        &state->canceller_ready, 0U, memory_order_release);
    atomic_store_explicit(
        &state->observed_mode,
        LLAM_IO_WAIT_MODE_NONE,
        memory_order_release);
    atomic_store_explicit(
        &state->cancel_called, 0U, memory_order_release);

    values[0].fd = fd;
    values[1].buffer.data = state->received;
    values[1].buffer.size = sizeof(state->received);
    values[2].u64 = sizeof(state->received);
    values[3].i64 = 17;
    return leir_phase0_instance_bind(
        &state->instance, values, 4U, &opts);
}

static int run_cancellation_activation(
    cancellation_state_t *state,
    llam_fd_t pair[2],
    bool preload_success,
    bool use_canceller,
    bool wait_for_mode,
    bool wait_for_inflight,
    unsigned delay_spins) {
    pthread_t canceller;
    llam_task_t *runner = NULL;
    bool canceller_started = false;
    int failed = 1;

    state->wait_for_mode = wait_for_mode;
    state->wait_for_inflight = wait_for_inflight;
    state->delay_spins = delay_spins;
    if (bind_cancellation_instance(state, pair[0]) != 0) {
        return 1;
    }
    if (preload_success &&
        leir_test_write_all(
            pair[1], state->expected, sizeof(state->expected)) != 0) {
        return 1;
    }
    if (use_canceller) {
        if (pthread_create(
                &canceller, NULL, cancellation_thread, state) != 0) {
            return 1;
        }
        canceller_started = true;
        while (atomic_load_explicit(
                   &state->canceller_ready,
                   memory_order_acquire) == 0U) {
            atomic_signal_fence(memory_order_seq_cst);
        }
    }

    runner = llam_spawn(cancellation_instance_task, state, NULL);
    if (runner == NULL || llam_run() != 0 ||
        llam_join(runner) != 0) {
        runner = NULL;
        goto cleanup;
    }
    runner = NULL;
    if (canceller_started) {
        if (pthread_join(canceller, NULL) != 0) {
            canceller_started = false;
            goto cleanup;
        }
        canceller_started = false;
    }
    if (atomic_load_explicit(
            &state->instance.running, memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &state->instance.cancel_readers, memory_order_acquire) != 0U ||
        state->metrics.terminal_publications != 1U ||
        !runtime_pending_ops_are_zero()) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (runner != NULL) {
        (void)llam_runtime_request_stop();
        (void)llam_run();
        (void)llam_join(runner);
    }
    if (canceller_started) {
        (void)leir_phase0_instance_cancel(&state->instance);
        (void)pthread_join(canceller, NULL);
    }
    return failed;
}

static size_t cancellation_race_iterations(void) {
    const char *value = getenv("LLAM_LEIR_RACE_ITERS");
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || *value == '\0') {
        return 100U;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed == 0UL || parsed > 10000UL) {
        return 100U;
    }
    return (size_t)parsed;
}

static int test_pre_submit_inflight_and_racing_cancellation(void) {
    cancellation_state_t state;
    leir_phase0_program_desc_t desc = valid_program_desc();
    leir_phase0_program_t *program = NULL;
    llam_runtime_opts_t runtime_opts;
    llam_fd_t pair[2] = {
        LLAM_INVALID_FD,
        LLAM_INVALID_FD,
    };
    bool runtime_started = false;
    bool saw_submit_queue = false;
    bool saw_inflight = false;
    size_t race_iterations;
    size_t i;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.runner_done, 0U);
    atomic_init(&state.canceller_ready, 0U);
    atomic_init(
        &state.observed_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&state.cancel_called, 0U);
    leir_test_fill_pattern(
        state.expected,
        sizeof(state.expected),
        UINT64_C(0x4c45495243414e43));
    if (leir_phase0_program_create(&desc, &program) != 0 ||
        leir_phase0_instance_init(
            &state.instance,
            sizeof(state.instance),
            program) != 0) {
        goto cleanup;
    }

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 1U;
    runtime_opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&runtime_opts) != 0) {
        goto cleanup;
    }
    runtime_started = true;

    if (leir_test_socketpair(pair) != 0 ||
        bind_cancellation_instance(&state, pair[0]) != 0 ||
        leir_phase0_instance_cancel(&state.instance) != 0) {
        goto cleanup;
    }
    {
        llam_task_t *runner =
            llam_spawn(cancellation_instance_task, &state, NULL);

        if (runner == NULL || llam_run() != 0 ||
            llam_join(runner) != 0 ||
            state.run_result != -1 ||
            state.run_errno != ECANCELED ||
            state.values_out[3].i64 != -1 ||
            state.metrics.backend_submits != 0U ||
            state.metrics.task_parks != 0U ||
            state.metrics.effect_completions != 0U ||
            state.metrics.terminal_publications != 1U ||
            !runtime_pending_ops_are_zero()) {
            goto cleanup;
        }
    }
    leir_test_close(&pair[0]);
    leir_test_close(&pair[1]);

    for (i = 0U; i < 64U &&
                 (!saw_submit_queue || !saw_inflight);
         i += 1U) {
        unsigned observed;

        if (leir_test_socketpair(pair) != 0 ||
            run_cancellation_activation(
                &state,
                pair,
                false,
                true,
                true,
                saw_submit_queue,
                0U) != 0 ||
            state.run_result != -1 ||
            state.run_errno != ECANCELED ||
            state.values_out[3].i64 != -1 ||
            state.metrics.backend_submits != 1U ||
            state.metrics.task_parks != 1U ||
            state.cancel_result != 0 ||
            atomic_load_explicit(
                &state.cancel_called, memory_order_acquire) == 0U) {
            goto cleanup;
        }
        observed = atomic_load_explicit(
            &state.observed_mode, memory_order_acquire);
        saw_submit_queue =
            saw_submit_queue ||
            observed == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE;
        saw_inflight =
            saw_inflight ||
            observed == LLAM_IO_WAIT_MODE_INFLIGHT;
        leir_test_close(&pair[0]);
        leir_test_close(&pair[1]);
    }
    if (!saw_submit_queue || !saw_inflight) {
        fprintf(
            stderr,
            "cancellation modes missing: submit=%u inflight=%u\n",
            saw_submit_queue ? 1U : 0U,
            saw_inflight ? 1U : 0U);
        goto cleanup;
    }

    race_iterations = cancellation_race_iterations();
    for (i = 0U; i < race_iterations; i += 1U) {
        if (leir_test_socketpair(pair) != 0 ||
            run_cancellation_activation(
                &state,
                pair,
                true,
                true,
                (i & 1U) != 0U,
                false,
                (unsigned)(i & 31U) * 32U) != 0) {
            goto cleanup;
        }
        if (state.run_result == 0) {
            if (state.run_errno != 0 ||
                state.values_out[3].i64 !=
                    (int64_t)sizeof(state.received) ||
                memcmp(
                    state.received,
                    state.expected,
                    sizeof(state.received)) != 0) {
                goto cleanup;
            }
        } else if (state.run_result != -1 ||
                   state.run_errno != ECANCELED) {
            goto cleanup;
        }
        leir_test_close(&pair[0]);
        leir_test_close(&pair[1]);
    }
    failed = 0;

cleanup:
    leir_test_close(&pair[0]);
    leir_test_close(&pair[1]);
    if (runtime_started) {
        llam_runtime_shutdown();
    }
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

typedef struct embedded_reuse_state {
    llam_fd_t pair[2];
    leir_phase0_program_t *program;
    llam_io_req_t *held_request;
    unsigned char expected[LEIR_PENDING_TEST_BYTES];
    unsigned char received[LEIR_PENDING_TEST_BYTES];
    leir_phase0_metrics_t metrics;
    atomic_uint request_held;
    atomic_uint request_released;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} embedded_reuse_state_t;

static void embedded_reuse_fail(
    embedded_reuse_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void embedded_reuse_release_task(void *arg) {
    embedded_reuse_state_t *state = arg;

    while (atomic_load_explicit(
               &state->request_held, memory_order_acquire) == 0U) {
        llam_yield();
    }
    atomic_store_explicit(
        &state->held_request->backend_event_refs,
        0U,
        memory_order_release);
    atomic_store_explicit(
        &state->request_released, 1U, memory_order_release);
}

static void embedded_reuse_peer_task(void *arg) {
    embedded_reuse_state_t *state = arg;

    if (llam_sleep_ns(UINT64_C(2) * 1000U * 1000U) != 0 ||
        leir_test_write_all(
            state->pair[1],
            state->expected,
            sizeof(state->expected)) != 0) {
        embedded_reuse_fail(state, "embedded reuse peer", errno);
    }
}

static void embedded_reuse_instance_task(void *arg) {
    embedded_reuse_state_t *state = arg;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = true,
    };
    leir_phase0_value_t values[4] = {0};
    leir_phase0_value_t values_out[4] = {0};
    leir_phase0_instance_t instance;

    if (leir_phase0_instance_init(
            &instance, sizeof(instance), state->program) != 0) {
        embedded_reuse_fail(state, "embedded reuse init", errno);
        return;
    }
    values[0].fd = state->pair[0];
    values[1].buffer.data = state->received;
    values[1].buffer.size = sizeof(state->received);
    values[2].u64 = sizeof(state->received);
    values[3].i64 = -1;
    if (leir_phase0_instance_bind(
            &instance, values, 4U, &opts) != 0) {
        embedded_reuse_fail(state, "embedded reuse bind", errno);
        return;
    }

    /*
     * Model the real terminal-completion race: the task is runnable while
     * the backend worker still owns its event-batch pin. A LEIR activation
     * must wait for the embedded request instead of entering the allocator.
     */
    state->held_request = &g_llam_tls_task->embedded_io_req;
    atomic_store_explicit(
        &state->held_request->backend_event_refs,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &state->request_held, 1U, memory_order_release);

    memset(&state->metrics, 0, sizeof(state->metrics));
    if (leir_phase0_instance_run(
            &instance, values_out, 4U, &state->metrics) != 0) {
        embedded_reuse_fail(state, "embedded reuse run", errno);
        return;
    }
    if (atomic_load_explicit(
            &state->request_released, memory_order_acquire) == 0U ||
        values_out[3].i64 != (int64_t)sizeof(state->received) ||
        memcmp(
            state->received,
            state->expected,
            sizeof(state->received)) != 0 ||
        state->metrics.heap_requests != 0U ||
        state->metrics.hot_allocations != 0U) {
        embedded_reuse_fail(
            state, "embedded reuse result or allocation", EPROTO);
    }
}

static int test_embedded_request_reuse_avoids_heap_fallback(void) {
    embedded_reuse_state_t state;
    leir_phase0_program_desc_t desc = valid_program_desc();
    llam_runtime_opts_t opts;
    llam_task_t *instance_task = NULL;
    llam_task_t *release_task = NULL;
    llam_task_t *peer_task = NULL;
    bool runtime_started = false;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.pair[0] = LLAM_INVALID_FD;
    state.pair[1] = LLAM_INVALID_FD;
    atomic_init(&state.request_held, 0U);
    atomic_init(&state.request_released, 0U);
    atomic_init(&state.failures, 0U);
    leir_test_fill_pattern(
        state.expected,
        sizeof(state.expected),
        UINT64_C(0x4c45495252455553));
    if (leir_phase0_program_create(&desc, &state.program) != 0 ||
        leir_test_socketpair(state.pair) != 0) {
        goto cleanup;
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    instance_task =
        llam_spawn(embedded_reuse_instance_task, &state, NULL);
    release_task =
        llam_spawn(embedded_reuse_release_task, &state, NULL);
    peer_task =
        llam_spawn(embedded_reuse_peer_task, &state, NULL);
    if (instance_task == NULL || release_task == NULL ||
        peer_task == NULL || llam_run() != 0 ||
        llam_join(instance_task) != 0 ||
        llam_join(release_task) != 0 ||
        llam_join(peer_task) != 0) {
        goto cleanup;
    }
    instance_task = NULL;
    release_task = NULL;
    peer_task = NULL;
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "embedded reuse failed at %s: errno=%d heap=%llu hot=%llu\n",
            state.first_case,
            state.first_errno,
            (unsigned long long)state.metrics.heap_requests,
            (unsigned long long)state.metrics.hot_allocations);
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (state.held_request != NULL &&
        atomic_load_explicit(
            &state.request_released, memory_order_acquire) == 0U) {
        atomic_store_explicit(
            &state.held_request->backend_event_refs,
            0U,
            memory_order_release);
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

#define LEIR_DIFF_BYTES 64U
#define LEIR_DIFF_MAX_ROUNDS 4U
#define LEIR_DIFF_MODES 2U

typedef struct differential_mode {
    llam_fd_t pair[2];
    unsigned char buffer[LEIR_DIFF_BYTES];
    unsigned char responses[LEIR_DIFF_MAX_ROUNDS][LEIR_DIFF_BYTES];
} differential_mode_t;

typedef struct differential_state {
    differential_mode_t modes[LEIR_DIFF_MODES];
    unsigned char expected[LEIR_DIFF_MAX_ROUNDS][LEIR_DIFF_BYTES];
    leir_phase0_program_t *program;
    leir_phase0_metrics_t candidate_metrics;
    unsigned rounds;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} differential_state_t;

typedef struct differential_task_arg {
    differential_state_t *state;
    unsigned mode;
} differential_task_arg_t;

static void differential_fail(
    differential_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void differential_peer_task(void *arg) {
    differential_task_arg_t *task_arg = arg;
    differential_state_t *state = task_arg->state;
    differential_mode_t *mode = &state->modes[task_arg->mode];
    unsigned round;

    for (round = 0U; round < state->rounds; round += 1U) {
        if (llam_sleep_ns(UINT64_C(500) * 1000U) != 0) {
            differential_fail(state, "differential peer sleep", errno);
            return;
        }
        if (leir_test_write_all(
                mode->pair[1],
                state->expected[round],
                LEIR_DIFF_BYTES) != 0 ||
            leir_test_read_exact(
                mode->pair[1],
                mode->responses[round],
                LEIR_DIFF_BYTES) != 0) {
            differential_fail(state, "differential peer I/O", errno);
            return;
        }
    }
}

static void differential_baseline_task(void *arg) {
    differential_task_arg_t *task_arg = arg;
    differential_state_t *state = task_arg->state;
    differential_mode_t *mode = &state->modes[task_arg->mode];
    unsigned round;

    for (round = 0U; round < state->rounds; round += 1U) {
        if (leir_test_read_exact(
                mode->pair[0], mode->buffer, LEIR_DIFF_BYTES) != 0 ||
            leir_test_write_all(
                mode->pair[0], mode->buffer, LEIR_DIFF_BYTES) != 0) {
            differential_fail(
                state, "differential baseline I/O", errno);
            leir_test_close(&mode->pair[0]);
            return;
        }
    }
}

static void differential_candidate_task(void *arg) {
    differential_task_arg_t *task_arg = arg;
    differential_state_t *state = task_arg->state;
    differential_mode_t *mode = &state->modes[task_arg->mode];
    leir_phase0_instance_t storage;
    leir_phase0_value_t values[5] = {0};
    leir_phase0_value_t values_out[5] = {0};
    leir_phase0_metrics_t metrics;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = false,
    };

    if (leir_phase0_instance_init(
            &storage, sizeof(storage), state->program) != 0) {
        differential_fail(state, "differential instance init", errno);
        return;
    }
    values[0].fd = mode->pair[0];
    values[1].buffer.data = mode->buffer;
    values[1].buffer.size = sizeof(mode->buffer);
    values[2].u64 = sizeof(mode->buffer);
    values[3].i64 = -1;
    values[4].i64 = -1;
    if (leir_phase0_instance_bind(
            &storage, values, 5U, &opts) != 0 ||
        leir_phase0_instance_run(
            &storage, values_out, 5U, &metrics) != 0) {
        differential_fail(state, "differential instance run", errno);
        leir_test_close(&mode->pair[0]);
        return;
    }
    if (values_out[4].i64 != (int64_t)LEIR_DIFF_BYTES) {
        differential_fail(
            state, "differential candidate result", EPROTO);
        return;
    }
    state->candidate_metrics = metrics;
}

static int create_differential_program(
    unsigned program_length,
    leir_phase0_program_t **program_out) {
    const leir_phase0_slot_kind_t slots[5] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_node_desc_t nodes[LEIR_DIFF_MAX_ROUNDS * 2U + 2U];
    leir_phase0_program_desc_t desc;
    uint16_t return_node = (uint16_t)program_length;
    uint16_t fail_node = (uint16_t)(program_length + 1U);
    unsigned i;

    if (program_out == NULL || program_length == 0U ||
        program_length > LEIR_DIFF_MAX_ROUNDS * 2U ||
        (program_length & 1U) != 0U) {
        errno = EINVAL;
        return -1;
    }
    *program_out = NULL;
    for (i = 0U; i < program_length; i += 1U) {
        bool is_read = (i & 1U) == 0U;

        nodes[i] = (leir_phase0_node_desc_t){
            .opcode = is_read
                          ? LEIR_PHASE0_OP_READ_EXACT
                          : LEIR_PHASE0_OP_WRITE_ALL,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = is_read ? 2U : 3U,
            .result_slot = is_read ? 3U : 4U,
            .on_success =
                i + 1U == program_length
                    ? return_node
                    : (uint16_t)(i + 1U),
            .on_eof = fail_node,
            .on_error = fail_node,
        };
    }
    nodes[return_node] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_RETURN,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 4U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    nodes[fail_node] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_FAIL,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 4U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    desc = (leir_phase0_program_desc_t){
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = program_length + 2U,
        .slot_count = 5U,
        .entry_node = 0U,
    };
    return leir_phase0_program_create(&desc, program_out);
}

static int run_differential_case(unsigned program_length) {
    differential_state_t state;
    differential_task_arg_t worker_args[LEIR_DIFF_MODES];
    differential_task_arg_t peer_args[LEIR_DIFF_MODES];
    llam_task_t *tasks[LEIR_DIFF_MODES * 2U] = {NULL};
    llam_runtime_opts_t opts;
    bool runtime_started = false;
    unsigned i;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.rounds = program_length / 2U;
    atomic_init(&state.failures, 0U);
    for (i = 0U; i < LEIR_DIFF_MODES; i += 1U) {
        state.modes[i].pair[0] = LLAM_INVALID_FD;
        state.modes[i].pair[1] = LLAM_INVALID_FD;
    }
    for (i = 0U; i < state.rounds; i += 1U) {
        leir_test_fill_pattern(
            state.expected[i],
            LEIR_DIFF_BYTES,
            UINT64_C(0x4c4549521000) + i);
    }
    if (create_differential_program(
            program_length, &state.program) != 0) {
        goto cleanup;
    }
    for (i = 0U; i < LEIR_DIFF_MODES; i += 1U) {
        if (leir_test_socketpair(state.modes[i].pair) != 0) {
            goto cleanup;
        }
        worker_args[i] = (differential_task_arg_t){
            .state = &state,
            .mode = i,
        };
        peer_args[i] = worker_args[i];
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    tasks[0] = llam_spawn(
        differential_baseline_task, &worker_args[0], NULL);
    tasks[1] = llam_spawn(
        differential_candidate_task, &worker_args[1], NULL);
    tasks[2] = llam_spawn(
        differential_peer_task, &peer_args[0], NULL);
    tasks[3] = llam_spawn(
        differential_peer_task, &peer_args[1], NULL);
    if (tasks[0] == NULL || tasks[1] == NULL ||
        tasks[2] == NULL || tasks[3] == NULL) {
        (void)llam_runtime_request_stop();
    }
    if (llam_run() != 0) {
        goto cleanup;
    }
    for (i = 0U; i < LEIR_DIFF_MODES * 2U; i += 1U) {
        if (tasks[i] != NULL && llam_join(tasks[i]) != 0) {
            tasks[i] = NULL;
            goto cleanup;
        }
        tasks[i] = NULL;
    }
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "differential %u-node failed at %s: errno=%d\n",
            program_length,
            state.first_case,
            state.first_errno);
        goto cleanup;
    }
    for (i = 0U; i < state.rounds; i += 1U) {
        if (memcmp(
                state.modes[0].responses[i],
                state.expected[i],
                LEIR_DIFF_BYTES) != 0 ||
            memcmp(
                state.modes[1].responses[i],
                state.expected[i],
                LEIR_DIFF_BYTES) != 0 ||
            memcmp(
                state.modes[0].responses[i],
                state.modes[1].responses[i],
                LEIR_DIFF_BYTES) != 0) {
            goto cleanup;
        }
    }
    if (state.candidate_metrics.effect_completions != program_length ||
        state.candidate_metrics.task_parks != 1U ||
        state.candidate_metrics.terminal_publications != 1U ||
        state.candidate_metrics.task_resumes_avoided <
            state.rounds - 1U ||
        state.candidate_metrics.backend_submits > program_length ||
        state.candidate_metrics.heap_requests != 0U ||
        state.candidate_metrics.hot_allocations != 0U) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    for (i = 0U; i < LEIR_DIFF_MODES; i += 1U) {
        leir_test_close(&state.modes[i].pair[0]);
        leir_test_close(&state.modes[i].pair[1]);
    }
    leir_phase0_program_destroy(state.program);
    return failed;
}

static int test_multi_node_differential_advancement(void) {
    return run_differential_case(4U) != 0 ||
                   run_differential_case(8U) != 0
               ? 1
               : 0;
}

#define LEIR_FAIR_IO_NODES (LEIR_PHASE0_MAX_NODES - 1U)
#define LEIR_FAIR_ACTIVATIONS 256U
#define LEIR_FAIR_GAP_CAPACITY 8192U
#define LEIR_FAIR_SAMPLE_WINDOW 32U

typedef enum fairness_phase {
    LEIR_FAIR_PHASE_BASELINE = 0,
    LEIR_FAIR_PHASE_CANDIDATE = 1,
    LEIR_FAIR_PHASE_DONE = 2,
} fairness_phase_t;

typedef struct fairness_state {
    llam_fd_t pair[2];
    leir_phase0_program_t *program;
    leir_phase0_metrics_t metrics;
    unsigned char preload[
        LEIR_FAIR_IO_NODES * LEIR_FAIR_ACTIVATIONS];
    uint64_t baseline_gaps[LEIR_FAIR_GAP_CAPACITY];
    uint64_t candidate_gaps[LEIR_FAIR_GAP_CAPACITY];
    size_t baseline_gap_count;
    size_t candidate_gap_count;
    uint64_t baseline_runs;
    uint64_t candidate_runs;
    unsigned inline_budget;
    atomic_uint phase;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} fairness_state_t;

typedef struct fairness_result {
    leir_phase0_metrics_t metrics;
    uint64_t baseline_p99_ns;
    uint64_t candidate_p99_ns;
    uint64_t companion_runs;
    bool ready_path_valid;
} fairness_result_t;

static void fairness_fail(
    fairness_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void fairness_metrics_add(
    leir_phase0_metrics_t *total,
    const leir_phase0_metrics_t *sample) {
    total->activations += sample->activations;
    total->effect_completions += sample->effect_completions;
    total->backend_submits += sample->backend_submits;
    total->direct_completions += sample->direct_completions;
    total->task_parks += sample->task_parks;
    total->terminal_publications += sample->terminal_publications;
    total->task_resumes_avoided += sample->task_resumes_avoided;
    total->fairness_resubmits += sample->fairness_resubmits;
    total->stale_completions += sample->stale_completions;
    total->heap_requests += sample->heap_requests;
    total->hot_allocations += sample->hot_allocations;
}

static void fairness_companion_task(void *arg) {
    fairness_state_t *state = arg;
    unsigned previous_phase = LEIR_FAIR_PHASE_DONE;
    uint64_t previous_ns = 0U;
    unsigned window_runs = 0U;

    for (;;) {
        unsigned phase = atomic_load_explicit(
            &state->phase, memory_order_acquire);
        uint64_t now_ns;

        if (phase == LEIR_FAIR_PHASE_DONE) {
            return;
        }
        if (phase != previous_phase) {
            previous_phase = phase;
            previous_ns = llam_now_ns();
            window_runs = 0U;
        } else {
            if (phase == LEIR_FAIR_PHASE_BASELINE) {
                state->baseline_runs += 1U;
            } else {
                state->candidate_runs += 1U;
            }
            window_runs += 1U;
            if (window_runs == LEIR_FAIR_SAMPLE_WINDOW) {
                uint64_t window_ns;
                uint64_t gap_ns;

                /*
                 * One Darwin monotonic tick can be about 42 ns. Adjacent
                 * yield timestamps therefore collapse to one or two ticks
                 * and turn an unchanged 10% bound into a flaky 2x test.
                 * Aggregate a fixed number of consecutive service
                 * intervals, then normalize back to nanoseconds per
                 * service. Every complete window is sampled and the
                 * companion remains continuously ready.
                 */
                now_ns = llam_now_ns();
                window_ns =
                    now_ns >= previous_ns
                        ? now_ns - previous_ns
                        : 0U;
                gap_ns =
                    window_ns / LEIR_FAIR_SAMPLE_WINDOW;
                if (phase == LEIR_FAIR_PHASE_BASELINE) {
                    if (state->baseline_gap_count <
                        LEIR_FAIR_GAP_CAPACITY) {
                        state->baseline_gaps[
                            state->baseline_gap_count++] =
                            gap_ns;
                    }
                } else if (state->candidate_gap_count <
                           LEIR_FAIR_GAP_CAPACITY) {
                    state->candidate_gaps[
                        state->candidate_gap_count++] =
                        gap_ns;
                }
                previous_ns = now_ns;
                window_runs = 0U;
            }
        }
        llam_yield();
    }
}

static bool fairness_baseline_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    (void)node;
    (void)req;
    (void)completion_owner;
    (void)wake_reason;
    (void)context;
    return false;
}

static void fairness_candidate_task(void *arg) {
    fairness_state_t *state = arg;
    leir_phase0_instance_t instance;
    leir_phase0_value_t values[4] = {0};
    leir_phase0_value_t values_out[4] = {0};
    leir_phase0_run_opts_t opts = {
        .force_backend = false,
    };
    unsigned char byte = 0U;
    unsigned i;

    if (leir_phase0_instance_init(
            &instance, sizeof(instance), state->program) != 0) {
        fairness_fail(state, "fairness instance init", errno);
        atomic_store_explicit(
            &state->phase,
            LEIR_FAIR_PHASE_DONE,
            memory_order_release);
        return;
    }
    for (i = 0U;
         i < LEIR_FAIR_IO_NODES * LEIR_FAIR_ACTIVATIONS;
         i += 1U) {
        llam_io_req_t *req =
            llam_api_io_req_acquire(g_llam_tls_shard);

        if (req == NULL) {
            fairness_fail(
                state, "fairness baseline acquire", errno);
            atomic_store_explicit(
                &state->phase,
                LEIR_FAIR_PHASE_DONE,
                memory_order_release);
            return;
        }
        req->kind = LLAM_IO_KIND_READ;
        req->fd = state->pair[0];
        req->buf = &byte;
        req->count = sizeof(byte);
        req->completion_sink =
            fairness_baseline_completion_sink;
        req->completion_sink_context = NULL;
        if (llam_issue_io(req, false, 0U) != 0) {
            int saved_errno = errno;

            req->completion_sink = NULL;
            llam_api_io_req_release(g_llam_tls_shard, req);
            fairness_fail(
                state,
                "fairness baseline await",
                saved_errno);
            atomic_store_explicit(
                &state->phase,
                LEIR_FAIR_PHASE_DONE,
                memory_order_release);
            return;
        }
        req->completion_sink = NULL;
        req->completion_sink_context = NULL;
        llam_api_io_req_release(g_llam_tls_shard, req);
    }
    if (leir_test_write_all(
            state->pair[1],
            state->preload,
            sizeof(state->preload)) != 0) {
        fairness_fail(
            state, "fairness candidate preload", errno);
        atomic_store_explicit(
            &state->phase,
            LEIR_FAIR_PHASE_DONE,
            memory_order_release);
        return;
    }

    opts.inline_budget = state->inline_budget;
    values[0].fd = state->pair[0];
    values[1].buffer.data = &byte;
    values[1].buffer.size = sizeof(byte);
    values[2].u64 = sizeof(byte);
    atomic_store_explicit(
        &state->phase,
        LEIR_FAIR_PHASE_CANDIDATE,
        memory_order_release);
    for (i = 0U; i < LEIR_FAIR_ACTIVATIONS; i += 1U) {
        leir_phase0_metrics_t metrics;

        values[3].i64 = -1;
        if (leir_phase0_instance_bind(
                &instance, values, 4U, &opts) != 0 ||
            leir_phase0_instance_run(
                &instance, values_out, 4U, &metrics) != 0 ||
            values_out[3].i64 != 1) {
            fairness_fail(state, "fairness activation", errno);
            break;
        }
        fairness_metrics_add(&state->metrics, &metrics);
    }
    atomic_store_explicit(
        &state->phase,
        LEIR_FAIR_PHASE_DONE,
        memory_order_release);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t lhs = *(const uint64_t *)left;
    uint64_t rhs = *(const uint64_t *)right;

    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static uint64_t fairness_p99(
    uint64_t *samples,
    size_t sample_count) {
    size_t index;

    if (sample_count == 0U) {
        return 0U;
    }
    qsort(
        samples,
        sample_count,
        sizeof(samples[0]),
        compare_u64);
    index = (sample_count * 99U + 99U) / 100U;
    return samples[index == 0U ? 0U : index - 1U];
}

static int create_fairness_program(
    leir_phase0_program_t **program_out) {
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_program_desc_t desc;
    uint16_t terminal_node = (uint16_t)LEIR_FAIR_IO_NODES;
    unsigned i;

    if (program_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LEIR_FAIR_IO_NODES; i += 1U) {
        nodes[i] = (leir_phase0_node_desc_t){
            .opcode = LEIR_PHASE0_OP_READ,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success =
                i + 1U == LEIR_FAIR_IO_NODES
                    ? terminal_node
                    : (uint16_t)(i + 1U),
            .on_eof = terminal_node,
            .on_error = terminal_node,
        };
    }
    nodes[terminal_node] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_RETURN,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 3U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    desc = (leir_phase0_program_desc_t){
        .nodes = nodes,
        .slot_kinds = valid_slots,
        .node_count = LEIR_PHASE0_MAX_NODES,
        .slot_count = sizeof(valid_slots) / sizeof(valid_slots[0]),
        .entry_node = 0U,
    };
    return leir_phase0_program_create(&desc, program_out);
}

static int run_fairness_case(
    unsigned inline_budget,
    fairness_result_t *result_out) {
    fairness_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *companion = NULL;
    llam_task_t *candidate = NULL;
    uint64_t expected_effects =
        LEIR_FAIR_IO_NODES * LEIR_FAIR_ACTIVATIONS;
    uint64_t expected_backend_per_activation =
        (LEIR_FAIR_IO_NODES + inline_budget - 1U) /
        inline_budget;
    uint64_t expected_fairness_per_activation =
        (LEIR_FAIR_IO_NODES - 1U) / inline_budget;
    bool runtime_started = false;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.pair[0] = LLAM_INVALID_FD;
    state.pair[1] = LLAM_INVALID_FD;
    state.inline_budget = inline_budget;
    atomic_init(&state.phase, LEIR_FAIR_PHASE_BASELINE);
    atomic_init(&state.failures, 0U);
    memset(result_out, 0, sizeof(*result_out));
    leir_test_fill_pattern(
        state.preload,
        sizeof(state.preload),
        UINT64_C(0x4c45495246414952) + inline_budget);
    if (create_fairness_program(&state.program) != 0 ||
        leir_test_socketpair(state.pair) != 0 ||
        leir_test_write_all(
            state.pair[1],
            state.preload,
            sizeof(state.preload)) != 0) {
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
    runtime_started = true;
    companion = llam_spawn(fairness_companion_task, &state, NULL);
    candidate = llam_spawn(fairness_candidate_task, &state, NULL);
    if (companion == NULL || candidate == NULL ||
        llam_run() != 0) {
        goto cleanup;
    }
    if (llam_join(companion) != 0 ||
        llam_join(candidate) != 0) {
        companion = NULL;
        candidate = NULL;
        goto cleanup;
    }
    companion = NULL;
    candidate = NULL;
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "fairness budget %u failed at %s: errno=%d\n",
            inline_budget,
            state.first_case,
            state.first_errno);
        goto cleanup;
    }

    result_out->metrics = state.metrics;
    result_out->baseline_p99_ns = fairness_p99(
        state.baseline_gaps, state.baseline_gap_count);
    result_out->candidate_p99_ns = fairness_p99(
        state.candidate_gaps, state.candidate_gap_count);
    result_out->companion_runs = state.candidate_runs;
    result_out->ready_path_valid =
        state.metrics.effect_completions == expected_effects &&
        state.metrics.backend_submits ==
            expected_backend_per_activation *
                LEIR_FAIR_ACTIVATIONS &&
        state.metrics.direct_completions ==
            expected_effects -
                state.metrics.backend_submits &&
        state.metrics.fairness_resubmits ==
            expected_fairness_per_activation *
                LEIR_FAIR_ACTIVATIONS;
    if (state.baseline_runs == 0U ||
        state.candidate_runs == 0U ||
        state.baseline_gap_count < 100U ||
        (inline_budget == 1U &&
         state.candidate_gap_count < 100U) ||
        state.metrics.activations != LEIR_FAIR_ACTIVATIONS ||
        state.metrics.task_parks != LEIR_FAIR_ACTIVATIONS ||
        state.metrics.terminal_publications !=
            LEIR_FAIR_ACTIVATIONS ||
        state.metrics.task_resumes_avoided !=
            (LEIR_FAIR_IO_NODES - 1U) *
                LEIR_FAIR_ACTIVATIONS ||
        state.metrics.heap_requests != 0U ||
        state.metrics.hot_allocations != 0U ||
        !runtime_pending_ops_are_zero()) {
        fprintf(
            stderr,
            "fairness budget %u integrity regression: "
            "baseline_runs=%llu baseline_gaps=%zu "
            "candidate_runs=%llu candidate_gaps=%zu "
            "activations=%llu parks=%llu terminal=%llu "
            "avoided=%llu heap=%llu hot=%llu\n",
            inline_budget,
            (unsigned long long)state.baseline_runs,
            state.baseline_gap_count,
            (unsigned long long)state.candidate_runs,
            state.candidate_gap_count,
            (unsigned long long)state.metrics.activations,
            (unsigned long long)state.metrics.task_parks,
            (unsigned long long)state.metrics.terminal_publications,
            (unsigned long long)
                state.metrics.task_resumes_avoided,
            (unsigned long long)state.metrics.heap_requests,
            (unsigned long long)state.metrics.hot_allocations);
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

static int test_inline_budget_fairness_and_companion_service(void) {
    const unsigned budgets[] = {1U, 8U, 32U};
    enum { latency_repetitions = 5 };
    fairness_result_t results[
        sizeof(budgets) / sizeof(budgets[0])];
    uint64_t baseline_p99[latency_repetitions];
    uint64_t candidate_p99[latency_repetitions];
    uint64_t latency_ratios[latency_repetitions];
    bool latency_gating = true;
    size_t i;

    for (i = 0U; i < latency_repetitions; i += 1U) {
        if (run_fairness_case(budgets[0], &results[0]) != 0 ||
            results[0].baseline_p99_ns == 0U ||
            results[0].candidate_p99_ns >
                UINT64_MAX / UINT64_C(1000000)) {
            return 1;
        }
        latency_gating =
            latency_gating && results[0].ready_path_valid;
        baseline_p99[i] = results[0].baseline_p99_ns;
        candidate_p99[i] = results[0].candidate_p99_ns;
        latency_ratios[i] =
            results[0].candidate_p99_ns * UINT64_C(1000000) /
            results[0].baseline_p99_ns;
    }
    for (i = 1U; i < sizeof(budgets) / sizeof(budgets[0]); i += 1U) {
        if (run_fairness_case(budgets[i], &results[i]) != 0) {
            return 1;
        }
        latency_gating =
            latency_gating && results[i].ready_path_valid;
    }
    if (results[0].metrics.fairness_resubmits == 0U ||
        results[0].companion_runs == 0U) {
        return 1;
    }
    qsort(
        latency_ratios,
        latency_repetitions,
        sizeof(latency_ratios[0]),
        compare_u64);
    if (!LEIR_TEST_THREAD_SANITIZER &&
        latency_gating &&
        latency_ratios[latency_repetitions / 2U] >
            UINT64_C(1100000)) {
        fprintf(
            stderr,
            "fairness median p99 ratio regression: ratio_ppm=%llu "
            "samples=%llu/%llu,%llu/%llu,%llu/%llu,%llu/%llu,"
            "%llu/%llu\n",
            (unsigned long long)
                latency_ratios[latency_repetitions / 2U],
            (unsigned long long)baseline_p99[0],
            (unsigned long long)candidate_p99[0],
            (unsigned long long)baseline_p99[1],
            (unsigned long long)candidate_p99[1],
            (unsigned long long)baseline_p99[2],
            (unsigned long long)candidate_p99[2],
            (unsigned long long)baseline_p99[3],
            (unsigned long long)candidate_p99[3],
            (unsigned long long)baseline_p99[4],
            (unsigned long long)candidate_p99[4]);
        return 1;
    }
    return 0;
}

#define LEIR_PARTIAL_BYTES (16U * 1024U)
#define LEIR_PARTIAL_SLICE 257U
#define LEIR_PARTIAL_EOF_BYTES 777U

typedef enum partial_io_case {
    PARTIAL_IO_READ_EXACT = 0,
    PARTIAL_IO_WRITE_ALL = 1,
    PARTIAL_IO_EOF = 2,
    PARTIAL_IO_ERROR = 3,
} partial_io_case_t;

typedef struct partial_io_state {
    llam_fd_t pair[2];
    leir_phase0_program_t *program;
    unsigned char sent[LEIR_PARTIAL_BYTES];
    unsigned char observed[LEIR_PARTIAL_BYTES];
    leir_phase0_metrics_t metrics;
    partial_io_case_t test_case;
    int run_result;
    int run_errno;
    int64_t result_value;
    atomic_uint failures;
    int first_errno;
    char first_case[96];
} partial_io_state_t;

static void partial_io_fail(
    partial_io_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int create_partial_io_program(
    partial_io_case_t test_case,
    leir_phase0_program_t **program_out) {
    const leir_phase0_slot_kind_t slots[4] = {
        LEIR_PHASE0_SLOT_FD,
        test_case == PARTIAL_IO_WRITE_ALL ||
                test_case == PARTIAL_IO_ERROR
            ? LEIR_PHASE0_SLOT_CONST_BUFFER
            : LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_program_desc_t desc;
    bool write_op =
        test_case == PARTIAL_IO_WRITE_ALL ||
        test_case == PARTIAL_IO_ERROR;

    nodes[0] = (leir_phase0_node_desc_t){
        .opcode =
            write_op
                ? LEIR_PHASE0_OP_WRITE_ALL
                : LEIR_PHASE0_OP_READ_EXACT,
        .fd_slot = 0U,
        .buffer_slot = 1U,
        .length_slot = 2U,
        .result_slot = 3U,
        .on_success =
            test_case == PARTIAL_IO_EOF ? 2U : 1U,
        .on_eof =
            test_case == PARTIAL_IO_EOF ? 1U : 2U,
        .on_error = 2U,
    };
    nodes[1] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_RETURN,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 3U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    nodes[2] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_FAIL,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 3U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    desc = (leir_phase0_program_desc_t){
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = 3U,
        .slot_count = 4U,
        .entry_node = 0U,
    };
    return leir_phase0_program_create(&desc, program_out);
}

static void partial_io_peer_task(void *arg) {
    partial_io_state_t *state = arg;
    size_t offset = 0U;

    if (llam_sleep_ns(UINT64_C(2) * 1000U * 1000U) != 0) {
        partial_io_fail(state, "partial peer initial sleep", errno);
        return;
    }

    if (state->test_case == PARTIAL_IO_EOF) {
        if (leir_test_write_all(
                state->pair[1],
                state->sent,
                LEIR_PARTIAL_EOF_BYTES) != 0 ||
            leir_test_shutdown_write(state->pair[1]) != 0) {
            partial_io_fail(state, "partial peer EOF", errno);
        }
        return;
    }

    while (offset < LEIR_PARTIAL_BYTES) {
        size_t slice = LEIR_PARTIAL_BYTES - offset;

        if (slice > LEIR_PARTIAL_SLICE) {
            slice = LEIR_PARTIAL_SLICE;
        }
        if (state->test_case == PARTIAL_IO_READ_EXACT) {
            if (leir_test_write_all(
                    state->pair[1],
                    state->sent + offset,
                    slice) != 0) {
                partial_io_fail(state, "partial peer write", errno);
                return;
            }
        } else if (leir_test_read_exact(
                       state->pair[1],
                       state->observed + offset,
                       slice) != 0) {
            partial_io_fail(state, "partial peer read", errno);
            return;
        }
        offset += slice;
        if (offset < LEIR_PARTIAL_BYTES &&
            llam_sleep_ns(UINT64_C(100) * 1000U) != 0) {
            partial_io_fail(state, "partial peer slice sleep", errno);
            return;
        }
    }
}

static void partial_io_instance_task(void *arg) {
    partial_io_state_t *state = arg;
    leir_phase0_instance_t storage;
    leir_phase0_value_t values[4] = {0};
    leir_phase0_value_t values_out[4] = {0};
    leir_phase0_metrics_t metrics;
    leir_phase0_run_opts_t opts = {
        .inline_budget = 8U,
        .force_backend = false,
    };
    bool write_op =
        state->test_case == PARTIAL_IO_WRITE_ALL ||
        state->test_case == PARTIAL_IO_ERROR;

    memset(&metrics, 0, sizeof(metrics));
    if (leir_phase0_instance_init(
            &storage, sizeof(storage), state->program) != 0) {
        partial_io_fail(state, "partial instance init", errno);
        return;
    }
    values[0].fd = state->pair[0];
    values[1].buffer.data =
        write_op ? state->sent : state->observed;
    values[1].buffer.size = LEIR_PARTIAL_BYTES;
    values[2].u64 = LEIR_PARTIAL_BYTES;
    values[3].i64 = -1;
    if (leir_phase0_instance_bind(
            &storage, values, 4U, &opts) != 0) {
        partial_io_fail(state, "partial instance bind", errno);
        return;
    }

    errno = 0;
    state->run_result = leir_phase0_instance_run(
        &storage, values_out, 4U, &metrics);
    state->run_errno = errno;
    state->result_value = values_out[3].i64;
    state->metrics = metrics;
    if (state->test_case == PARTIAL_IO_ERROR) {
        if (state->run_result != -1 || state->run_errno == 0) {
            partial_io_fail(
                state, "partial expected error result", EPROTO);
        }
    } else if (state->run_result != 0) {
        partial_io_fail(
            state, "partial expected success result", state->run_errno);
    }
}

static int run_partial_io_case(partial_io_case_t test_case) {
    partial_io_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *instance_task = NULL;
    llam_task_t *peer_task = NULL;
    bool runtime_started = false;
    bool needs_peer = test_case != PARTIAL_IO_ERROR;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.pair[0] = LLAM_INVALID_FD;
    state.pair[1] = LLAM_INVALID_FD;
    state.test_case = test_case;
    state.run_result = -2;
    atomic_init(&state.failures, 0U);
    leir_test_fill_pattern(
        state.sent,
        sizeof(state.sent),
        UINT64_C(0x4c45495250415254) + (uint64_t)test_case);

    if (create_partial_io_program(test_case, &state.program) != 0 ||
        leir_test_socketpair(state.pair) != 0) {
        goto cleanup;
    }
    (void)leir_test_set_socket_buffers(state.pair[0], 1024);
    (void)leir_test_set_socket_buffers(state.pair[1], 1024);
    if (!needs_peer) {
        leir_test_close(&state.pair[1]);
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    instance_task =
        llam_spawn(partial_io_instance_task, &state, NULL);
    if (needs_peer) {
        peer_task =
            llam_spawn(partial_io_peer_task, &state, NULL);
    }
    if (instance_task == NULL || (needs_peer && peer_task == NULL)) {
        (void)llam_runtime_request_stop();
    }
    if (llam_run() != 0 ||
        (instance_task != NULL && llam_join(instance_task) != 0) ||
        (peer_task != NULL && llam_join(peer_task) != 0)) {
        instance_task = NULL;
        peer_task = NULL;
        goto cleanup;
    }
    instance_task = NULL;
    peer_task = NULL;

    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "partial case %u failed at %s: errno=%d\n",
            (unsigned)test_case,
            state.first_case,
            state.first_errno);
        goto cleanup;
    }
    if (state.metrics.activations != 1U ||
        state.metrics.backend_submits == 0U ||
        state.metrics.effect_completions == 0U ||
        state.metrics.task_parks != 1U ||
        state.metrics.terminal_publications != 1U ||
        state.metrics.task_resumes_avoided + 1U !=
            state.metrics.effect_completions ||
        state.metrics.direct_completions >
            state.metrics.effect_completions ||
        state.metrics.heap_requests != 0U ||
        state.metrics.hot_allocations != 0U) {
        goto cleanup;
    }

    if (test_case == PARTIAL_IO_READ_EXACT) {
        if (state.result_value != (int64_t)LEIR_PARTIAL_BYTES ||
            state.metrics.effect_completions <= 1U ||
            memcmp(
                state.sent,
                state.observed,
                LEIR_PARTIAL_BYTES) != 0) {
            goto cleanup;
        }
    } else if (test_case == PARTIAL_IO_WRITE_ALL) {
        if (state.result_value != (int64_t)LEIR_PARTIAL_BYTES ||
            state.metrics.effect_completions <= 1U ||
            memcmp(
                state.sent,
                state.observed,
                LEIR_PARTIAL_BYTES) != 0) {
            goto cleanup;
        }
    } else if (test_case == PARTIAL_IO_EOF) {
        if (state.result_value != (int64_t)LEIR_PARTIAL_EOF_BYTES ||
            state.metrics.effect_completions < 2U ||
            memcmp(
                state.sent,
                state.observed,
                LEIR_PARTIAL_EOF_BYTES) != 0) {
            goto cleanup;
        }
    } else if (state.result_value != -1 ||
               state.run_result != -1 ||
               state.run_errno == 0) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

static int test_partial_exact_eof_and_error_paths(void) {
    return run_partial_io_case(PARTIAL_IO_READ_EXACT) != 0 ||
                   run_partial_io_case(PARTIAL_IO_WRITE_ALL) != 0 ||
                   run_partial_io_case(PARTIAL_IO_EOF) != 0 ||
                   run_partial_io_case(PARTIAL_IO_ERROR) != 0
               ? 1
               : 0;
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
    if (test_benchmark_option_parser() != 0) {
        fputs("test_benchmark_option_parser failed\n", stderr);
        return 1;
    }
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
    if (test_program_accepts_result_length_dataflow() != 0) {
        fputs("test_program_accepts_result_length_dataflow failed\n",
              stderr);
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
    if (test_instance_cancel_before_run_publishes_request() != 0) {
        fputs("test_instance_cancel_before_run_publishes_request failed\n",
              stderr);
        return 1;
    }
    if (test_stale_and_duplicate_completion_injection() != 0) {
        fputs("test_stale_and_duplicate_completion_injection failed\n",
              stderr);
        return 1;
    }
    if (test_pre_submit_inflight_and_racing_cancellation() != 0) {
        fputs("test_pre_submit_inflight_and_racing_cancellation failed\n",
              stderr);
        return 1;
    }
    if (test_pending_read_terminal_uses_one_park() != 0) {
        fputs("test_pending_read_terminal_uses_one_park failed\n", stderr);
        return 1;
    }
    if (test_embedded_request_reuse_avoids_heap_fallback() != 0) {
        fputs(
            "test_embedded_request_reuse_avoids_heap_fallback failed\n",
            stderr);
        return 1;
    }
    if (test_multi_node_differential_advancement() != 0) {
        fputs("test_multi_node_differential_advancement failed\n", stderr);
        return 1;
    }
    if (test_inline_budget_fairness_and_companion_service() != 0) {
        fputs("test_inline_budget_fairness_and_companion_service failed\n",
              stderr);
        return 1;
    }
    if (test_partial_exact_eof_and_error_paths() != 0) {
        fputs("test_partial_exact_eof_and_error_paths failed\n", stderr);
        return 1;
    }
    puts("LEIR Phase 0 tests passed");
    return 0;
}
