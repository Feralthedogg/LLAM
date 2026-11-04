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
#include <inttypes.h>

typedef struct bench_state bench_state_t;

typedef struct bench_job {
    bench_state_t *state;
    uint64_t *input;
    uint64_t *output;
    void *workspace;
    llam_cancel_token_t *token;
    llam_task_t *task;
    uint64_t checksum;
    uint64_t auto_routes[4];
    int failed;
} bench_job_t;

struct bench_state {
    llam_runtime_t *runtime;
    llam_channel_t *ready;
    llam_channel_t *start;
    magic2_cpu_plan *plan;
    llam_magic2_cpu_work_t *auto_work;
    magic2_cpu_plan_info info;
    bench_job_t *jobs;
    size_t elements;
    size_t grain;
    unsigned iterations;
    unsigned tasks;
    unsigned inner;
    bool magic2;
    bool automatic;
    uint64_t elapsed_ns;
    int failed;
};

static uint64_t mix(uint64_t value, unsigned rounds) {
    for (unsigned i = 0U; i < rounds; ++i) {
        value ^= value >> 27U;
        value *= UINT64_C(0x3c79ac492ba7b653);
        value ^= value >> 33U;
    }
    return value;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int tile(const void *environment, void *const *ports, size_t begin,
                size_t end, uint32_t slot, void *scratch) {
    const bench_state_t *state = environment;
    uint64_t *output = ports[0];
    const uint64_t *input = ports[1];

    (void)slot;
    (void)scratch;
    if (state->inner == 0U) {
        memcpy(output + begin, input + begin, (end - begin) * sizeof(*output));
    } else {
        for (size_t i = begin; i < end; ++i) {
            output[i] = mix(input[i], state->inner);
        }
    }
    return 0;
}

static bool cancelled(void) {
    llam_runtime_t *runtime = g_llam_tls_task->owner_runtime;

    return atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
           atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
           llam_task_cancel_token_is_cancelled(g_llam_tls_task);
}

static bool context_ready(void) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;

    return shard != NULL && task != NULL && shard->runtime != NULL &&
           task->owner_runtime == shard->runtime &&
           g_llam_tls_processor == &shard->processor &&
           atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) == 0U &&
           llam_worker_has_safe_managed_g(g_llam_tls_worker, &shard->processor, task);
}

static int manual_run(bench_job_t *job) {
    bench_state_t *state = job->state;
    void *ports[] = {job->output, job->input};
    size_t tile_count = state->elements / state->grain +
                        (state->elements % state->grain != 0U);

    /* Match llam_magic2_cpu_run's managed-context and entry cancellation check. */
    if (!context_ready() || cancelled()) {
        return -1;
    }
    /* llam_magic2_cpu_execute separately checks cancellation before admission. */
    if (cancelled()) {
        return -1;
    }
    for (size_t index = 0U; index < tile_count; ++index) {
        if (cancelled()) {
            return -1;
        }
        if (index != 0U) {
            llam_yield();
            if (!context_ready() || cancelled()) {
                return -1;
            }
        }
        size_t begin = index * state->grain;
        size_t end = state->elements - begin < state->grain ?
                     state->elements : begin + state->grain;

        if (tile(state, ports, begin, end, 0U, NULL) != 0) {
            return -1;
        }
    }
    return cancelled() ? -1 : 0;
}

static int manual_baseline(void *arg, magic2_cpu_run_status *status, int *run_status) {
    int result = manual_run(arg);

    (void)status; /* This benchmark requests only the scalar result channel. */
    *run_status = result == 0 ? MAGIC2_OK : MAGIC2_ECANCELLED;
    if (result != 0) {
        errno = ECANCELED;
    }
    return result;
}

static void worker(void *arg) {
    bench_job_t *job = arg;
    bench_state_t *state = job->state;
    magic2_region regions[] = {
        {job->output, state->elements * sizeof(uint64_t)},
        {job->input, state->elements * sizeof(uint64_t)}
    };
    void *message = job;

    if (llam_channel_send(state->ready, message) != 0 ||
        llam_channel_recv_result(state->start, &message) != 0) {
        job->failed = 1;
        return;
    }
    for (unsigned i = 0U; i < state->iterations; ++i) {
        int result;

        if (state->automatic) {
            int run_status = MAGIC2_ENOTPROCESSED;
            llam_magic2_cpu_auto_info_t info;

            result = llam_magic2_cpu_auto(state->auto_work, job, regions, 2U,
                                          job->workspace, state->info.workspace.bytes,
                                          NULL, &run_status, &info);
            if (run_status != MAGIC2_OK || info.route > LLAM_MAGIC2_CPU_AUTO_NO_CANDIDATE) {
                result = -1;
            } else {
                job->auto_routes[info.route] += 1U;
            }
        } else if (state->magic2) {
            int run_status = MAGIC2_ENOTPROCESSED;

            result = llam_magic2_cpu_run(state->plan, regions, 2U,
                                         job->workspace, state->info.workspace.bytes,
                                         NULL, &run_status);
            if (run_status != MAGIC2_OK) {
                result = -1;
            }
        } else {
            result = manual_run(job);
        }
        if (result != 0) {
            job->failed = 1;
            return;
        }
        /* An observed output from every activation prevents call elimination. */
        job->checksum += job->output[i % state->elements];
    }
}

