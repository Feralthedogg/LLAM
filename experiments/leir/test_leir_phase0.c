#include "runtime_internal.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"

#include <errno.h>
#include <stdio.h>
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
    puts("LEIR Phase 0 tests passed");
    return 0;
}
