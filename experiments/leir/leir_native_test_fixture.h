// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_NATIVE_TEST_FIXTURE_H
#define LLAM_EXPERIMENTS_LEIR_NATIVE_TEST_FIXTURE_H

#if defined(__linux__)

#include "io/linux/runtime_io_segment_linux_internal.h"

typedef struct queue_fixture {
    llam_runtime_t runtime;
    llam_runtime_t foreign_runtime;
    llam_shard_t shard;
    llam_node_t node;
    llam_io_req_t req;
    llam_linux_native_segment_t segment;
    llam_linux_native_batch_t batch;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    bool shard_lock_ready;
    bool submit_lock_ready;
} queue_fixture_t;

void fill_operations(
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32],
    unsigned count);
bool consume_test_completion(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context);
int queue_fixture_init(
    queue_fixture_t *fixture,
    llam_linux_native_segment_mode_t mode,
    unsigned op_count,
    unsigned *completion_count);
void queue_fixture_destroy(queue_fixture_t *fixture);
void initialize_batch(
    llam_linux_native_batch_t *batch,
    llam_runtime_t *runtime,
    llam_linux_native_segment_t *first,
    llam_linux_native_segment_t *second,
    unsigned count);
void init_userspace_sq_fixture(
    struct io_uring *ring,
    struct io_uring_sqe *sqes,
    unsigned *head,
    unsigned entries);

#endif

#endif
