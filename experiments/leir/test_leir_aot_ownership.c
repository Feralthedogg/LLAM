// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/test_leir_aot_ownership.c
 * @brief Independent terminal-ticket ownership tests for Linux AOT segments.
 */

#include <stdio.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR AOT ownership tests require Linux");
    return 0;
}

#else

#include "leir_native_test_fixture.h"

#include "io/runtime_io_api_internal.h"

#if !defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_io_test_abort_published_io_setup(
    llam_io_req_t *req,
    llam_io_abort_reason_t reason,
    bool *wait_for_completion);
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

static int test_independent_tickets_wake_out_of_order(void) {
    queue_fixture_t fixture;
    llam_io_req_t second_request;
    llam_linux_native_segment_t second_segment;
    llam_linux_native_batch_t second_batch;
    llam_linux_native_op_t
        second_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        second_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqes[4];
    llam_linux_native_batch_t *taken;
    unsigned first_completions = 0U;
    unsigned second_completions = 0U;
    unsigned sq_head;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            1U,
            &first_completions) != 0) {
        perror("independent ticket fixture init");
        return 1;
    }
    llam_io_req_reset(
        &second_request, &fixture.runtime, 0U, UINT_MAX);
    atomic_store_explicit(
        &second_request.wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    second_request.completion_sink = consume_test_completion;
    second_request.completion_sink_context = &second_completions;
    fill_operations(second_ops, second_buffers, 1U);
    if (llam_linux_native_segment_configure(
            &second_segment,
            second_ops,
            1U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) != 0) {
        perror("independent second segment configure");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    second_segment.owner_runtime = &fixture.runtime;
    second_segment.generation = UINT64_C(2);
    initialize_batch(
        &second_batch,
        &fixture.runtime,
        &second_segment,
        NULL,
        1U);
    init_userspace_sq_fixture(
        &fixture.node.ring, sqes, &sq_head, 4U);
    fixture.node.ring_ready = true;

    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        !llam_linux_native_batch_enqueue(
            &fixture.node,
            &second_batch,
            &second_request)) {
        perror("independent ticket enqueue");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &fixture.batch ||
        taken->next != &second_batch ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken) != 1U ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken->next) != 1U) {
        fprintf(stderr, "independent ticket drain mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }

    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &second_segment.tokens[0],
        (int)second_segment.ops[0].length);
    if (first_completions != 0U ||
        second_completions != 1U ||
        second_segment.terminal_wakes != 1U ||
        fixture.segment.terminal_wakes != 0U ||
        second_segment.batch != NULL ||
        fixture.segment.batch != &fixture.batch ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "second independent owner did not wake first\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }

    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[0],
        (int)fixture.segment.ops[0].length);
    if (first_completions != 1U ||
        second_completions != 1U ||
        fixture.segment.terminal_wakes != 1U ||
        fixture.segment.batch != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "first independent owner retirement mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_setup_abort_detaches_queued_batch(void) {
    queue_fixture_t fixture;
    bool wait_for_completion = true;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            1U,
            &completions) != 0) {
        perror("queued setup-abort fixture init");
        return 1;
    }
    if (!llam_linux_native_batch_enqueue(
            &fixture.node, &fixture.batch, &fixture.req) ||
        !llam_io_test_abort_published_io_setup(
            &fixture.req, LLAM_IO_ABORT_CANCEL,
            &wait_for_completion) ||
        wait_for_completion ||
        fixture.req.result != -1 ||
        fixture.req.error_code != ECANCELED ||
        atomic_load_explicit(
            &fixture.req.wait_mode,
            memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE ||
        atomic_load_explicit(
            &fixture.req.linux_native_batch,
            memory_order_acquire) != NULL ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
                LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_RETIRED ||
        fixture.segment.first_error != ECANCELED ||
        fixture.segment.first_error_index != UINT_MAX ||
        fixture.segment.semantic_result != -(int64_t)ECANCELED ||
        fixture.segment.batch != NULL ||
        fixture.node.native_batch_head != NULL ||
        fixture.node.native_batch_tail != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        completions != 0U) {
        fputs("queued native setup abort did not retire ownership\n",
              stderr);
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_setup_abort_requests_inflight_native_cancel(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *taken;
    bool wait_for_completion = false;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            1U,
            &completions) != 0) {
        perror("inflight setup-abort fixture init");
        return 1;
    }
    if (!llam_linux_native_batch_enqueue(
            &fixture.node, &fixture.batch, &fixture.req)) {
        perror("inflight setup-abort enqueue");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &fixture.batch || taken->next != NULL ||
        !llam_io_test_abort_published_io_setup(
            &fixture.req, LLAM_IO_ABORT_CANCEL,
            &wait_for_completion) ||
        !wait_for_completion ||
        atomic_load_explicit(
            &fixture.req.abort_reason,
            memory_order_acquire) != LLAM_IO_ABORT_CANCEL ||
        atomic_load_explicit(
            &fixture.req.wait_mode,
            memory_order_acquire) != LLAM_IO_WAIT_MODE_INFLIGHT ||
        atomic_load_explicit(
            &fixture.req.linux_native_batch,
            memory_order_acquire) != &fixture.batch ||
        atomic_load_explicit(
            &fixture.batch.cancel_requested,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.batch.cancel_state,
            memory_order_acquire) !=
                LLAM_LINUX_NATIVE_CANCEL_QUEUED ||
        fixture.node.native_cancel_head != &fixture.batch ||
        fixture.node.native_cancel_tail != &fixture.batch ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U ||
        completions != 0U) {
        fputs("inflight native setup abort did not retain ownership\n",
              stderr);
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

int main(void) {
    if (test_independent_tickets_wake_out_of_order() != 0) {
        fputs("FAIL: independent tickets wake out of order\n", stderr);
        return 1;
    }
    if (test_setup_abort_detaches_queued_batch() != 0) {
        fputs("FAIL: queued native setup abort\n", stderr);
        return 1;
    }
    if (test_setup_abort_requests_inflight_native_cancel() != 0) {
        fputs("FAIL: inflight native setup abort\n", stderr);
        return 1;
    }
    puts("LEIR AOT ownership tests passed");
    return 0;
}

#endif
