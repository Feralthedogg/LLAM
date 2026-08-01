// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#include "leir_native_test_fixture.h"

#if !defined(__linux__)

typedef int leir_native_test_fixture_non_linux_translation_unit_t;

#else

#include <errno.h>
#include <limits.h>
#include <string.h>

void fill_operations(
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32],
    unsigned count) {
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
        ops[i].kind = LLAM_LINUX_NATIVE_OP_SEND;
        ops[i].result_slot = (uint16_t)(4U + i);
        ops[i].fd = (llam_fd_t)(10 + (int)i);
        ops[i].buffer = buffers[i];
        ops[i].length = (uint32_t)(16U + i);
    }
}

bool consume_test_completion(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    unsigned *completion_count = context;

    (void)node;
    (void)req;
    (void)completion_owner;
    (void)wake_reason;
    if (completion_count != NULL) {
        *completion_count += 1U;
    }
    return true;
}

int queue_fixture_init(
    queue_fixture_t *fixture,
    llam_linux_native_segment_mode_t mode,
    unsigned op_count,
    unsigned *completion_count) {
    int rc;

    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->runtime.fatal_errno, 0);
    fixture->runtime.active_shards = 1U;
    fixture->runtime.shards = &fixture->shard;
    fixture->runtime.active_nodes = 1U;
    fixture->runtime.nodes = &fixture->node;
    fixture->shard.runtime = &fixture->runtime;
    fixture->shard.id = 0U;
    atomic_init(&fixture->shard.inflight_io_waiters, 0U);
    rc = pthread_mutex_init(&fixture->shard.lock, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    fixture->shard_lock_ready = true;

    fixture->node.runtime = &fixture->runtime;
    fixture->node.index = 0U;
    fixture->node.event_fd = -1;
    fixture->node.linux_ring_features = IORING_FEAT_CQE_SKIP;
    fixture->node.linux_submit_all = true;
    atomic_init(&fixture->node.event_pending, 0U);
    atomic_init(&fixture->node.pending_ops, 0U);
    rc = pthread_mutex_init(&fixture->node.submit_lock, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->shard_lock_ready = false;
        return -1;
    }
    fixture->submit_lock_ready = true;

    llam_io_req_reset(
        &fixture->req, &fixture->runtime, 0U, UINT_MAX);
    atomic_store_explicit(
        &fixture->req.wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    fixture->req.completion_sink = consume_test_completion;
    fixture->req.completion_sink_context = completion_count;

    fill_operations(fixture->ops, fixture->buffers, op_count);
    if (llam_linux_native_segment_configure(
            &fixture->segment,
            fixture->ops,
            op_count,
            mode) != 0) {
        pthread_mutex_destroy(&fixture->node.submit_lock);
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->submit_lock_ready = false;
        fixture->shard_lock_ready = false;
        return -1;
    }
    fixture->segment.owner_runtime = &fixture->runtime;
    fixture->segment.generation = UINT64_C(1);
    fixture->batch.owner_runtime = &fixture->runtime;
    fixture->batch.segments[0] = &fixture->segment;
    fixture->batch.segment_count = 1U;
    atomic_init(
        &fixture->batch.state,
        LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&fixture->batch.terminal_claimed, 0U);
    atomic_init(
        &fixture->batch.cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&fixture->batch.cancel_requested, 0U);
    return 0;
}

void queue_fixture_destroy(queue_fixture_t *fixture) {
    if (fixture->submit_lock_ready) {
        pthread_mutex_destroy(&fixture->node.submit_lock);
        fixture->submit_lock_ready = false;
    }
    if (fixture->shard_lock_ready) {
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->shard_lock_ready = false;
    }
}

void initialize_batch(
    llam_linux_native_batch_t *batch,
    llam_runtime_t *runtime,
    llam_linux_native_segment_t *first,
    llam_linux_native_segment_t *second,
    unsigned count) {
    memset(batch, 0, sizeof(*batch));
    batch->owner_runtime = runtime;
    batch->segments[0] = first;
    batch->segments[1] = second;
    batch->segment_count = count;
    atomic_init(&batch->state, LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&batch->terminal_claimed, 0U);
    atomic_init(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&batch->cancel_requested, 0U);
}

void init_userspace_sq_fixture(
    struct io_uring *ring,
    struct io_uring_sqe *sqes,
    unsigned *head,
    unsigned entries) {
    memset(ring, 0, sizeof(*ring));
    memset(sqes, 0, entries * sizeof(sqes[0]));
    *head = 0U;
    ring->ring_fd = -1;
    ring->sq.khead = head;
    ring->sq.sqes = sqes;
    ring->sq.ring_entries = entries;
    ring->sq.ring_mask = entries - 1U;
}

#endif
