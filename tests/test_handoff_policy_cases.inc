/**
 * @file tests/test_handoff_policy_cases.inc
 * @brief Table-driven direct-handoff policy and rejection-mapping checks.
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

typedef struct handoff_policy_fixture {
    llam_runtime_t runtime;
    llam_shard_t shard;
    llam_task_t current;
    llam_task_t next;
    llam_handoff_policy_input_t input;
} handoff_policy_fixture_t;

static void set_handoff_fixture_live_tasks(handoff_policy_fixture_t *fixture,
                                           unsigned live_tasks) {
    /*
     * Windows maintains the authoritative live-task count on the runtime,
     * while the other backends sum the shard-local counters. Keep both
     * projections synchronized so this policy fixture exercises the same
     * logical snapshot on every backend.
     */
    atomic_store_explicit(&fixture->runtime.live_tasks,
                          live_tasks,
                          memory_order_release);
    atomic_store_explicit(&fixture->shard.live_tasks,
                          live_tasks,
                          memory_order_release);
}

static void init_handoff_policy_fixture(handoff_policy_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->runtime.active_shards = 1U;
    fixture->runtime.shards = &fixture->shard;
    fixture->runtime.experimental_lockfree_normq = 1U;
    fixture->shard.runtime = &fixture->runtime;
    fixture->shard.id = 0U;
    fixture->current.owner_runtime = &fixture->runtime;
    fixture->next.owner_runtime = &fixture->runtime;
    atomic_init(&fixture->runtime.direct_handoff_budget, 0U);
    atomic_init(&fixture->runtime.live_tasks, 0U);
    atomic_init(&fixture->runtime.live_task_shards, 0U);
    atomic_init(&fixture->shard.timer_count, 0U);
    atomic_init(&fixture->shard.live_tasks, 0U);
    atomic_init(&fixture->shard.merge_pause_requested, 0U);
    atomic_init(&fixture->shard.merge_pause_ack, 0U);
    fixture->input.runtime = &fixture->runtime;
    fixture->input.shard = &fixture->shard;
    fixture->input.current = &fixture->current;
    fixture->input.next = &fixture->next;
    fixture->input.target_id = 0U;
    fixture->input.target_deadline_active = false;
    fixture->input.honor_timer_allowance = true;
    fixture->input.require_lockfree_queue = true;
}

static int exercise_handoff_reason_mapping(void) {
    static const struct {
        llam_handoff_reject_t reject;
        llam_handoff_result_class_t expected;
    } cases[] = {
        {LLAM_HANDOFF_REJECT_NONE, LLAM_HANDOFF_RESULT_NONE},
        {LLAM_HANDOFF_REJECT_CONTEXT, LLAM_HANDOFF_RESULT_CONTEXT},
        {LLAM_HANDOFF_REJECT_EXTERNAL_DRIVER, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_INSTRUMENTATION, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_QUEUE_MODE, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_SHARD_STATE, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_OPAQUE_REDIRECT, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_DEADLINE, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_TIMER, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_LIVE_LIMIT, LLAM_HANDOFF_RESULT_POLICY},
        {LLAM_HANDOFF_REJECT_BUDGET, LLAM_HANDOFF_RESULT_BUDGET},
        {LLAM_HANDOFF_REJECT_AFFINITY, LLAM_HANDOFF_RESULT_CONTEXT},
        {LLAM_HANDOFF_REJECT_NO_WORK, LLAM_HANDOFF_RESULT_NO_WORK},
        {LLAM_HANDOFF_REJECT_SELF, LLAM_HANDOFF_RESULT_SELF},
        {LLAM_HANDOFF_REJECT_PUSH, LLAM_HANDOFF_RESULT_PUSH},
        {LLAM_HANDOFF_REJECT_RACE, LLAM_HANDOFF_RESULT_RACE},
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (llam_handoff_reject_classify(cases[i].reject) !=
            cases[i].expected) {
            return fail_msg("handoff rejection mapping changed");
        }
    }
    return 0;
}

