/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"
#include "engine/runtime_watchdog_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "test_processor_queue_timer_internal: %s\n", message);
    return -1;
}

static int create_runtime(uint64_t experimental_flags, llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    *out = NULL;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.experimental_flags = experimental_flags;
    return llam_runtime_create_compat(&opts, sizeof(opts), out);
}

static int test_unbound_m_cannot_mutate_owner_deque(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_task_t fake_task;
    llam_task_t *popped = NULL;
    bool accepted;
    int rc = 0;

    if (create_runtime(LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ, &runtime) != 0 ||
        runtime == NULL || runtime->shards == NULL || runtime->active_shards != 1U) {
        llam_runtime_destroy(runtime);
        return fail("could not create owner-deque test runtime");
    }
    shard = &runtime->shards[0];
    memset(&fake_task, 0, sizeof(fake_task));

    /* Runtime initialization has no live M lease and therefore no P ticket. */
    accepted = llam_norm_queue_push_owner_unlocked(shard, &fake_task);
    if (accepted) {
        /* Retire the unexpected RED insertion before reporting the failure. */
        llam_scheduler_bind_identity(shard,
                                     &shard->primary_worker,
                                     &shard->scheduler_ctx);
        popped = llam_norm_queue_pop_owner_unlocked(shard);
        if (popped != &fake_task) {
            rc = fail("could not retire unexpected owner-deque insertion");
        }
        llam_scheduler_unbind_identity(shard, &shard->primary_worker);
        llam_scheduler_clear_identity();
    }
    if (accepted && rc == 0) {
        rc = fail("unbound M mutated the owner-only normal deque without a P ticket");
    }

    llam_runtime_destroy(runtime);
    return rc;
}

static void remote_spawn_task(void *arg) {
    atomic_uint *completed = arg;

    atomic_store_explicit(completed, 1U, memory_order_release);
}

static int test_host_spawn_uses_inject_lane(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_task_t *task;
    atomic_uint completed;
    unsigned inject_depth;
    unsigned hot_depth;
    unsigned normal_depth;
    bool routed_to_inject;
    int run_rc;
    int join_rc = -1;

    atomic_init(&completed, 0U);
    if (create_runtime(0U, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards != 1U) {
        llam_runtime_destroy(runtime);
        return fail("could not create remote-spawn routing runtime");
    }
    shard = &runtime->shards[0];
    task = llam_runtime_spawn_ex(runtime, remote_spawn_task, &completed, NULL, 0U);
    if (task == NULL) {
        llam_runtime_destroy(runtime);
        return fail("host spawn failed before queue routing could be inspected");
    }

    pthread_mutex_lock(&shard->lock);
    inject_depth = shard->processor.inject_q.depth;
    hot_depth = shard->processor.hot_q.depth;
    normal_depth = shard->processor.norm_q.depth;
    pthread_mutex_unlock(&shard->lock);
    routed_to_inject = inject_depth == 1U && hot_depth == 0U && normal_depth == 0U;

    run_rc = llam_runtime_run_handle(runtime);
    if (run_rc == 0) {
        join_rc = llam_join(task);
    }
    llam_runtime_destroy(runtime);
    if (!routed_to_inject) {
        return fail("host spawn bypassed inject and wrote an owner-only runnable lane");
    }
    if (run_rc != 0 || join_rc != 0 ||
        atomic_load_explicit(&completed, memory_order_acquire) == 0U) {
        return fail("inject-routed host task did not complete through the scheduler");
    }
    return 0;
}

static void lock_shard_pair(llam_shard_t *left, llam_shard_t *right) {
    llam_shard_t *first = left->id < right->id ? left : right;
    llam_shard_t *second = first == left ? right : left;

    pthread_mutex_lock(&first->opaque_lock);
    pthread_mutex_lock(&second->opaque_lock);
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
}

static void unlock_shard_pair(llam_shard_t *left, llam_shard_t *right) {
    llam_shard_t *first = left->id < right->id ? left : right;
    llam_shard_t *second = first == left ? right : left;

    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    pthread_mutex_unlock(&second->opaque_lock);
    pthread_mutex_unlock(&first->opaque_lock);
}

static void setup_merge_pause_ack(llam_shard_t *source) {
    uint64_t incarnation = atomic_load_explicit(
        &source->primary_worker.binding_incarnation,
        memory_order_acquire);
    uint64_t epoch = source->legacy_lease.epoch;

    atomic_store_explicit(&source->merge_pause_request_sequence, 1U, memory_order_release);
    atomic_store_explicit(&source->merge_pause_ack_sequence, 1U, memory_order_release);
    atomic_store_explicit(&source->merge_pause_ack_lease_epoch, epoch, memory_order_release);
    atomic_store_explicit(&source->merge_pause_ack_worker_incarnation,
                          incarnation,
                          memory_order_release);
    atomic_store_explicit(&source->merge_pause_ack, 1U, memory_order_release);
    atomic_store_explicit(&source->merge_pause_requested, 1U, memory_order_release);
}

static void initialize_fake_sleep_task(llam_runtime_t *runtime,
                                       llam_shard_t *shard,
                                       llam_task_t *task,
                                       uint64_t id,
                                       uint64_t generation,
                                       uint64_t deadline_ns) {
    memset(task, 0, sizeof(*task));
    task->owner_runtime = runtime;
    task->id = id;
    atomic_init(&task->home_shard, shard->id);
    task->live_shard = shard->id;
    task->deadline_ns = deadline_ns;
    atomic_init(&task->state, LLAM_TASK_STATE_PARKED);
    atomic_init(&task->wait_reason, LLAM_WAIT_SLEEP);
    atomic_init(&task->last_shard, shard->id);
    atomic_init(&task->parked_shard, shard->id);
    atomic_init(&task->active_timer_heap_shard, UINT_MAX);
    atomic_init(&task->task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->base_task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->wait_resolver_state, 0U);
    atomic_init(&task->wait_generation, generation);
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_io_generation, 0U);
    atomic_init(&task->active_block_job, NULL);
    atomic_init(&task->join_target, NULL);
    atomic_init(&task->wake_error_code, 0);
}

