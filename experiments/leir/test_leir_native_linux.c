#include <stdio.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR native Linux tests require Linux");
    return 0;
}

#else

#include "io/linux/runtime_io_segment_linux_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef int (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn run;
} test_case_t;

static void fill_operations(
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32],
    unsigned count) {
    static const uint16_t expected_kinds[8] = {
        LLAM_LINUX_NATIVE_OP_RECV,
        LLAM_LINUX_NATIVE_OP_SEND,
        LLAM_LINUX_NATIVE_OP_RECV,
        LLAM_LINUX_NATIVE_OP_SEND,
        LLAM_LINUX_NATIVE_OP_RECV,
        LLAM_LINUX_NATIVE_OP_SEND,
        LLAM_LINUX_NATIVE_OP_RECV,
        LLAM_LINUX_NATIVE_OP_SEND,
    };
    unsigned i;

    memset(
        ops,
        0,
        LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS * sizeof(ops[0]));
    memset(
        buffers,
        0,
        LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS * sizeof(buffers[0]));
    for (i = 0U; i < count; i += 1U) {
        ops[i].kind = expected_kinds[i];
        ops[i].result_slot = (uint16_t)(4U + i);
        ops[i].fd = (llam_fd_t)(10 + (int)i);
        ops[i].buffer = buffers[i];
        ops[i].length = (uint32_t)(16U + i);
    }
}

static int configure_segment(
    llam_linux_native_segment_t *segment,
    llam_linux_native_segment_mode_t mode,
    unsigned count,
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32]) {
    unsigned i;

    fill_operations(ops, buffers, count);
    if (llam_linux_native_segment_configure(
            segment, ops, count, mode) != 0) {
        return -1;
    }
    segment->generation = UINT64_C(42);
    for (i = 0U; i < count; i += 1U) {
        segment->tokens[i].generation = UINT64_C(42);
    }
    atomic_store_explicit(
        &segment->state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);
    return 0;
}

static int test_validates_configuration(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned valid_counts[] = {1U, 2U, 4U, 8U};
    size_t i;

    for (i = 0U;
         i < sizeof(valid_counts) / sizeof(valid_counts[0]);
         i += 1U) {
        fill_operations(ops, buffers, valid_counts[i]);
        if (llam_linux_native_segment_configure(
                &segment,
                ops,
                valid_counts[i],
                LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
            fprintf(
                stderr,
                "valid operation count %u was rejected\n",
                valid_counts[i]);
            return 1;
        }
    }

    fill_operations(ops, buffers, 4U);
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            3U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "invalid operation count was accepted\n");
        return 1;
    }
    ops[1].kind = ops[0].kind;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "non-alternating operations were accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].fd = -1;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "invalid descriptor was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].buffer = NULL;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "null buffer was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].length = 0U;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "zero length was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            (llam_linux_native_segment_mode_t)2) == 0) {
        fprintf(stderr, "invalid mode was accepted\n");
        return 1;
    }
    return 0;
}

static int test_encodes_recv_and_send_fields(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe recv_sqe;
    struct io_uring_sqe send_sqe;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            ops,
            buffers) != 0) {
        perror("configure encoding segment");
        return 1;
    }
    memset(&recv_sqe, 0, sizeof(recv_sqe));
    memset(&send_sqe, 0, sizeof(send_sqe));
    llam_linux_native_segment_prepare_sqe(
        &segment, 0U, &recv_sqe);
    llam_linux_native_segment_prepare_sqe(
        &segment, 1U, &send_sqe);

    if (recv_sqe.opcode != IORING_OP_RECV ||
        recv_sqe.fd != ops[0].fd ||
        recv_sqe.addr != (uint64_t)(uintptr_t)ops[0].buffer ||
        recv_sqe.len != ops[0].length ||
        recv_sqe.msg_flags != 0U ||
        send_sqe.opcode != IORING_OP_SEND ||
        send_sqe.fd != ops[1].fd ||
        send_sqe.addr != (uint64_t)(uintptr_t)ops[1].buffer ||
        send_sqe.len != ops[1].length ||
        send_sqe.msg_flags != (uint32_t)MSG_NOSIGNAL) {
        fprintf(stderr, "native SQE operation fields mismatch\n");
        return 1;
    }
    return 0;
}

