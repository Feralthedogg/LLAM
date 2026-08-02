// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/test_leir_aot_linux_unit.c
 * @brief Linux SQE encoding and CQE reduction tests for LEIR AOT segments.
 */

#include <stdio.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR AOT Linux unit tests require Linux");
    return 0;
}

#else

#include "leir_aot_connect_bench_support.h"
#include "io/linux/runtime_io_segment_linux_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static void fill_connect_write_operations(
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    struct sockaddr_in *address,
    unsigned char *payload,
    uint32_t payload_length) {
    memset(
        ops,
        0,
        LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS * sizeof(ops[0]));
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(9U);
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ops[0].kind = LLAM_LINUX_NATIVE_OP_CONNECT;
    ops[0].result_slot = 3U;
    ops[0].fd = 10;
    ops[0].buffer = address;
    ops[0].length = (uint32_t)sizeof(*address);
    ops[1].kind = LLAM_LINUX_NATIVE_OP_SEND;
    ops[1].result_slot = 6U;
    ops[1].flags = LLAM_LINUX_NATIVE_OP_PARTIAL_OK;
    ops[1].fd = 10;
    ops[1].buffer = payload;
    ops[1].length = payload_length;
}

static int test_encodes_connect_and_partial_write_fields(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    struct sockaddr_in address;
    unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};
    struct io_uring_sqe connect_sqe;
    struct io_uring_sqe write_sqe;

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("configure connect-write segment");
        return 1;
    }
    memset(&connect_sqe, 0, sizeof(connect_sqe));
    memset(&write_sqe, 0, sizeof(write_sqe));
    llam_linux_native_segment_prepare_sqe(
        &segment, 0U, &connect_sqe);
    llam_linux_native_segment_prepare_sqe(
        &segment, 1U, &write_sqe);

    if (connect_sqe.opcode != IORING_OP_CONNECT ||
        connect_sqe.fd != ops[0].fd ||
        connect_sqe.addr !=
            (uint64_t)(uintptr_t)&address ||
        connect_sqe.off != sizeof(address) ||
        connect_sqe.len != 0U ||
        (connect_sqe.flags & IOSQE_IO_LINK) == 0U ||
        (connect_sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0U ||
        write_sqe.opcode != IORING_OP_SEND ||
        write_sqe.fd != ops[1].fd ||
        write_sqe.addr !=
            (uint64_t)(uintptr_t)payload ||
        write_sqe.len != sizeof(payload) ||
        write_sqe.msg_flags != (uint32_t)MSG_NOSIGNAL ||
        write_sqe.flags != 0U) {
        fprintf(stderr, "connect-write SQE fields mismatch\n");
        return 1;
    }
    return 0;
}

static int test_rejects_malformed_connect_write_shapes(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    struct sockaddr_in address;
    unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    ops[0].length = 0U;
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "zero-length connect address was accepted\n");
        return 1;
    }

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    ops[0].length = (uint32_t)sizeof(struct sockaddr_storage) + 1U;
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "oversized connect address was accepted\n");
        return 1;
    }

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    ops[1].fd += 1;
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) == 0 ||
        errno != ENOTSUP) {
        fprintf(stderr, "cross-descriptor connect-write was accepted\n");
        return 1;
    }

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    ops[1].flags = 0U;
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) == 0 ||
        errno != ENOTSUP) {
        fprintf(stderr, "exact connect-write shape was accepted\n");
        return 1;
    }

    fill_connect_write_operations(
        ops, &address, payload, 0U);
    ops[1].buffer = NULL;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("zero-length ordinary write was rejected");
        return 1;
    }
    return 0;
}

static int test_connect_write_partial_success_is_visible(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    struct sockaddr_in address;
    unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};
    int terminal_result = INT_MIN;

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("configure partial connect-write segment");
        return 1;
    }
    segment.generation = UINT64_C(42);
    segment.tokens[0].generation = segment.generation;
    segment.tokens[1].generation = segment.generation;
    segment.atomic_submission = true;
    atomic_store_explicit(
        &segment.state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);

    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            3,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_OK ||
        terminal_result != 3 ||
        segment.first_error != 0 ||
        segment.suppressed_success_cqes != 1U) {
        fprintf(stderr, "partial ordinary write was not preserved\n");
        return 1;
    }
    return 0;
}