typedef struct cancel_timer_rehome_gate {
    llam_task_t *task;
    atomic_bool reached;
    atomic_bool resume;
} cancel_timer_rehome_gate_t;

static cancel_timer_rehome_gate_t *g_cancel_timer_rehome_gate;

static void pause_cancel_after_owner_sample(void) {
    cancel_timer_rehome_gate_t *gate = g_cancel_timer_rehome_gate;

    if (gate == NULL) {
        return;
    }
    atomic_store_explicit(&gate->reached, true, memory_order_release);
    while (!atomic_load_explicit(&gate->resume, memory_order_acquire)) {
        llam_pause_cpu();
    }
}

static void *cancel_test_sleep_thread(void *arg) {
    llam_cancel_task_wait((llam_task_t *)arg);
    return NULL;
}

static bool wait_for_atomic_bool(const atomic_bool *value, uint64_t timeout_ns) {
    uint64_t start_ns = llam_now_ns();

    while (!atomic_load_explicit(value, memory_order_acquire)) {
        if (llam_now_ns() - start_ns >= timeout_ns) {
            return false;
        }
        llam_pause_cpu();
    }
    return true;
}

static int test_timer_rehome_generation_and_ownership(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_shard_t *source;
    llam_shard_t *target;
    llam_task_t task;
    llam_task_t runnable_task;
    llam_timer_node_t timer;
    llam_timer_node_t *removed;
    unsigned migrated = 0U;
    unsigned runnable_migrated = 0U;
    bool stale_rejected;
    bool moved;
    bool offline_ready;
    bool source_offlined = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return fail("timer-rehome options initialization failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL) {
        llam_runtime_destroy(runtime);
        return fail("could not create timer-rehome test runtime");
    }
    if (runtime->active_shards < 2U) {
        fprintf(stderr,
                "test_processor_queue_timer_internal: SKIP timer rehome (one processor)\n");
        llam_runtime_destroy(runtime);
        return 77;
    }
    source = &runtime->shards[0];
    target = &runtime->shards[1];
    llam_scheduler_bind_identity(source, &source->primary_worker, &source->scheduler_ctx);
    memset(&runnable_task, 0, sizeof(runnable_task));
    atomic_init(&runnable_task.state, LLAM_TASK_STATE_RUNNABLE);
    atomic_init(&runnable_task.wait_reason, LLAM_WAIT_NONE);
    atomic_init(&runnable_task.home_shard, source->id);
    atomic_init(&runnable_task.last_shard, source->id);
    runnable_task.owner_runtime = runtime;
    memset(&task, 0, sizeof(task));
    task.owner_runtime = runtime;
    task.id = UINT64_C(0x1234);
    atomic_init(&task.home_shard, source->id);
    atomic_init(&task.state, LLAM_TASK_STATE_PARKED);
    atomic_init(&task.wait_reason, LLAM_WAIT_SLEEP);
    atomic_init(&task.wait_generation, 7U);
    atomic_init(&task.parked_shard, source->id);
    atomic_init(&task.active_timer_heap_shard, source->id);
    atomic_init(&task.wait_resolver_state, 0U);
    atomic_init(&task.active_wait_node, NULL);
    atomic_init(&task.active_wait_queue, NULL);
    atomic_init(&task.active_wait_queue_lock, NULL);
    atomic_init(&task.active_select_state, NULL);
    atomic_init(&task.active_wait_lifetime_ops, NULL);
    memset(&timer, 0, sizeof(timer));
    timer.owner_runtime = runtime;
    timer.task = &task;
    timer.deadline_ns = llam_now_ns() + UINT64_C(1000000000);
    timer.wait_generation = 7U;
    timer.owner_shard = source->id;
    task.deadline_ns = timer.deadline_ns;
    task.active_timer = &timer;

    lock_shard_pair(source, target);
    if (!llam_timer_heap_push_locked(source, &timer)) {
        unlock_shard_pair(source, target);
        fail("could not seed source timer heap");
        goto cleanup;
    }
    if (!llam_queue_push_bounded_locked(source,
                                        &source->processor.inject_q,
                                        LLAM_INJECT_QUEUE_CAP,
                                        &runnable_task,
                                        NULL)) {
        unlock_shard_pair(source, target);
        fail("could not seed source runnable inject queue");
        goto cleanup;
    }
    unlock_shard_pair(source, target);
    pthread_mutex_lock(&source->opaque_lock);
    setup_merge_pause_ack(source);
    pthread_mutex_unlock(&source->opaque_lock);

    atomic_store_explicit(&task.wait_generation, 8U, memory_order_release);
    lock_shard_pair(source, target);
    stale_rejected = !llam_merge_shard_timers_locked(source, target, &migrated) &&
                     migrated == 0U && source->processor.timer_heap_len == 1U &&
                     source->processor.timer_heap[0] == &timer && target->processor.timer_heap_len == 0U &&
                     source->processor.timer_count == 1U && target->processor.timer_count == 0U &&
                     source->processor.inject_q.depth == 1U &&
                     target->processor.inject_q.depth == 0U &&
                     atomic_load_explicit(&task.active_timer_heap_shard,
                                          memory_order_acquire) == source->id &&
                     atomic_load_explicit(&task.parked_shard, memory_order_acquire) ==
                         source->id;
    atomic_store_explicit(&task.wait_generation, 7U, memory_order_release);
    llam_timer_test_fail_next_rehome_reserve();
    moved = llam_merge_shard_timers_locked(source, target, &migrated);
    if (stale_rejected && !moved && migrated == 0U &&
        source->processor.timer_heap_len == 1U &&
        source->processor.timer_heap[0] == &timer &&
        target->processor.timer_heap_len == 0U &&
        source->processor.timer_count == 1U && target->processor.timer_count == 0U &&
        source->processor.inject_q.depth == 1U &&
        target->processor.inject_q.depth == 0U &&
        timer.heap_index == 0U && timer.next == NULL && task.active_timer == &timer &&
        atomic_load_explicit(&task.active_timer_heap_shard,
                             memory_order_acquire) == source->id &&
        atomic_load_explicit(&task.parked_shard, memory_order_acquire) == source->id &&
        atomic_load_explicit(&task.home_shard, memory_order_acquire) == source->id) {
        stale_rejected = true;
    } else {
        stale_rejected = false;
    }
    migrated = 0U;
    moved = llam_merge_shard_timers_locked(source, target, &migrated);
    if (moved) {
        moved = llam_merge_runnable_queues_locked(source,
                                                  target,
                                                  &runnable_migrated);
    }
    offline_ready = moved && runnable_migrated == 1U &&
                    llam_shard_can_offline_locked(source);
    if (offline_ready) {
        unsigned previous;

        llam_processor_set_online(&source->processor, false);
        previous = atomic_fetch_sub_explicit(&runtime->online_shards,
                                             1U,
                                             memory_order_acq_rel);
        if (previous > 0U) {
            llam_runtime_note_online_shards(runtime, previous - 1U);
            source_offlined = !llam_processor_online(&source->processor);
        } else {
            atomic_fetch_add_explicit(&runtime->online_shards,
                                      1U,
                                      memory_order_acq_rel);
        }
    }
    if (stale_rejected && moved && migrated == 1U && source->processor.timer_heap_len == 0U &&
        source->processor.timers == NULL && target->processor.timer_heap_len == 1U &&
        target->processor.timer_heap[0] == &timer && target->processor.timers == &timer &&
        source->processor.timer_count == 0U && target->processor.timer_count == 1U &&
        source->processor.inject_q.depth == 0U &&
        target->processor.inject_q.depth == 1U &&
        atomic_load_explicit(&runnable_task.home_shard, memory_order_acquire) == target->id &&
        offline_ready && source_offlined &&
        timer.owner_shard == source->id && timer.heap_index == 0U &&
        atomic_load_explicit(&task.active_timer_heap_shard,
                             memory_order_acquire) == target->id &&
        atomic_load_explicit(&task.parked_shard, memory_order_acquire) == target->id &&
        atomic_load_explicit(&task.home_shard, memory_order_acquire) == target->id) {
        rc = 0;
    }
    if (target->processor.timer_heap_len != 0U) {
        removed = llam_timer_heap_pop_min_locked(target);
    } else if (source->processor.timer_heap_len != 0U) {
        removed = llam_timer_heap_pop_min_locked(source);
    } else {
        removed = NULL;
    }
    if (target->processor.inject_q.depth > 0U &&
        llam_queue_pop_head(&target->processor.inject_q) != &runnable_task) {
        rc = fail("runnable queue cleanup lost its migrated task");
    }
    task.active_timer = NULL;
    atomic_store_explicit(&task.active_timer_heap_shard,
                          UINT_MAX,
                          memory_order_release);
    if (target->processor.inject_q.depth == 0U) {
        atomic_store_explicit(&target->processor.inject_depth,
                              0U,
                              memory_order_release);
    }
    unlock_shard_pair(source, target);
    if (removed != &timer) {
        rc = fail("timer-rehome fixture cleanup lost its timer node");
    }

cleanup:
    llam_shard_release_merge_pause(source);
    llam_scheduler_unbind_identity(source, &source->primary_worker);
    llam_scheduler_clear_identity();
    llam_runtime_destroy(runtime);
    if (rc != 0) {
        return fail("timer rehome did not preserve generations and single-heap ownership");
    }
    return 0;
}

