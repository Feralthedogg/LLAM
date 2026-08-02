// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file tests/test_norm_queue_wrap.c
 * @brief Counter-wrap regression tests for the Chase-Lev normal queue.
 */

#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct norm_queue_fixture {
    llam_runtime_t runtime;
    llam_shard_t shard;
    llam_cldeque_t deque;
} norm_queue_fixture_t;

static int fail(const char *message) {
    fprintf(stderr, "[test_norm_queue_wrap] %s\n", message);
    return 1;
}

static void fixture_init(
    norm_queue_fixture_t *fixture,
    size_t logical_index) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->runtime.experimental_lockfree_normq = 1U;
    fixture->runtime.active_shards = 1U;
    fixture->runtime.shards = &fixture->shard;
    fixture->shard.runtime = &fixture->runtime;
    fixture->shard.norm_cldeque = &fixture->deque;
    atomic_init(&fixture->runtime.fatal_errno, 0);
    atomic_init(&fixture->shard.norm_depth, 0U);
    llam_cldeque_init(&fixture->deque);
    atomic_store_explicit(
        &fixture->deque.top, logical_index, memory_order_relaxed);
    atomic_store_explicit(
        &fixture->deque.bottom, logical_index, memory_order_relaxed);
}

static void task_init(llam_task_t *task, llam_runtime_t *runtime) {
    memset(task, 0, sizeof(*task));
    task->owner_runtime = runtime;
}

static int test_owner_pop_crosses_counter_wrap(void) {
    norm_queue_fixture_t fixture;
    llam_task_t first;
    llam_task_t second;

    fixture_init(&fixture, SIZE_MAX);
    task_init(&first, &fixture.runtime);
    task_init(&second, &fixture.runtime);
    if (!llam_norm_queue_push_owner_locked(&fixture.shard, &first) ||
        !llam_norm_queue_push_owner_locked(&fixture.shard, &second)) {
        return fail("owner setup push failed");
    }
    if (atomic_load_explicit(
            &fixture.deque.bottom, memory_order_acquire) != 1U) {
        return fail("owner setup did not cross SIZE_MAX");
    }
    if (llam_norm_queue_pop_owner_locked(&fixture.shard) != &second ||
        llam_norm_queue_pop_owner_locked(&fixture.shard) != &first ||
        llam_norm_queue_pop_owner_locked(&fixture.shard) != NULL) {
        return fail("owner pop lost wrap-crossing tasks");
    }
    if (atomic_load_explicit(
            &fixture.shard.norm_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.deque.top, memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.deque.bottom, memory_order_acquire) != 0U) {
        return fail("owner pop did not restore an empty wrapped deque");
    }
    return 0;
}

static int test_thief_crosses_counter_wrap(void) {
    norm_queue_fixture_t fixture;
    llam_task_t first;
    llam_task_t second;

    fixture_init(&fixture, SIZE_MAX);
    task_init(&first, &fixture.runtime);
    task_init(&second, &fixture.runtime);
    if (!llam_norm_queue_push_owner_locked(&fixture.shard, &first) ||
        !llam_norm_queue_push_owner_locked(&fixture.shard, &second)) {
        return fail("thief setup push failed");
    }
    if (llam_norm_queue_steal(&fixture.shard) != &first ||
        llam_norm_queue_steal(&fixture.shard) != &second ||
        llam_norm_queue_steal(&fixture.shard) != NULL) {
        return fail("thief lost wrap-crossing tasks");
    }
    if (atomic_load_explicit(
            &fixture.shard.norm_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.deque.top, memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.deque.bottom, memory_order_acquire) != 1U) {
        return fail("thief did not restore an empty wrapped deque");
    }
    return 0;
}

static int test_direct_handoff_sees_wrapped_owner_work(void) {
    norm_queue_fixture_t fixture;
    llam_task_t owner_task;
    llam_task_t fifo_task;
    llam_task_t current;
    llam_task_t *next = NULL;
    bool push_failed = false;

    fixture_init(&fixture, SIZE_MAX);
    task_init(&owner_task, &fixture.runtime);
    task_init(&fifo_task, &fixture.runtime);
    task_init(&current, &fixture.runtime);
    if (!llam_norm_queue_push_owner_unlocked(
            &fixture.shard, &owner_task) ||
        !llam_norm_queue_push_yield_unlocked(
            &fixture.shard, &fifo_task)) {
        return fail("direct handoff setup push failed");
    }
    fixture.shard.direct_handoff_streak = 8U;
    if (!llam_norm_queue_exchange_yield_unlocked(
            &fixture.shard,
            &current,
            &next,
            &push_failed) ||
        push_failed || next != &owner_task) {
        return fail("direct handoff misclassified wrapped owner work");
    }
    if (llam_norm_queue_pop_owner_unlocked(&fixture.shard) != &fifo_task ||
        llam_norm_queue_pop_owner_unlocked(&fixture.shard) != &current ||
        llam_norm_queue_pop_owner_unlocked(&fixture.shard) != NULL ||
        atomic_load_explicit(
            &fixture.shard.norm_depth, memory_order_acquire) != 0U) {
        return fail("direct handoff wrap cleanup was inconsistent");
    }
    return 0;
}

int main(void) {
    if (test_owner_pop_crosses_counter_wrap() != 0 ||
        test_thief_crosses_counter_wrap() != 0 ||
        test_direct_handoff_sees_wrapped_owner_work() != 0) {
        return 1;
    }
    puts("test_norm_queue_wrap ok");
    return 0;
}
