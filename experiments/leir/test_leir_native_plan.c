#include "leir_native_plan.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TEST_FD_SLOT 0U
#define TEST_MUT_BUFFER_SLOT 1U
#define TEST_CONST_BUFFER_SLOT 2U
#define TEST_LENGTH_SLOT 3U
#define TEST_FIRST_RESULT_SLOT 4U

typedef struct linear_fixture_options {
    unsigned operation_count;
    bool start_with_send;
    bool exact_operations;
    bool alternate;
    bool return_last_result;
} linear_fixture_options_t;

static leir_phase0_node_desc_t terminal_node(
    leir_phase0_opcode_t opcode,
    uint16_t result_slot) {
    leir_phase0_node_desc_t node;

    memset(&node, 0, sizeof(node));
    node.opcode = (uint16_t)opcode;
    node.fd_slot = LEIR_PHASE0_NODE_NONE;
    node.buffer_slot = LEIR_PHASE0_NODE_NONE;
    node.length_slot = LEIR_PHASE0_NODE_NONE;
    node.result_slot = result_slot;
    node.on_success = LEIR_PHASE0_NODE_NONE;
    node.on_eof = LEIR_PHASE0_NODE_NONE;
    node.on_error = LEIR_PHASE0_NODE_NONE;
    return node;
}