static int test_cancel_retries_after_timer_rehome(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_shard_t *source;
    llam_shard_t *target;
    llam_task_t task;
    cancel_timer_rehome_gate_t gate;
    pthread_t cancel_thread;
    bool thread_started = false;
    bool rehomed = false;
    unsigned migrated = 0U;
    bool completed;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return fail("cancel/rehome options initialization failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL) {
        llam_runtime_destroy(runtime);
        return fail("could not create cancel/rehome runtime");
    }
    if (runtime->active_shards < 2U) {
        fprintf(stderr,
                "test_processor_queue_timer_internal: SKIP cancel/rehome (one processor)\n");
        llam_runtime_destroy(runtime);
        return 77;
    }
    source = &runtime->shards[0];
    target = &runtime->shards[1];
    llam_scheduler_bind_identity(source, &source->primary_worker, &source->scheduler_ctx);
    initialize_fake_sleep_task(runtime,
                               source,
                               &task,
                               UINT64_C(0x9876),
                               9U,
                               llam_now_ns() + UINT64_C(1000000000));
    pthread_mutex_lock(&source->lock);
    llam_timer_insert_locked(source, &task);
    pthread_mutex_unlock(&source->lock);
    if (task.active_timer == NULL ||
        atomic_load_explicit(&task.active_timer_heap_shard,
                             memory_order_acquire) != source->id) {
        llam_scheduler_unbind_identity(source, &source->primary_worker);
        llam_scheduler_clear_identity();
        llam_runtime_destroy(runtime);
        return fail("could not arm the cancel/rehome timer");
    }

    atomic_init(&gate.reached, false);
    atomic_init(&gate.resume, false);
    gate.task = &task;
    g_cancel_timer_rehome_gate = &gate;
    llam_timer_test_set_cancel_owner_sample_hook(pause_cancel_after_owner_sample);
    pthread_mutex_lock(&source->opaque_lock);
    setup_merge_pause_ack(source);
    pthread_mutex_unlock(&source->opaque_lock);
    if (pthread_create(&cancel_thread, NULL, cancel_test_sleep_thread, &task) != 0) {
        llam_timer_test_set_cancel_owner_sample_hook(NULL);
        g_cancel_timer_rehome_gate = NULL;
        atomic_store_explicit(&gate.resume, true, memory_order_release);
        llam_scheduler_unbind_identity(source, &source->primary_worker);
        llam_scheduler_clear_identity();
        llam_runtime_destroy(runtime);
        return fail("could not start the cancellation race thread");
    }
    thread_started = true;
    if (wait_for_atomic_bool(&gate.reached, UINT64_C(2000000000))) {
        lock_shard_pair(source, target);
        rehomed = llam_merge_shard_timers_locked(source, target, &migrated) &&
                  migrated == 1U && source->processor.timer_heap_len == 0U &&
                  target->processor.timer_heap_len == 1U &&
                  atomic_load_explicit(&task.active_timer_heap_shard,
                                       memory_order_acquire) == target->id;
        unlock_shard_pair(source, target);
    }
    atomic_store_explicit(&gate.resume, true, memory_order_release);
    if (thread_started) {
        (void)pthread_join(cancel_thread, NULL);
    }
    llam_timer_test_set_cancel_owner_sample_hook(NULL);
    g_cancel_timer_rehome_gate = NULL;

    pthread_mutex_lock(&target->lock);
    completed = rehomed && task.active_timer == NULL &&
                atomic_load_explicit(&task.active_timer_heap_shard,
                                     memory_order_acquire) == UINT_MAX &&
                atomic_load_explicit(&task.state, memory_order_acquire) ==
                    LLAM_TASK_STATE_RUNNABLE &&
                atomic_load_explicit(&task.wait_reason, memory_order_acquire) ==
                    LLAM_WAIT_NONE &&
                atomic_load_explicit(&task.wake_error_code, memory_order_acquire) ==
                    ECANCELED &&
                source->processor.timer_heap_len == 0U &&
                target->processor.timer_heap_len == 0U &&
                source->processor.timer_count == 0U &&
                target->processor.timer_count == 0U &&
                target->processor.inject_q.depth == 1U;
    pthread_mutex_unlock(&target->lock);
    if (completed) {
        rc = 0;
    }
    llam_shard_release_merge_pause(source);
    llam_scheduler_unbind_identity(source, &source->primary_worker);
    llam_scheduler_clear_identity();
    llam_runtime_destroy(runtime);
    if (rc != 0) {
        return fail("cancellation used a stale timer heap owner after rehome");
    }
    return 0;
}