static int test_encodes_link_and_skip_flags(void) {
    llam_linux_native_segment_t link;
    llam_linux_native_segment_t skip;
    llam_linux_native_op_t link_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_op_t skip_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char link_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned char skip_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqe;
    unsigned i;

    if (configure_segment(
            &link,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            link_ops,
            link_buffers) != 0 ||
        configure_segment(
            &skip,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            skip_ops,
            skip_buffers) != 0) {
        perror("configure flag segment");
        return 1;
    }

    for (i = 0U; i < 4U; i += 1U) {
        unsigned expected_link =
            i == 3U ? 0U : IOSQE_IO_LINK;
        unsigned expected_skip =
            i == 3U
                ? 0U
                : IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;

        memset(&sqe, 0, sizeof(sqe));
        llam_linux_native_segment_prepare_sqe(
            &link, i, &sqe);
        if (sqe.flags != expected_link) {
            fprintf(
                stderr,
                "link flags mismatch index=%u actual=%u\n",
                i,
                (unsigned)sqe.flags);
            return 1;
        }
        memset(&sqe, 0, sizeof(sqe));
        llam_linux_native_segment_prepare_sqe(
            &skip, i, &sqe);
        if (sqe.flags != expected_skip) {
            fprintf(
                stderr,
                "skip flags mismatch index=%u actual=%u\n",
                i,
                (unsigned)sqe.flags);
            return 1;
        }
    }
    return 0;
}

static int test_encodes_aligned_generation_token(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqe;
    llam_linux_native_token_t *decoded;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure token segment");
        return 1;
    }
    memset(&sqe, 0, sizeof(sqe));
    llam_linux_native_segment_prepare_sqe(
        &segment, 0U, &sqe);
    decoded = llam_io_udata_ptr(sqe.user_data);

    if (llam_io_udata_tag(sqe.user_data) !=
            LLAM_IO_UDATA_NATIVE_SEGMENT ||
        decoded != &segment.tokens[0] ||
        decoded->owner != &segment ||
        decoded->generation != UINT64_C(42) ||
        decoded->operation_index != 0U ||
        ((uintptr_t)decoded & (uintptr_t)7U) != 0U) {
        fprintf(stderr, "native SQE token mismatch\n");
        return 1;
    }
    return 0;
}

static int test_link_waits_for_final_cqe(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;
    unsigned i;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            ops,
            buffers) != 0) {
        perror("configure link completion segment");
        return 1;
    }
    for (i = 0U; i < 3U; i += 1U) {
        if (llam_linux_native_segment_apply_cqe(
                &segment,
                &segment.tokens[i],
                (int)ops[i].length,
                &terminal_result) !=
                LLAM_LINUX_NATIVE_CQE_CONTINUE) {
            fprintf(stderr, "link completed before final CQE\n");
            return 1;
        }
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            (int)ops[3].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_COMPLETE_OK ||
        terminal_result != (int)ops[3].length ||
        segment.observed_cqes != 4U ||
        atomic_load_explicit(
            &segment.state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_TERMINAL) {
        fprintf(stderr, "link final completion mismatch\n");
        return 1;
    }
    return 0;
}

static int test_link_preserves_first_non_cancel_error(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    static const int results[4] = {
        16,
        -EPIPE,
        -ECANCELED,
        -ECANCELED,
    };
    int terminal_result = INT_MIN;
    unsigned i;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            ops,
            buffers) != 0) {
        perror("configure link error segment");
        return 1;
    }
    for (i = 0U; i < 4U; i += 1U) {
        llam_linux_native_cqe_action_t action =
            llam_linux_native_segment_apply_cqe(
                &segment,
                &segment.tokens[i],
                results[i],
                &terminal_result);

        if (i < 3U &&
            action != LLAM_LINUX_NATIVE_CQE_CONTINUE) {
            fprintf(stderr, "link error completed out of order\n");
            return 1;
        }
        if (i == 3U &&
            action != LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR) {
            fprintf(stderr, "link error did not complete at tail\n");
            return 1;
        }
    }
    if (terminal_result != -EPIPE ||
        segment.first_error_index != 1U ||
        segment.first_error != EPIPE) {
        fprintf(stderr, "link lost first non-cancel error\n");
        return 1;
    }
    return 0;
}