static int create_linear_program(
    const linear_fixture_options_t *options,
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_slot_kind_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_program_desc_t desc;
    unsigned return_node_index;
    unsigned fail_node_index;
    unsigned i;

    if (options == NULL || out == NULL ||
        options->operation_count == 0U ||
        options->operation_count + 2U > LEIR_PHASE0_MAX_NODES ||
        TEST_FIRST_RESULT_SLOT + options->operation_count >
            LEIR_PHASE0_MAX_SLOTS) {
        errno = EINVAL;
        return -1;
    }

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[TEST_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[TEST_MUT_BUFFER_SLOT] = LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[TEST_CONST_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_CONST_BUFFER;
    slots[TEST_LENGTH_SLOT] = LEIR_PHASE0_SLOT_U64;
    for (i = 0U; i < options->operation_count; i += 1U) {
        slots[TEST_FIRST_RESULT_SLOT + i] =
            LEIR_PHASE0_SLOT_I64;
    }

    return_node_index = options->operation_count;
    fail_node_index = options->operation_count + 1U;
    for (i = 0U; i < options->operation_count; i += 1U) {
        bool send = options->alternate
            ? (((i & 1U) != 0U) != options->start_with_send)
            : options->start_with_send;
        leir_phase0_node_desc_t *node = &nodes[i];

        node->opcode = (uint16_t)(
            send
                ? (options->exact_operations
                       ? LEIR_PHASE0_OP_WRITE_ALL
                       : LEIR_PHASE0_OP_WRITE)
                : (options->exact_operations
                       ? LEIR_PHASE0_OP_READ_EXACT
                       : LEIR_PHASE0_OP_READ));
        node->fd_slot = TEST_FD_SLOT;
        node->buffer_slot = send
            ? TEST_CONST_BUFFER_SLOT
            : TEST_MUT_BUFFER_SLOT;
        node->length_slot = TEST_LENGTH_SLOT;
        node->result_slot =
            (uint16_t)(TEST_FIRST_RESULT_SLOT + i);
        node->on_success = (uint16_t)(
            i + 1U < options->operation_count
                ? i + 1U
                : return_node_index);
        node->on_eof = (uint16_t)fail_node_index;
        node->on_error = (uint16_t)fail_node_index;
    }
    nodes[return_node_index] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        (uint16_t)(
            options->return_last_result
                ? TEST_FIRST_RESULT_SLOT +
                      options->operation_count - 1U
                : TEST_FIRST_RESULT_SLOT));
    nodes[fail_node_index] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        TEST_FIRST_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = options->operation_count + 2U;
    desc.slot_count =
        TEST_FIRST_RESULT_SLOT + options->operation_count;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int compile_must_fail(leir_phase0_program_t *program) {
    leir_native_plan_t plan;
    int result;

    memset(&plan, 0xa5, sizeof(plan));
    errno = 0;
    result = leir_native_plan_compile(program, &plan);
    if (result != -1 || errno != EINVAL) {
        fprintf(
            stderr,
            "expected planner EINVAL, result=%d errno=%d\n",
            result,
            errno);
        return 1;
    }
    return 0;
}

static int test_compile_exact_linear_lengths(void) {
    static const unsigned counts[] = {1U, 2U, 4U, 8U};
    static const uint16_t expected_kinds[8] = {
        LEIR_NATIVE_STEP_RECV,
        LEIR_NATIVE_STEP_SEND,
        LEIR_NATIVE_STEP_RECV,
        LEIR_NATIVE_STEP_SEND,
        LEIR_NATIVE_STEP_RECV,
        LEIR_NATIVE_STEP_SEND,
        LEIR_NATIVE_STEP_RECV,
        LEIR_NATIVE_STEP_SEND,
    };
    unsigned case_index;

    for (case_index = 0U;
         case_index < sizeof(counts) / sizeof(counts[0]);
         case_index += 1U) {
        linear_fixture_options_t options = {
            counts[case_index],
            false,
            true,
            true,
            true,
        };
        leir_phase0_program_t *program = NULL;
        leir_native_plan_t plan;
        unsigned i;

        if (create_linear_program(&options, &program) != 0) {
            perror("create exact linear program");
            return 1;
        }
        memset(&plan, 0, sizeof(plan));
        if (leir_native_plan_compile(program, &plan) != 0) {
            perror("compile exact linear program");
            leir_phase0_program_destroy(program);
            return 1;
        }
        if (plan.step_count != counts[case_index] ||
            plan.return_node != counts[case_index] ||
            plan.result_slot !=
                TEST_FIRST_RESULT_SLOT + counts[case_index] - 1U) {
            fprintf(stderr, "planner summary mismatch\n");
            leir_phase0_program_destroy(program);
            return 1;
        }
        for (i = 0U; i < counts[case_index]; i += 1U) {
            const leir_native_step_t *step = &plan.steps[i];

            if (step->kind != expected_kinds[i] ||
                step->fd_slot != TEST_FD_SLOT ||
                step->buffer_slot !=
                    ((i & 1U) != 0U
                         ? TEST_CONST_BUFFER_SLOT
                         : TEST_MUT_BUFFER_SLOT) ||
                step->length_slot != TEST_LENGTH_SLOT ||
                step->result_slot != TEST_FIRST_RESULT_SLOT + i) {
                fprintf(
                    stderr,
                    "planner step mismatch count=%u index=%u\n",
                    counts[case_index],
                    i);
                leir_phase0_program_destroy(program);
                return 1;
            }
        }
        leir_phase0_program_destroy(program);
    }
    return 0;
}

static int test_compile_can_start_with_send(void) {
    const linear_fixture_options_t options = {
        2U,
        true,
        true,
        true,
        true,
    };
    leir_phase0_program_t *program = NULL;
    leir_native_plan_t plan;
    int failed = 0;

    if (create_linear_program(&options, &program) != 0 ||
        leir_native_plan_compile(program, &plan) != 0) {
        perror("compile send-first program");
        leir_phase0_program_destroy(program);
        return 1;
    }
    if (plan.steps[0].kind != LEIR_NATIVE_STEP_SEND ||
        plan.steps[0].buffer_slot != TEST_CONST_BUFFER_SLOT ||
        plan.steps[1].kind != LEIR_NATIVE_STEP_RECV ||
        plan.steps[1].buffer_slot != TEST_MUT_BUFFER_SLOT) {
        fprintf(stderr, "send-first lowering mismatch\n");
        failed = 1;
    }
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_compile_copies_exact_slot_indices(void) {
    const linear_fixture_options_t options = {
        1U,
        false,
        true,
        true,
        true,
    };
    leir_phase0_program_t *program = NULL;
    leir_native_plan_t plan;
    int failed = 0;

    if (create_linear_program(&options, &program) != 0 ||
        leir_native_plan_compile(program, &plan) != 0) {
        perror("compile slot copy program");
        leir_phase0_program_destroy(program);
        return 1;
    }
    if (plan.steps[0].fd_slot != 0U ||
        plan.steps[0].buffer_slot != 1U ||
        plan.steps[0].length_slot != 3U ||
        plan.steps[0].result_slot != 4U) {
        fprintf(stderr, "slot indices were not copied exactly\n");
        failed = 1;
    }
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_rejects_non_power_of_two_operation_count(void) {
    const linear_fixture_options_t options = {
        3U,
        false,
        true,
        true,
        true,
    };
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_linear_program(&options, &program) != 0) {
        perror("create three-operation program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_rejects_read_and_write_non_exact_opcodes(void) {
    const linear_fixture_options_t options = {
        2U,
        false,
        false,
        true,
        true,
    };
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_linear_program(&options, &program) != 0) {
        perror("create non-exact program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_rejects_non_alternating_steps(void) {
    const linear_fixture_options_t options = {
        2U,
        false,
        true,
        false,
        true,
    };
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_linear_program(&options, &program) != 0) {
        perror("create non-alternating program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int create_cycle_program(leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[3];
    leir_phase0_slot_kind_t slots[6] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = 0U;
    nodes[0].buffer_slot = 1U;
    nodes[0].length_slot = 3U;
    nodes[0].result_slot = 4U;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 2U;
    nodes[0].on_error = 2U;
    nodes[1].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    nodes[1].fd_slot = 0U;
    nodes[1].buffer_slot = 2U;
    nodes[1].length_slot = 3U;
    nodes[1].result_slot = 5U;
    nodes[1].on_success = 0U;
    nodes[1].on_eof = 2U;
    nodes[1].on_error = 2U;
    nodes[2] = terminal_node(LEIR_PHASE0_OP_FAIL, 4U);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 3U;
    desc.slot_count = 6U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int test_rejects_success_cycle(void) {
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_cycle_program(&program) != 0) {
        perror("create success cycle");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int create_success_to_fail_program(
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[2];
    leir_phase0_slot_kind_t slots[5] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = 0U;
    nodes[0].buffer_slot = 1U;
    nodes[0].length_slot = 3U;
    nodes[0].result_slot = 4U;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 1U;
    nodes[0].on_error = 1U;
    nodes[1] = terminal_node(LEIR_PHASE0_OP_FAIL, 4U);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 2U;
    desc.slot_count = 5U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int test_rejects_success_branch_to_fail(void) {
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_success_to_fail_program(&program) != 0) {
        perror("create success-to-fail program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int create_nonterminal_failure_edge_program(
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t slots[6] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = 0U;
    nodes[0].buffer_slot = 1U;
    nodes[0].length_slot = 3U;
    nodes[0].result_slot = 4U;
    nodes[0].on_success = 2U;
    nodes[0].on_eof = 1U;
    nodes[0].on_error = 3U;
    nodes[1].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    nodes[1].fd_slot = 0U;
    nodes[1].buffer_slot = 2U;
    nodes[1].length_slot = 3U;
    nodes[1].result_slot = 5U;
    nodes[1].on_success = 2U;
    nodes[1].on_eof = 3U;
    nodes[1].on_error = 3U;
    nodes[2] = terminal_node(LEIR_PHASE0_OP_RETURN, 4U);
    nodes[3] = terminal_node(LEIR_PHASE0_OP_FAIL, 4U);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = 6U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int test_rejects_nonterminal_eof_or_error_edge(void) {
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_nonterminal_failure_edge_program(&program) != 0) {
        perror("create nonterminal failure edge program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int create_result_dependent_length_program(
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t slots[6] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = 0U;
    nodes[0].buffer_slot = 1U;
    nodes[0].length_slot = 3U;
    nodes[0].result_slot = 4U;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 3U;
    nodes[0].on_error = 3U;
    nodes[1].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    nodes[1].fd_slot = 0U;
    nodes[1].buffer_slot = 2U;
    nodes[1].length_slot = 4U;
    nodes[1].result_slot = 5U;
    nodes[1].on_success = 2U;
    nodes[1].on_eof = 3U;
    nodes[1].on_error = 3U;
    nodes[2] = terminal_node(LEIR_PHASE0_OP_RETURN, 5U);
    nodes[3] = terminal_node(LEIR_PHASE0_OP_FAIL, 4U);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = 6U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int test_rejects_prior_result_as_later_length(void) {
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_result_dependent_length_program(&program) != 0) {
        perror("create result-dependent length program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_rejects_nonfinal_return_result(void) {
    const linear_fixture_options_t options = {
        2U,
        false,
        true,
        true,
        false,
    };
    leir_phase0_program_t *program = NULL;
    int failed;

    if (create_linear_program(&options, &program) != 0) {
        perror("create nonfinal return-result program");
        return 1;
    }
    failed = compile_must_fail(program);
    leir_phase0_program_destroy(program);
    return failed;
}

typedef int (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn run;
} test_case_t;

int main(void) {
    static const test_case_t tests[] = {
        {"compile exact linear lengths",
         test_compile_exact_linear_lengths},
        {"compile can start with send",
         test_compile_can_start_with_send},
        {"compile copies exact slot indices",
         test_compile_copies_exact_slot_indices},
        {"reject non-power-of-two operation count",
         test_rejects_non_power_of_two_operation_count},
        {"reject non-exact operations",
         test_rejects_read_and_write_non_exact_opcodes},
        {"reject non-alternating steps",
         test_rejects_non_alternating_steps},
        {"reject success cycle",
         test_rejects_success_cycle},
        {"reject success branch to fail",
         test_rejects_success_branch_to_fail},
        {"reject nonterminal failure edge",
         test_rejects_nonterminal_eof_or_error_edge},
        {"reject result-dependent length",
         test_rejects_prior_result_as_later_length},
        {"reject nonfinal return result",
         test_rejects_nonfinal_return_result},
    };
    size_t i;

    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i += 1U) {
        if (tests[i].run() != 0) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return 1;
        }
    }
    puts("LEIR native planner tests passed");
    return 0;
}