static int test_immediate_expiry_uses_remote_inject(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_task_t task;
    bool completed;

    if (create_runtime(0U, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards != 1U) {
        llam_runtime_destroy(runtime);
        return fail("could not create immediate-expiry runtime");
    }
    shard = &runtime->shards[0];
    initialize_fake_sleep_task(runtime,
                               shard,
                               &task,
                               UINT64_C(0x4321),
                               11U,
                               llam_now_ns() - 1U);
    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, &task);
    pthread_mutex_unlock(&shard->lock);
    llam_fire_expired_timers(shard);

    pthread_mutex_lock(&shard->lock);
    completed = task.active_timer == NULL &&
                atomic_load_explicit(&task.active_timer_heap_shard,
                                     memory_order_acquire) == UINT_MAX &&
                atomic_load_explicit(&task.state, memory_order_acquire) ==
                    LLAM_TASK_STATE_RUNNABLE &&
                atomic_load_explicit(&task.wait_reason, memory_order_acquire) ==
                    LLAM_WAIT_NONE &&
                shard->processor.timer_heap_len == 0U &&
                shard->processor.timer_count == 0U &&
                atomic_load_explicit(&shard->processor.timer_callbacks_active,
                                     memory_order_acquire) == 0U &&
                shard->processor.inject_q.depth == 1U;
    pthread_mutex_unlock(&shard->lock);
    llam_runtime_destroy(runtime);
    if (!completed) {
        return fail("immediate timer expiry bypassed inject or retained heap ownership");
    }
    return 0;
}

