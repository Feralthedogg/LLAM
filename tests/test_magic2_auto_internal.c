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
#include "runtime_magic2.h"

typedef struct auto_fixture auto_fixture_t;
typedef struct auto_environment {
    auto_fixture_t *fixture;
    unsigned variant;
} auto_environment_t;

typedef struct auto_data {
    unsigned calls;
    unsigned values[4];
} auto_data_t;

typedef struct auto_job {
    auto_fixture_t *fixture;
    llam_magic2_cpu_work_t *work;
    auto_data_t data;
    void *workspace[1];
    magic2_cpu_run_status status;
    llam_magic2_cpu_auto_info_t info;
    int result;
    int error;
    int run_status;
} auto_job_t;

struct auto_fixture {
    llam_runtime_t *runtime;
    llam_magic2_cpu_work_t *work;
    magic2_cpu_plan *baseline;
    llam_cancel_token_t *token;
    auto_environment_t environments[3];
    atomic_uint owners;
    atomic_uint started;
    atomic_uint timer_started;
    unsigned baseline_calls;
    unsigned failures;
    unsigned which;
};

static void check(auto_fixture_t *fixture, bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_magic2_auto_internal: case %u: %s\n", fixture->which, message);
        fixture->failures += 1U;
    }
}

static int retain_owner(void *arg) {
    auto_fixture_t *fixture = arg;

    atomic_fetch_add_explicit(&fixture->owners, 1U, memory_order_relaxed);
    return 0;
}

static void release_owner(void *arg) {
    auto_fixture_t *fixture = arg;

    (void)atomic_fetch_sub_explicit(&fixture->owners, 1U, memory_order_acq_rel);
    errno = EIO; /* A final environment release must not overwrite cancellation errno. */
}

static int auto_tile(const void *arg, void *const *ports, size_t begin,
                     size_t end, uint32_t slot, void *scratch) {
    const auto_environment_t *environment = arg;
    auto_data_t *data = ports[0];

    (void)scratch;
    if (slot != 0U || end != begin + 1U || end > 4U) {
        return 91;
    }
    data->calls += 1U;
    data->values[begin] += 1U;
    atomic_store_explicit(&environment->fixture->started, 1U, memory_order_release);
    return environment->fixture->which == 2U && environment->variant == 0U ? 73 : 0;
}

static int baseline(void *arg, magic2_cpu_run_status *status, int *run_status) {
    auto_job_t *job = arg;
    magic2_region region = {&job->data, sizeof(job->data)};

    job->fixture->baseline_calls += 1U;
    return llam_magic2_cpu_run(job->fixture->baseline, &region, 1U,
                               job->workspace, sizeof(job->workspace), status, run_status);
}

static void invoke(void *arg) {
    auto_job_t *job = arg;
    magic2_region region = {&job->data, sizeof(job->data)};

    job->status = magic2_cpu_run_status_initializer();
    job->result = llam_magic2_cpu_auto(job->work, job, &region, 1U,
                                      job->workspace, sizeof(job->workspace),
                                      &job->status, &job->run_status, &job->info);
    job->error = errno;
}

static void timer_task(void *arg) {
    auto_fixture_t *fixture = arg;

    atomic_store_explicit(&fixture->timer_started, 1U, memory_order_release);
    (void)llam_sleep_ns(UINT64_C(10000000));
}