static int test_connect_failure_retires_omitted_write(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    struct sockaddr_in address;
    unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};
    int terminal_result = INT_MIN;

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("configure failing connect-write segment");
        return 1;
    }
    segment.generation = UINT64_C(42);
    segment.tokens[0].generation = segment.generation;
    segment.tokens[1].generation = segment.generation;
    segment.atomic_submission = true;
    atomic_store_explicit(
        &segment.state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);

    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            -ECONNREFUSED,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -ECONNREFUSED ||
        segment.first_error != ECONNREFUSED ||
        segment.first_error_index != 0U ||
        segment.observed_operation_mask != UINT64_C(1)) {
        fprintf(stderr, "connect failure retirement mismatch\n");
        return 1;
    }
    return 0;
}

static int test_connect_write_rejects_impossible_results(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    struct sockaddr_in address;
    unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};
    int terminal_result = INT_MIN;

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("configure impossible-result segment");
        return 1;
    }
    segment.generation = UINT64_C(42);
    segment.tokens[0].generation = segment.generation;
    segment.tokens[1].generation = segment.generation;
    segment.atomic_submission = true;
    atomic_store_explicit(
        &segment.state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            1,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -EPROTO) {
        fprintf(stderr, "positive connect result was not rejected\n");
        return 1;
    }

    fill_connect_write_operations(
        ops, &address, payload, (uint32_t)sizeof(payload));
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("reconfigure impossible-result segment");
        return 1;
    }
    segment.generation = UINT64_C(43);
    segment.tokens[0].generation = segment.generation;
    segment.tokens[1].generation = segment.generation;
    segment.atomic_submission = true;
    atomic_store_explicit(
        &segment.state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            0,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -EIO) {
        fprintf(stderr, "zero nonempty write was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_ring_profile_option_parsing(void) {
    static const char *const profiles[] = {
        "submit_all",
        "coop_taskrun",
        "defer_taskrun",
    };
    size_t i;

    for (i = 0U; i < sizeof(profiles) / sizeof(profiles[0]); i += 1U) {
        char *arguments[] = {
            "bench_leir_aot_connect",
            "--candidate",
            "linux",
            "--process",
            "linux",
            "--ring-profile",
            (char *)profiles[i],
            "--transport",
            "tcp",
            "--block",
            "0",
            "--order",
            "1",
            "--seed",
            "42",
        };
        bench_options_t options;

        if (parse_options(
                (int)(sizeof(arguments) / sizeof(arguments[0])),
                arguments,
                &options) != 0 ||
            options.ring_profile == NULL ||
            strcmp(options.ring_profile, profiles[i]) != 0) {
            fprintf(
                stderr,
                "ring profile parse failed for %s\n",
                profiles[i]);
            return 1;
        }
    }
    {
        char *arguments[] = {
            "bench_leir_aot_connect",
            "--candidate",
            "oracle",
            "--process",
            "portable",
            "--ring-profile",
            "portable_control",
            "--transport",
            "unix",
            "--block",
            "3",
            "--order",
            "0",
            "--seed",
            "0",
        };
        bench_options_t options;

        if (parse_options(
                (int)(sizeof(arguments) / sizeof(arguments[0])),
                arguments,
                &options) != 0 ||
            options.candidate != BENCH_CANDIDATE_ORACLE ||
            options.process != BENCH_PROCESS_PORTABLE ||
            options.transport != BENCH_TRANSPORT_UNIX ||
            strcmp(options.ring_profile, "portable_control") != 0 ||
            options.block != 3U || options.order != 0U ||
            options.seed != 0U) {
            fputs("portable evidence identity parse failed\n", stderr);
            return 1;
        }
    }
    {
        char *arguments[] = {
            "bench_leir_aot_connect",
            "--candidate",
            "oracle",
            "--process",
            "portable",
            "--ring-profile",
        };
        bench_options_t options;

        errno = 0;
        if (parse_options(
                (int)(sizeof(arguments) / sizeof(arguments[0])),
                arguments,
                &options) == 0 ||
            errno != EINVAL) {
            fputs("missing ring profile value was accepted\n", stderr);
            return 1;
        }
    }
    {
        char *arguments[] = {
            "bench_leir_aot_connect",
            "--candidate",
            "portable",
            "--process",
            "linux",
            "--ring-profile",
            "unknown",
            "--transport",
            "tcp",
            "--block",
            "0",
            "--order",
            "1",
            "--seed",
            "9",
        };
        bench_options_t options;

        errno = 0;
        if (parse_options(
                (int)(sizeof(arguments) / sizeof(arguments[0])),
                arguments,
                &options) == 0 ||
            errno != EINVAL) {
            fputs("unknown ring profile was accepted\n", stderr);
            return 1;
        }
    }
    {
        char *arguments[] = {
            "bench_leir_aot_connect",
            "--candidate",
            "linux",
            "--process",
            "portable",
            "--ring-profile",
            "portable_control",
            "--transport",
            "tcp",
            "--block",
            "0",
            "--order",
            "0",
            "--seed",
            "9",
        };
        bench_options_t options;

        errno = 0;
        if (parse_options(
                (int)(sizeof(arguments) / sizeof(arguments[0])),
                arguments,
                &options) == 0 ||
            errno != EINVAL) {
            fputs("cross-axis candidate was accepted\n", stderr);
            return 1;
        }
    }
    return 0;
}

static int test_seeded_receipts_are_deterministic(void) {
    unsigned char first[32];
    unsigned char same[32];
    unsigned char different[32];

    fill_payload(first, sizeof(first), 7U, 11U);
    fill_payload(same, sizeof(same), 7U, 11U);
    fill_payload(different, sizeof(different), 7U, 12U);
    if (memcmp(first, same, sizeof(first)) != 0 ||
        memcmp(first, different, sizeof(first)) == 0 ||
        bench_result_receipt(7U, 0, 32) !=
            bench_result_receipt(7U, 0, 32) ||
        bench_result_receipt(7U, 0, 32) ==
            bench_result_receipt(8U, 0, 32)) {
        fputs("seeded benchmark receipts are not deterministic\n", stderr);
        return 1;
    }
    return 0;
}

static int test_benchmark_timing_invariants(void) {
    bench_metrics_t metrics;

    memset(&metrics, 0, sizeof(metrics));
    metrics.bind_ns = 5U;
    metrics.execute_ns = 30U;
    if (!bench_metrics_timing_is_valid(
            BENCH_CANDIDATE_ORACLE, &metrics)) {
        fputs("oracle zero AOT timing was rejected\n", stderr);
        return 1;
    }
    metrics.aot_prepare_ns = 1U;
    if (bench_metrics_timing_is_valid(
            BENCH_CANDIDATE_ORACLE, &metrics)) {
        fputs("oracle nonzero AOT timing was accepted\n", stderr);
        return 1;
    }

    metrics.aot_prepare_ns = 5U;
    metrics.aot_ring_ns = 20U;
    metrics.aot_resume_ns = 5U;
    if (!bench_metrics_timing_is_valid(
            BENCH_CANDIDATE_LINUX, &metrics)) {
        fputs("Linux timing decomposition was rejected\n", stderr);
        return 1;
    }
    metrics.aot_resume_ns = 6U;
    if (bench_metrics_timing_is_valid(
            BENCH_CANDIDATE_LINUX, &metrics)) {
        fputs("oversized Linux timing decomposition was accepted\n", stderr);
        return 1;
    }
    metrics.aot_prepare_ns = UINT64_MAX;
    metrics.aot_ring_ns = UINT64_MAX;
    metrics.aot_resume_ns = UINT64_MAX;
    metrics.execute_ns = UINT64_MAX;
    if (bench_metrics_timing_is_valid(
            BENCH_CANDIDATE_LINUX, &metrics)) {
        fputs("overflowing Linux timing decomposition was accepted\n", stderr);
        return 1;
    }
    return 0;
}

typedef int (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn run;
} test_case_t;

int main(void) {
    static const test_case_t tests[] = {
        {"encode connect and partial write fields",
         test_encodes_connect_and_partial_write_fields},
        {"reject malformed connect-write shapes",
         test_rejects_malformed_connect_write_shapes},
        {"connect-write partial success is visible",
         test_connect_write_partial_success_is_visible},
        {"connect failure retires omitted write",
         test_connect_failure_retires_omitted_write},
        {"connect-write rejects impossible results",
         test_connect_write_rejects_impossible_results},
        {"ring profile option parsing",
         test_ring_profile_option_parsing},
        {"seeded receipts are deterministic",
         test_seeded_receipts_are_deterministic},
        {"benchmark timing invariants",
         test_benchmark_timing_invariants},
    };
    size_t i;

    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i += 1U) {
        if (tests[i].run() != 0) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return 1;
        }
    }
    puts("LEIR AOT Linux unit tests passed");
    return 0;
}

#endif