static int test_owner_timer_expiry_uses_local_lane(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_task_t task;
    llam_task_t *queued = NULL;
    bool completed;

    if (create_runtime(0U, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards != 1U) {
        llam_runtime_destroy(runtime);
        return fail("could not create owner-expiry runtime");
    }
    shard = &runtime->shards[0];
    llam_scheduler_bind_identity(shard, &shard->primary_worker, &shard->scheduler_ctx);
    initialize_fake_sleep_task(runtime,
                               shard,
                               &task,
                               UINT64_C(0x4322),
                               12U,
                               llam_now_ns() - 1U);
    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, &task);
    pthread_mutex_unlock(&shard->lock);
    llam_fire_expired_timers(shard);

    pthread_mutex_lock(&shard->lock);
    completed = task.active_timer == NULL &&
                atomic_load_explicit(&task.active_timer_heap_shard,
                                     memory_order_acquire) == UINT_MAX &&
                atomic_load_explicit(&task.state, memory_order_acquire) ==
                    LLAM_TASK_STATE_RUNNABLE &&
                shard->processor.inject_q.depth == 0U &&
                (shard->processor.hot_q.depth + shard->processor.norm_q.depth) == 1U;
    if (shard->processor.hot_q.depth > 0U) {
        queued = llam_queue_pop_head(&shard->processor.hot_q);
    } else if (shard->processor.norm_q.depth > 0U) {
        queued = llam_queue_pop_head(&shard->processor.norm_q);
        (void)llam_norm_queue_note_dequeue(shard);
    }
    pthread_mutex_unlock(&shard->lock);
    llam_scheduler_unbind_identity(shard, &shard->primary_worker);
    llam_scheduler_clear_identity();
    llam_runtime_destroy(runtime);
    if (!completed || queued != &task) {
        return fail("owner timer expiry failed to use a validated local queue lane");
    }
    return 0;
}

int main(void) {
    int failures = 0;
    int timer_rc;
    int cancel_rc;

    if (test_unbound_m_cannot_mutate_owner_deque() != 0) {
        failures += 1;
    }
    if (test_host_spawn_uses_inject_lane() != 0) {
        failures += 1;
    }
    timer_rc = test_timer_rehome_generation_and_ownership();
    if (timer_rc != 0 && timer_rc != 77) {
        failures += 1;
    }
    cancel_rc = test_cancel_retries_after_timer_rehome();
    if (cancel_rc != 0 && cancel_rc != 77) {
        failures += 1;
    }
    if (test_immediate_expiry_uses_remote_inject() != 0) {
        failures += 1;
    }
    if (test_owner_timer_expiry_uses_local_lane() != 0) {
        failures += 1;
    }
    if (failures != 0) {
        return 1;
    }
    puts("test_processor_queue_timer_internal: ownership routing checks passed");
    return 0;
}
