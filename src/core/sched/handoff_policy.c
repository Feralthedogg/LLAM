/**
 * @file src/core/sched/handoff_policy.c
 * @brief Pure policy decisions shared by direct task handoff paths.
 *
 * @details
 * Direct wake, yield, join, and reinjection have different commit points and
 * fallback behavior. They nevertheless share the same eligibility policy.
 * This unit evaluates that policy without changing streaks, queues, task
 * state, metrics, or ownership. Callers retain responsibility for context
 * validation, snapshots that require locks, result accounting, and commits.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

/** @brief Check task ownership and hard affinity without recording a fatal. */
static bool llam_handoff_task_matches(const llam_runtime_t *rt,
                                      const llam_shard_t *shard,
                                      const llam_task_t *task) {
    if (rt == NULL || shard == NULL || task == NULL ||
        task->owner_runtime != rt) {
        return false;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) == 0U) {
        return true;
    }
    return task->home_shard < rt->active_shards &&
           task->home_shard == shard->id;
}

llam_handoff_reject_t llam_direct_handoff_policy(
    const llam_handoff_policy_input_t *input) {
    const llam_runtime_t *rt;
    const llam_shard_t *shard;
    unsigned timer_count;
    unsigned budget;

    if (input == NULL || input->runtime == NULL || input->shard == NULL ||
        input->current == NULL) {
        return LLAM_HANDOFF_REJECT_CONTEXT;
    }
    rt = input->runtime;
    shard = input->shard;
    if (rt->shards == NULL || rt->active_shards == 0U ||
        input->target_id >= rt->active_shards ||
        shard != &rt->shards[input->target_id] ||
        shard->runtime != rt || shard->id != input->target_id) {
        return LLAM_HANDOFF_REJECT_CONTEXT;
    }
    if (input->next == input->current) {
        return LLAM_HANDOFF_REJECT_SELF;
    }
    if (!llam_handoff_task_matches(rt, shard, input->current) ||
        (input->next != NULL &&
         !llam_handoff_task_matches(rt, shard, input->next))) {
        return LLAM_HANDOFF_REJECT_AFFINITY;
    }
    if (rt->external_driver.enabled) {
        return LLAM_HANDOFF_REJECT_EXTERNAL_DRIVER;
    }
    if (rt->trace_events_enabled != 0U ||
        rt->run_timing_enabled != 0U ||
        rt->wake_latency_metrics_enabled != 0U) {
        return LLAM_HANDOFF_REJECT_INSTRUMENTATION;
    }
    if (input->require_lockfree_queue &&
        !llam_lockfree_normq_enabled(rt)) {
        return LLAM_HANDOFF_REJECT_QUEUE_MODE;
    }
    if (!llam_shard_accepts_new_work(shard)) {
        return LLAM_HANDOFF_REJECT_SHARD_STATE;
    }
    if (shard->opaque_redirect_active) {
        return LLAM_HANDOFF_REJECT_OPAQUE_REDIRECT;
    }
    if (input->target_deadline_active) {
        return LLAM_HANDOFF_REJECT_DEADLINE;
    }
    timer_count = atomic_load_explicit(
        &shard->timer_count, memory_order_acquire);
    if (timer_count != 0U &&
        (!input->honor_timer_allowance ||
         rt->direct_handoff_allow_timers == 0U)) {
        return LLAM_HANDOFF_REJECT_TIMER;
    }
    if (rt->direct_handoff_live_limit != 0U &&
        llam_runtime_live_tasks((llam_runtime_t *)rt) >
            rt->direct_handoff_live_limit) {
        return LLAM_HANDOFF_REJECT_LIVE_LIMIT;
    }
    budget = llam_runtime_direct_handoff_budget(rt);
    if (budget != 0U && shard->direct_handoff_streak >= budget) {
        return LLAM_HANDOFF_REJECT_BUDGET;
    }
    return LLAM_HANDOFF_REJECT_NONE;
}

llam_handoff_result_class_t llam_handoff_reject_classify(
    llam_handoff_reject_t reject) {
    switch (reject) {
    case LLAM_HANDOFF_REJECT_NONE:
        return LLAM_HANDOFF_RESULT_NONE;
    case LLAM_HANDOFF_REJECT_CONTEXT:
    case LLAM_HANDOFF_REJECT_AFFINITY:
        return LLAM_HANDOFF_RESULT_CONTEXT;
    case LLAM_HANDOFF_REJECT_BUDGET:
        return LLAM_HANDOFF_RESULT_BUDGET;
    case LLAM_HANDOFF_REJECT_NO_WORK:
        return LLAM_HANDOFF_RESULT_NO_WORK;
    case LLAM_HANDOFF_REJECT_SELF:
        return LLAM_HANDOFF_RESULT_SELF;
    case LLAM_HANDOFF_REJECT_PUSH:
        return LLAM_HANDOFF_RESULT_PUSH;
    case LLAM_HANDOFF_REJECT_RACE:
        return LLAM_HANDOFF_RESULT_RACE;
    case LLAM_HANDOFF_REJECT_EXTERNAL_DRIVER:
    case LLAM_HANDOFF_REJECT_INSTRUMENTATION:
    case LLAM_HANDOFF_REJECT_QUEUE_MODE:
    case LLAM_HANDOFF_REJECT_SHARD_STATE:
    case LLAM_HANDOFF_REJECT_OPAQUE_REDIRECT:
    case LLAM_HANDOFF_REJECT_DEADLINE:
    case LLAM_HANDOFF_REJECT_TIMER:
    case LLAM_HANDOFF_REJECT_LIVE_LIMIT:
    default:
        return LLAM_HANDOFF_RESULT_POLICY;
    }
}