static int exercise_handoff_timer_matrix(void) {
    static const struct {
        unsigned configured_allow;
        unsigned timer_count;
        bool honor_allowance;
        llam_handoff_reject_t expected;
    } cases[] = {
        {0U, 0U, true, LLAM_HANDOFF_REJECT_NONE},
        {0U, 1U, true, LLAM_HANDOFF_REJECT_TIMER},
        {1U, 0U, true, LLAM_HANDOFF_REJECT_NONE},
        {1U, 1U, true, LLAM_HANDOFF_REJECT_NONE},
        {0U, 0U, false, LLAM_HANDOFF_REJECT_NONE},
        {0U, 1U, false, LLAM_HANDOFF_REJECT_TIMER},
        {1U, 0U, false, LLAM_HANDOFF_REJECT_NONE},
        {1U, 1U, false, LLAM_HANDOFF_REJECT_TIMER},
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        handoff_policy_fixture_t fixture;

        init_handoff_policy_fixture(&fixture);
        fixture.runtime.direct_handoff_allow_timers =
            cases[i].configured_allow;
        fixture.input.honor_timer_allowance = cases[i].honor_allowance;
        atomic_store_explicit(&fixture.shard.timer_count,
                              cases[i].timer_count,
                              memory_order_release);
        if (llam_direct_handoff_policy(&fixture.input) != cases[i].expected) {
            return fail_msg("handoff timer policy matrix changed");
        }
    }
    return 0;
}

static int exercise_handoff_policy_rejections(void) {
    handoff_policy_fixture_t fixture;

    init_handoff_policy_fixture(&fixture);
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_NONE) {
        return fail_msg("baseline handoff policy was rejected");
    }

    fixture.input.target_deadline_active = true;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_DEADLINE) {
        return fail_msg("handoff deadline policy was not enforced");
    }
    fixture.input.target_deadline_active = false;

    fixture.runtime.external_driver.enabled = true;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_EXTERNAL_DRIVER) {
        return fail_msg("external driver handoff policy was not enforced");
    }
    fixture.runtime.external_driver.enabled = false;

    fixture.runtime.trace_events_enabled = 1U;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_INSTRUMENTATION) {
        return fail_msg("instrumented handoff policy was not enforced");
    }
    fixture.runtime.trace_events_enabled = 0U;

    fixture.runtime.experimental_lockfree_normq = 0U;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_QUEUE_MODE) {
        return fail_msg("handoff queue policy was not enforced");
    }
    fixture.runtime.experimental_lockfree_normq = 1U;

    fixture.shard.opaque_redirect_active = true;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_OPAQUE_REDIRECT) {
        return fail_msg("opaque redirect handoff policy was not enforced");
    }
    fixture.shard.opaque_redirect_active = false;

    fixture.runtime.direct_handoff_live_limit = 1U;
    set_handoff_fixture_live_tasks(&fixture, 2U);
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_LIVE_LIMIT) {
        return fail_msg("handoff live-task limit was not enforced");
    }
    fixture.runtime.direct_handoff_live_limit = 0U;
    set_handoff_fixture_live_tasks(&fixture, 0U);

    atomic_store_explicit(&fixture.runtime.direct_handoff_budget,
                          1U,
                          memory_order_release);
    fixture.shard.direct_handoff_streak = 1U;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_BUDGET) {
        return fail_msg("handoff burst budget was not enforced");
    }
    atomic_store_explicit(&fixture.runtime.direct_handoff_budget,
                          0U,
                          memory_order_release);
    fixture.shard.direct_handoff_streak = 0U;

    fixture.current.flags = LLAM_TASK_FLAG_PINNED;
    fixture.current.home_shard = 1U;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_AFFINITY) {
        return fail_msg("handoff affinity policy was not enforced");
    }
    fixture.current.flags = 0U;

    fixture.input.next = &fixture.current;
    if (llam_direct_handoff_policy(&fixture.input) !=
        LLAM_HANDOFF_REJECT_SELF) {
        return fail_msg("self handoff was not rejected");
    }
    return 0;
}

static int exercise_handoff_policy_cases(void) {
    if (exercise_handoff_reason_mapping() != 0 ||
        exercise_handoff_timer_matrix() != 0 ||
        exercise_handoff_policy_rejections() != 0) {
        return 1;
    }
    return 0;
}