static int test_skip_success_completes_on_final_cqe(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure skip success segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            (int)ops[3].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_COMPLETE_OK ||
        terminal_result != (int)ops[3].length ||
        segment.observed_cqes != 1U ||
        segment.suppressed_success_cqes != 3U) {
        fprintf(stderr, "skip success accounting mismatch\n");
        return 1;
    }
    return 0;
}

static int test_skip_intermediate_failure_is_terminal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure skip failure segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            -ECONNRESET,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR ||
        terminal_result != -ECONNRESET ||
        segment.first_error_index != 1U ||
        segment.suppressed_success_cqes != 1U ||
        segment.observed_cqes != 1U) {
        fprintf(stderr, "skip intermediate failure mismatch\n");
        return 1;
    }
    return 0;
}

static int test_short_success_becomes_emsgsize(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure short success segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[2],
            (int)ops[2].length - 1,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR ||
        terminal_result != -EMSGSIZE ||
        segment.first_error_index != 2U ||
        segment.first_error != EMSGSIZE ||
        segment.suppressed_success_cqes != 2U) {
        fprintf(stderr, "short exact result was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_stale_generation_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_token_t stale;
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure stale token segment");
        return 1;
    }
    stale = segment.tokens[0];
    stale.generation = UINT64_C(41);
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &stale,
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 0U ||
        atomic_load_explicit(
            &segment.terminal_claimed,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "stale generation was not failed closed\n");
        return 1;
    }
    return 0;
}

static int test_foreign_owner_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_segment_t foreign;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_token_t token;
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure foreign token segment");
        return 1;
    }
    memset(&foreign, 0, sizeof(foreign));
    token = segment.tokens[0];
    token.owner = &foreign;
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &token,
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 0U) {
        fprintf(stderr, "foreign token owner was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_duplicate_terminal_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            1U,
            ops,
            buffers) != 0) {
        perror("configure duplicate terminal segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            (int)ops[0].length,
            &terminal_result) !=
        LLAM_LINUX_NATIVE_CQE_COMPLETE_OK) {
        fprintf(stderr, "first terminal CQE did not complete\n");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 1U) {
        fprintf(stderr, "duplicate terminal CQE was not rejected\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static const test_case_t tests[] = {
        {"validate configuration",
         test_validates_configuration},
        {"encode recv and send fields",
         test_encodes_recv_and_send_fields},
        {"encode link and skip flags",
         test_encodes_link_and_skip_flags},
        {"encode aligned generation token",
         test_encodes_aligned_generation_token},
        {"link waits for final CQE",
         test_link_waits_for_final_cqe},
        {"link preserves first non-cancel error",
         test_link_preserves_first_non_cancel_error},
        {"skip success completes on final CQE",
         test_skip_success_completes_on_final_cqe},
        {"skip intermediate failure is terminal",
         test_skip_intermediate_failure_is_terminal},
        {"short success becomes EMSGSIZE",
         test_short_success_becomes_emsgsize},
        {"stale generation is fatal",
         test_stale_generation_is_fatal},
        {"foreign owner is fatal",
         test_foreign_owner_is_fatal},
        {"duplicate terminal is fatal",
         test_duplicate_terminal_is_fatal},
    };
    size_t i;

    if (argc != 1 &&
        !(argc == 2 && strcmp(argv[1], "--unit-only") == 0)) {
        fputs("usage: test_leir_native_linux [--unit-only]\n", stderr);
        return 2;
    }
    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i += 1U) {
        if (tests[i].run() != 0) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return 1;
        }
    }
    puts("LEIR native Linux unit tests passed");
    return 0;
}

#endif