static void controller(void *arg) {
    bench_state_t *state = arg;
    void *message = state;
    uint64_t begin;

    state->ready = llam_channel_create(state->tasks);
    state->start = llam_channel_create(state->tasks);
    if (state->ready == NULL || state->start == NULL) {
        state->failed = 1;
        return;
    }
    for (unsigned i = 0U; i < state->tasks; ++i) {
        llam_spawn_opts_t options;

        if (llam_spawn_opts_init(&options, sizeof(options)) != 0 ||
            (state->jobs[i].token = llam_cancel_token_create()) == NULL) {
            state->failed = 1;
            (void)llam_runtime_request_stop();
            return;
        }
        options.cancel_token = state->jobs[i].token;
        state->jobs[i].task = llam_spawn_ex(worker, &state->jobs[i], &options, sizeof(options));
        if (state->jobs[i].task == NULL) {
            state->failed = 1;
            (void)llam_runtime_request_stop();
            return;
        }
    }
    for (unsigned i = 0U; i < state->tasks; ++i) {
        if (llam_channel_recv_result(state->ready, &message) != 0) {
            state->failed = 1;
            return;
        }
    }
    begin = llam_now_ns();
    for (unsigned i = 0U; i < state->tasks; ++i) {
        if (llam_channel_send(state->start, state) != 0) {
            state->failed = 1;
        }
    }
    for (unsigned i = 0U; i < state->tasks; ++i) {
        if (llam_join(state->jobs[i].task) != 0) {
            state->failed = 1;
        }
    }
    state->elapsed_ns = llam_now_ns() - begin;
    for (unsigned i = 0U; i < state->tasks; ++i) {
        if (llam_cancel_token_destroy(state->jobs[i].token) != 0) {
            state->failed = 1;
        }
        state->jobs[i].token = NULL;
    }
    (void)llam_channel_destroy(state->ready);
    (void)llam_channel_destroy(state->start);
}

static unsigned number(const char *text, unsigned maximum) {
    char *end;
    unsigned long result;

    errno = 0;
    result = strtoul(text, &end, 10);
    if (errno != 0 || *text < '0' || *text > '9' || *end != '\0' || result > maximum) {
        return UINT_MAX;
    }
    return (unsigned)result;
}