static void controller(void *arg) {
    auto_fixture_t *fixture = arg;
    auto_job_t first = {.fixture = fixture, .work = fixture->work};
    auto_job_t second = {.fixture = fixture, .work = fixture->work};
    llam_task_t *task = NULL;
    llam_spawn_opts_t options;

    if (fixture->which == 0U) {
        int run_status = 0;

        check(fixture, llam_magic2_cpu_auto(NULL, NULL, NULL, 0U, NULL, 0U,
                                            NULL, &run_status, NULL) == -1 &&
                       errno == EINVAL && run_status == MAGIC2_ENOTPROCESSED,
              "NULL descriptor was not rejected as a wrapper argument error");
    }
    if (fixture->which == 3U || fixture->which == 4U || fixture->which == 5U) {
        check(fixture, llam_spawn_opts_init(&options, sizeof(options)) == 0, "spawn options");
        fixture->token = llam_cancel_token_create();
        check(fixture, fixture->token != NULL, "cancel token");
        options.cancel_token = fixture->token;
        if (fixture->which == 5U) {
            check(fixture, llam_cancel_token_cancel(fixture->token) == 0, "pre-cancel");
        }
        task = llam_spawn_ex(invoke, &first, &options, sizeof(options));
        check(fixture, task != NULL, "child spawn");
        if (task == NULL) {
            return;
        }
        if (fixture->which != 5U) {
            while (atomic_load_explicit(&fixture->started, memory_order_acquire) == 0U) {
                llam_yield();
            }
        }
        if (fixture->which == 3U) {
            invoke(&second);
            check(fixture, second.info.route == LLAM_MAGIC2_CPU_AUTO_BUSY &&
                           second.info.admission_status == MAGIC2_EBUSY &&
                           second.data.calls == 4U && second.result == 0,
                  "busy admission did not execute exactly one baseline");
        } else if (fixture->which == 4U) {
            llam_magic2_cpu_work_release(&fixture->work);
            check(fixture, atomic_load_explicit(&fixture->owners, memory_order_acquire) == 3U,
                  "work or environments freed during a yielded activation");
            check(fixture, llam_cancel_token_cancel(fixture->token) == 0, "running cancel");
        }
        check(fixture, llam_join(task) == 0, "child drain");
        check(fixture, llam_cancel_token_destroy(fixture->token) == 0, "token drain");
        fixture->token = NULL;
    } else if (fixture->which == 6U) {
        task = llam_spawn(timer_task, fixture, NULL);
        check(fixture, task != NULL, "timer spawn");
        while (atomic_load_explicit(&fixture->timer_started, memory_order_acquire) == 0U) {
            llam_yield();
        }
        invoke(&first);
        check(fixture, first.info.bucket == 2U && first.info.minimum_cost_ns == 200000U,
              "I/O/timer pressure did not raise the admission threshold");
        check(fixture, llam_join(task) == 0, "timer drain");
    } else {
        invoke(&first);
    }

    if (fixture->which == 0U || fixture->which == 6U) {
        check(fixture, first.result == 0 && first.run_status == MAGIC2_OK &&
                       first.info.route == LLAM_MAGIC2_CPU_AUTO_BASELINE &&
                       fixture->baseline_calls == 1U && first.data.calls == 4U,
              "below-threshold work did not execute the baseline once");
    } else if (fixture->which == 2U) {
        check(fixture, first.result == 0 && first.run_status == MAGIC2_EIMPLEMENTATION &&
                       first.status.implementation_status == 73 && first.data.calls == 1U &&
                       fixture->baseline_calls == 0U && first.info.observation_valid == 0U,
              "kernel failure retried/fell back or trained timing evidence");
        invoke(&second);
        check(fixture, second.info.selected_index == 1U && second.run_status == MAGIC2_OK &&
                       second.data.calls == 4U && fixture->baseline_calls == 0U,
              "failed family member was not disabled in its bucket");
    } else if (fixture->which == 4U || fixture->which == 5U) {
        check(fixture, first.result == -1 && first.error == ECANCELED &&
                       first.data.calls == (fixture->which == 4U ? 1U : 0U) &&
                       first.run_status == (fixture->which == 4U ? MAGIC2_ECANCELLED : MAGIC2_ENOTPROCESSED) &&
                       first.info.observation_valid == 0U && fixture->baseline_calls == 0U,
              "cancellation status/lifetime/no-replay contract changed");
        if (fixture->which == 4U) {
            check(fixture, atomic_load_explicit(&fixture->owners, memory_order_acquire) == 1U,
                  "last activation did not release family ownership");
        }
    } else {
        check(fixture, first.result == 0 && first.run_status == MAGIC2_OK &&
                       first.info.route == LLAM_MAGIC2_CPU_AUTO_FAMILY &&
                       first.info.slot_budget == 1U && first.info.observation_valid == 1U &&
                       first.data.calls == 4U,
              "heavy work did not execute one managed family member");
    }
}

int test_magic2_auto_internal(void) {
    for (unsigned which = 0U; which < 7U; ++which) {
        auto_fixture_t fixture = {.which = which};
        magic2_cpu_plan_spec plans[2];
        magic2_port port = {0};
        magic2_storage workspace;
        llam_runtime_opts_t options;
        llam_task_t *root;
        llam_runtime_stats_t stats;
        llam_magic2_cpu_work_spec_t descriptor = {0};

        atomic_init(&fixture.owners, 0U);
        atomic_init(&fixture.started, 0U);
        atomic_init(&fixture.timer_started, 0U);
        port.bytes = sizeof(auto_data_t);
        port.alignment = _Alignof(auto_data_t);
        port.access = MAGIC2_BUFFER_READ_WRITE;
        port.present = 1U;
        for (unsigned i = 0U; i < 3U; ++i) {
            magic2_cpu_plan_spec plan = MAGIC2_CPU_PLAN_SPEC_INIT;

            fixture.environments[i].fixture = &fixture;
            fixture.environments[i].variant = i;
            plan.schedule = MAGIC2_CPU_SCHEDULE_CALLER;
            plan.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
            plan.failure_contract = MAGIC2_FAILURE_PARTIAL;
            plan.stable_id.low = i + 1U;
            plan.shape.operation_id.low = 101U;
            plan.shape.semantic_id.low = 1U;
            plan.shape.count = 4U;
            plan.shape.port_count = plan.shape.region_count = 1U;
            plan.shape.ports = &port;
            plan.tile = auto_tile;
            plan.environment = &fixture.environments[i];
            plan.environment_bytes = sizeof(fixture.environments[i]);
            plan.owner.token = &fixture;
            plan.owner.retain = retain_owner;
            plan.owner.release = release_owner;
            if (i < 2U) {
                plans[i] = plan;
            } else {
                check(&fixture, llam_magic2_cpu_plan_create(&plan, &fixture.baseline) == MAGIC2_OK,
                      "baseline fixture create");
            }
        }
        descriptor.kind = LLAM_MAGIC2_CPU_WORK_TRANSFORM;
        descriptor.plans = plans;
        descriptor.plan_count = 2U;
        descriptor.baseline = baseline;
        descriptor.estimated_cost_ns = which == 0U ? 1U : which == 6U ? 100000U : 1000000U;
        if (which == 0U) {
            workspace = (magic2_storage){123U, 64U};
            plans[1].shape.operation_id.low += 1U;
            check(&fixture, llam_magic2_cpu_work_create(&descriptor, &fixture.work, &workspace) == MAGIC2_ECONTRACT &&
                           fixture.work == NULL && workspace.bytes == 0U && workspace.alignment == 0U &&
                           atomic_load_explicit(&fixture.owners, memory_order_acquire) == 1U,
                  "failed family construction leaked references or stale workspace requirements");
            plans[1].shape.operation_id.low -= 1U;
        }
        check(&fixture, llam_magic2_cpu_work_create(&descriptor, &fixture.work, &workspace) == MAGIC2_OK &&
                       workspace.bytes <= sizeof(void *), "work family create");
        check(&fixture, llam_runtime_opts_init(&options, sizeof(options)) == 0, "runtime options");
        options.deterministic = 1U;
        options.preempt_mode = LLAM_PREEMPT_OFF;
        options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
        check(&fixture, llam_runtime_create(&options, sizeof(options), &fixture.runtime) == 0,
              "runtime create");
        if (fixture.failures != 0U) {
            return 1;
        }
        root = llam_runtime_spawn_ex(fixture.runtime, controller, &fixture, NULL, 0U);
        check(&fixture, root != NULL && llam_runtime_run_handle(fixture.runtime) == 0 &&
                       llam_join(root) == 0, "controller run");
        check(&fixture, llam_runtime_collect_stats_ex_handle(fixture.runtime, &stats, sizeof(stats)) == 0 &&
                       stats.active_workers == 1U && stats.blocking_calls == 0U, "one-P/no-pool budget");
        llam_runtime_destroy(fixture.runtime);
        llam_magic2_cpu_work_release(&fixture.work);
        llam_magic2_cpu_plan_release(&fixture.baseline);
        check(&fixture, atomic_load_explicit(&fixture.owners, memory_order_acquire) == 0U,
              "environment owners leaked");
        if (fixture.failures != 0U) {
            return 1;
        }
    }
    puts("test_magic2_auto_internal: threshold, pressure, family, fallback, cancellation, and lifetime passed");
    return 0;
}