int main(int argc, char **argv) {
    bench_state_t state = {0};
    llam_runtime_opts_t options;
    llam_runtime_stats_t stats;
    llam_task_t *root;
    uint64_t checksum = 0U;
    uint64_t auto_routes[4] = {0};
    uint64_t setup_ns;
    uint64_t begin;
    int result = 1;

    if (argc != 7 || (strcmp(argv[1], "manual") != 0 && strcmp(argv[1], "magic2") != 0 &&
                     strcmp(argv[1], "auto") != 0)) {
        fprintf(stderr, "usage: %s manual|magic2|auto elements grain iterations tasks inner_rounds\n", argv[0]);
        return 2;
    }
    state.magic2 = strcmp(argv[1], "magic2") == 0;
    state.automatic = strcmp(argv[1], "auto") == 0;
    state.elements = number(argv[2], 1048576U);
    state.grain = number(argv[3], 1048576U);
    state.iterations = number(argv[4], 1000000U);
    state.tasks = number(argv[5], 64U);
    state.inner = number(argv[6], 1024U);
    if (state.elements == 0U || state.elements == UINT_MAX || state.grain == 0U ||
        state.grain == UINT_MAX || state.iterations == 0U || state.iterations == UINT_MAX ||
        state.tasks == 0U || state.tasks == UINT_MAX || state.inner == UINT_MAX) {
        return 2;
    }
    state.info = magic2_cpu_plan_info_initializer();
    state.jobs = calloc(state.tasks, sizeof(*state.jobs));
    if (state.jobs == NULL) {
        return 1;
    }
    begin = llam_now_ns();
    if (state.magic2 || state.automatic) {
        magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;
        magic2_port ports[2] = {{0}, {0}};

        for (unsigned i = 0U; i < 2U; ++i) {
            ports[i].bytes = state.elements * sizeof(uint64_t);
            ports[i].stride = sizeof(uint64_t);
            ports[i].alignment = _Alignof(uint64_t);
            ports[i].region = i;
            ports[i].access = i == 0U ? MAGIC2_BUFFER_WRITE : MAGIC2_BUFFER_READ;
            ports[i].present = 1U;
            ports[i].initialization = MAGIC2_PRESERVE;
        }
        spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
        spec.schedule = MAGIC2_CPU_SCHEDULE_CALLER;
        spec.shape.count = state.elements;
        spec.stable_id.low = 1U;
        spec.shape.operation_id.low = state.inner == 0U ? 1U : 2U;
        spec.shape.semantic_id.low = state.inner + 1U;
        spec.shape.port_count = 2U;
        spec.shape.region_count = 2U;
        spec.shape.ports = ports;
        spec.tile = tile;
        spec.environment = &state;
        spec.environment_bytes = sizeof(state);
        spec.grain = state.grain;
        spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
        if (state.automatic) {
            /* Known internal workload hints, not inference from arbitrary task code. */
            llam_magic2_cpu_work_spec_t descriptor = {0};

            descriptor.kind = LLAM_MAGIC2_CPU_WORK_TRANSFORM;
            descriptor.plans = &spec;
            descriptor.plan_count = 1U;
            descriptor.baseline = manual_baseline;
            descriptor.estimated_cost_ns = state.inner == 0U ?
                                           state.elements / 4U : state.elements * state.inner;
            if (llam_magic2_cpu_work_create(&descriptor, &state.auto_work,
                                            &state.info.workspace) != MAGIC2_OK) {
                goto done;
            }
        } else {
            if (llam_magic2_cpu_plan_create(&spec, &state.plan) != MAGIC2_OK ||
                llam_magic2_cpu_plan_get_info(state.plan, &state.info) != MAGIC2_OK) {
                goto done;
            }
        }
    }
    setup_ns = llam_now_ns() - begin;
    for (unsigned i = 0U; i < state.tasks; ++i) {
        state.jobs[i].state = &state;
        state.jobs[i].input = malloc(state.elements * sizeof(uint64_t));
        state.jobs[i].output = malloc(state.elements * sizeof(uint64_t));
        state.jobs[i].workspace = (state.magic2 || state.automatic) ? malloc(state.info.workspace.bytes) : NULL;
        if (state.jobs[i].input == NULL || state.jobs[i].output == NULL ||
            ((state.magic2 || state.automatic) && state.jobs[i].workspace == NULL)) {
            goto done;
        }
        for (size_t j = 0U; j < state.elements; ++j) {
            state.jobs[i].input[j] = (j + 1U) * UINT64_C(0x9e3779b97f4a7c15) + i;
            state.jobs[i].output[j] = 0U;
        }
    }
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        goto done;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    options.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_create_compat(&options, sizeof(options), &state.runtime) != 0) {
        goto done;
    }
    root = llam_runtime_spawn_ex(state.runtime, controller, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(state.runtime) != 0 || llam_join(root) != 0 ||
        state.failed || llam_runtime_collect_stats_ex_handle(state.runtime, &stats, sizeof(stats)) != 0 ||
        stats.active_workers != 1U || stats.blocking_calls != 0U) {
        goto done;
    }
    for (unsigned i = 0U; i < state.tasks; ++i) {
        uint64_t expected_checksum = 0U;

        if (state.jobs[i].failed) {
            goto done;
        }
        for (size_t j = 0U; j < state.elements; ++j) {
            if (state.jobs[i].output[j] != mix(state.jobs[i].input[j], state.inner)) {
                goto done;
            }
        }
        for (unsigned j = 0U; j < state.iterations; ++j) {
            expected_checksum += mix(state.jobs[i].input[j % state.elements], state.inner);
        }
        if (state.jobs[i].checksum != expected_checksum) {
            goto done;
        }
        checksum += state.jobs[i].checksum;
        for (unsigned route = 0U; route < 4U; ++route) {
            auto_routes[route] += state.jobs[i].auto_routes[route];
        }
    }
    printf("{\"mode\":\"%s\",\"elements\":%zu,\"grain\":%zu,\"iterations\":%u,"
           "\"tasks\":%u,\"inner_rounds\":%u,\"workers\":%u,\"elapsed_ns\":%" PRIu64 ","
           "\"plan_setup_ns\":%" PRIu64 ",\"yields\":%" PRIu64 ",\"blocking_calls\":%" PRIu64 ","
           "\"auto_baseline\":%" PRIu64 ",\"auto_family\":%" PRIu64 ",\"auto_busy\":%" PRIu64 ","
           "\"checksum\":\"%016" PRIx64 "\"}\n", argv[1], state.elements, state.grain,
           state.iterations, state.tasks, state.inner, stats.active_workers, state.elapsed_ns,
           setup_ns, stats.yields, stats.blocking_calls,
           auto_routes[0], auto_routes[1], auto_routes[2], checksum);
    result = 0;
done:
    if (result != 0) {
        fprintf(stderr, "bench_magic2_cpu: setup, execution, or exact-output verification failed (errno=%d)\n", errno);
    }
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    llam_magic2_cpu_plan_release(&state.plan);
    llam_magic2_cpu_work_release(&state.auto_work);
    for (unsigned i = 0U; i < state.tasks; ++i) {
        free(state.jobs[i].input);
        free(state.jobs[i].output);
        free(state.jobs[i].workspace);
    }
    free(state.jobs);
    return result;
}
