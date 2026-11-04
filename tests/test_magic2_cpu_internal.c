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

#define CPU_TILES 16U
#define CPU_TASKS 6U

typedef enum cpu_case {
    CPU_SUCCESS,
    CPU_FAILURE,
    CPU_CANCEL_BEFORE,
    CPU_CANCEL_BETWEEN,
    CPU_CANCEL_RUNNING,
    CPU_CANCEL_FAILURE,
    CPU_SHUTDOWN
} cpu_case_t;

typedef struct cpu_state cpu_state_t;

typedef struct cpu_environment {
    cpu_state_t *state;
} cpu_environment_t;

typedef struct cpu_budget {
    atomic_uint active;
    atomic_uint peak;
} cpu_budget_t;

struct cpu_state {
    cpu_environment_t environment;
    magic2_cpu_plan *plan;
    magic2_cpu_plan_info info;
    magic2_cpu_run_status status;
    llam_runtime_t *runtime;
    llam_cancel_token_t *token;
    cpu_budget_t *budget;
    void *workspace;
    cpu_case_t which;
    atomic_uint failures;
    atomic_uint owner_refs;
    atomic_uint owner_releases;
    atomic_uint calls;
    atomic_uint started;
    atomic_uint tile_finished;
    atomic_uint returned;
    atomic_uint native_action;
    int output[CPU_TILES];
    int bridge_result;
    int bridge_errno;
    int run_status;
    int driver_result;
};

static void cpu_fail(cpu_state_t *state, const char *message) {
    fprintf(stderr, "test_magic2_cpu_internal: case %u: %s\n",
            (unsigned)state->which, message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static int cpu_owner_retain(void *token) {
    cpu_state_t *state = token;

    atomic_fetch_add_explicit(&state->owner_refs, 1U, memory_order_relaxed);
    return 0;
}

static void cpu_owner_release(void *token) {
    cpu_state_t *state = token;

    if (atomic_fetch_sub_explicit(&state->owner_refs, 1U, memory_order_relaxed) != 1U) {
        cpu_fail(state, "environment owner was released incorrectly");
    }
    atomic_fetch_add_explicit(&state->owner_releases, 1U, memory_order_relaxed);
}

static int cpu_tile(const void *environment, void *const *ports,
                    size_t begin, size_t end, uint32_t slot, void *scratch) {
    cpu_state_t *state = ((const cpu_environment_t *)environment)->state;
    unsigned call = atomic_fetch_add_explicit(&state->calls, 1U, memory_order_relaxed);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);

    if (!llam_worker_has_safe_managed_g(g_llam_tls_worker, g_llam_tls_processor,
                                      g_llam_tls_task) ||
        g_llam_tls_task->owner_runtime != state->runtime ||
        g_llam_tls_processor != &g_llam_tls_shard->processor) {
        cpu_fail(state, "tile did not execute in its managed G/P");
    }
    if (slot != 0U || begin != call || end != begin + 1U || end > CPU_TILES ||
        ports[0] != state->output || scratch == NULL ||
        atomic_load_explicit(&state->owner_refs, memory_order_acquire) != 1U) {
        cpu_fail(state, "tile bounds, slot, binding, scratch, or owner lifetime changed");
        return 99;
    }
    if (state->budget != NULL) {
        unsigned active = atomic_fetch_add_explicit(&state->budget->active, 1U,
                                                    memory_order_acq_rel) + 1U;
        unsigned peak = atomic_load_explicit(&state->budget->peak, memory_order_relaxed);

        while (peak < active &&
               !atomic_compare_exchange_weak_explicit(&state->budget->peak,
                                                       &peak, active,
                                                       memory_order_relaxed,
                                                       memory_order_relaxed)) {
        }
        if (active > state->runtime->active_shards) {
            cpu_fail(state, "CPU callbacks exceeded the runtime P budget");
        }
    }
    *(uint64_t *)scratch = (uint64_t)begin;
    atomic_store_explicit(&state->started, 1U, memory_order_release);
    if (begin == 0U && (state->which == CPU_CANCEL_RUNNING ||
                       state->which == CPU_CANCEL_FAILURE)) {
        /* Test-only bounded CPU tile: a native thread cancels while it runs. */
        while (atomic_load_explicit(&state->native_action, memory_order_acquire) == 0U &&
               llam_now_ns() < deadline) {
            llam_pause_cpu();
        }
        if (atomic_load_explicit(&state->native_action, memory_order_acquire) == 0U) {
            cpu_fail(state, "native cancellation did not reach the running tile");
        }
    }
    if (begin == 0U && state->which == CPU_SHUTDOWN) {
        /* The live G keeps runtime storage alive until this tile returns. */
        while (!atomic_load_explicit(&state->runtime->shutdown_requested,
                                     memory_order_acquire) && llam_now_ns() < deadline) {
            llam_pause_cpu();
        }
        if (!atomic_load_explicit(&state->runtime->shutdown_requested,
                                  memory_order_acquire)) {
            cpu_fail(state, "runtime destroy did not request stop during the tile");
        }
    }
    state->output[begin] = 100 + (int)begin;
    atomic_store_explicit(&state->tile_finished, 1U, memory_order_release);
    if (atomic_load_explicit(&state->returned, memory_order_acquire) != 0U) {
        cpu_fail(state, "activation returned before its tile drained");
    }
    if (state->budget != NULL) {
        atomic_fetch_sub_explicit(&state->budget->active, 1U, memory_order_acq_rel);
    }
    return (state->which == CPU_FAILURE && begin == 2U) ||
           (state->which == CPU_CANCEL_FAILURE && begin == 0U) ? 73 : 0;
}

static magic2_cpu_plan_spec cpu_spec(cpu_state_t *state, magic2_port *port) {
    magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;

    memset(port, 0, sizeof(*port));
    port->bytes = sizeof(state->output);
    port->stride = sizeof(int);
    port->alignment = _Alignof(int);
    port->access = MAGIC2_BUFFER_WRITE;
    port->present = 1U;
    port->initialization = MAGIC2_ZERO;
    spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
    spec.schedule = MAGIC2_CPU_SCHEDULE_CALLER;
    spec.shape.count = CPU_TILES;
    spec.shape.port_count = 1U;
    spec.shape.region_count = 1U;
    spec.shape.ports = port;
    spec.tile = cpu_tile;
    spec.environment = &state->environment;
    spec.environment_bytes = sizeof(state->environment);
    spec.owner.token = state;
    spec.owner.retain = cpu_owner_retain;
    spec.owner.release = cpu_owner_release;
    spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
    spec.per_slot_scratch.bytes = sizeof(uint64_t);
    spec.per_slot_scratch.alignment = _Alignof(uint64_t);
    return spec;
}

static int cpu_state_init(cpu_state_t *state, cpu_case_t which) {
    magic2_cpu_plan_spec spec;
    magic2_port port;

    memset(state, 0, sizeof(*state));
    atomic_init(&state->failures, 0U);
    atomic_init(&state->owner_refs, 0U);
    atomic_init(&state->owner_releases, 0U);
    atomic_init(&state->calls, 0U);
    atomic_init(&state->started, 0U);
    atomic_init(&state->tile_finished, 0U);
    atomic_init(&state->returned, 0U);
    atomic_init(&state->native_action, 0U);
    state->environment.state = state;
    state->which = which;
    state->info = magic2_cpu_plan_info_initializer();
    state->status = magic2_cpu_run_status_initializer();
    for (unsigned i = 0U; i < CPU_TILES; ++i) {
        state->output[i] = -99;
    }
    spec = cpu_spec(state, &port);
    if (llam_magic2_cpu_plan_create(&spec, &state->plan) != MAGIC2_OK ||
        llam_magic2_cpu_plan_get_info(state->plan, &state->info) != MAGIC2_OK ||
        state->info.worker_limit != 1U || state->info.schedule != MAGIC2_CPU_SCHEDULE_CALLER ||
        state->info.tile_count != CPU_TILES) {
        cpu_fail(state, "CPU plan creation or metadata failed");
        return -1;
    }
    state->workspace = malloc(state->info.workspace.bytes);
    if (state->workspace == NULL) {
        cpu_fail(state, "CPU workspace allocation failed");
        return -1;
    }
    return 0;
}

static void cpu_state_finish(cpu_state_t *state) {
    llam_magic2_cpu_plan_release(&state->plan);
    free(state->workspace);
    if (atomic_load_explicit(&state->owner_refs, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state->owner_releases, memory_order_acquire) != 1U) {
        cpu_fail(state, "plan/environment reference leaked or released twice");
    }
}

static int cpu_runtime_create(llam_runtime_t **runtime) {
    llam_runtime_opts_t options;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        return -1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    return llam_runtime_create_compat(&options, sizeof(options), runtime);
}

static void cpu_request(void *arg) {
    cpu_state_t *state = arg;
    magic2_region region = {state->output, sizeof(state->output)};

    state->bridge_result = llam_magic2_cpu_run(state->plan, &region, 1U,
                                              state->workspace, state->info.workspace.bytes,
                                              &state->status, &state->run_status);
    state->bridge_errno = errno;
    if (state->which != CPU_CANCEL_BEFORE &&
        atomic_load_explicit(&state->tile_finished, memory_order_acquire) == 0U) {
        cpu_fail(state, "CPU activation returned without a drained callback");
    }
    atomic_store_explicit(&state->returned, 1U, memory_order_release);
}

static void cpu_preflight(cpu_state_t *state) {
    magic2_region region = {state->output, sizeof(state->output)};
    int result;

    if (!llam_worker_runtime_critical_enter(g_llam_tls_worker)) {
        cpu_fail(state, "could not enter CPU runtime-critical test region");
        return;
    }
    if (llam_magic2_cpu_run(state->plan, &region, 1U, state->workspace,
                           state->info.workspace.bytes, &state->status, &result) != -1 ||
        errno != EBUSY || result != MAGIC2_ENOTPROCESSED) {
        cpu_fail(state, "CPU runtime-critical call was not rejected");
    }
    (void)llam_worker_runtime_critical_leave(g_llam_tls_worker);
    if (!llam_worker_preempt_disable(g_llam_tls_worker)) {
        cpu_fail(state, "could not enter CPU preemption-disabled test region");
        return;
    }
    if (llam_magic2_cpu_run(state->plan, &region, 1U, state->workspace,
                           state->info.workspace.bytes, &state->status, &result) != -1 ||
        errno != EBUSY || result != MAGIC2_ENOTPROCESSED) {
        cpu_fail(state, "CPU preemption-disabled call was not rejected");
    }
    (void)llam_worker_preempt_enable(g_llam_tls_worker);
    /* Low-level guard leave does not publish MANAGED_SAFE; scheduler resume does. */
    llam_yield();
    region.capacity -= sizeof(int);
    if (llam_magic2_cpu_run(state->plan, &region, 1U, state->workspace,
                           state->info.workspace.bytes, &state->status, &result) != 0 ||
        result != MAGIC2_ECAPACITY) {
        cpu_fail(state, "invalid CPU span did not produce a magic2 contract error");
    }
    region.capacity = sizeof(state->output);
    state->status.tag = 0U;
    if (llam_magic2_cpu_run(state->plan, &region, 1U, state->workspace,
                           state->info.workspace.bytes, &state->status, &result) != 0 ||
        result != MAGIC2_EABI) {
        cpu_fail(state, "invalid CPU status escaped validation");
    }
    state->status = magic2_cpu_run_status_initializer();
    for (unsigned i = 0U; i < CPU_TILES; ++i) {
        if (state->output[i] != -99) {
            cpu_fail(state, "CPU preflight initialized an output port");
            break;
        }
    }
}

static void cpu_controller(void *arg) {
    cpu_state_t *state = arg;
    llam_spawn_opts_t options;
    llam_task_t *request;
    uint64_t deadline;

    cpu_preflight(state);
    state->token = llam_cancel_token_create();
    if (state->token == NULL || llam_spawn_opts_init(&options, sizeof(options)) != 0) {
        cpu_fail(state, "could not create CPU cancellation fixture");
        return;
    }
    options.cancel_token = state->token;
    if (state->which == CPU_CANCEL_BEFORE && llam_cancel_token_cancel(state->token) != 0) {
        cpu_fail(state, "CPU token could not be cancelled before submission");
    }
    request = llam_spawn_ex(cpu_request, state, &options, sizeof(options));
    if (request == NULL) {
        cpu_fail(state, "CPU task spawn failed");
        return;
    }
    deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&state->started, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->returned, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        llam_yield();
    }
    if (state->which == CPU_SUCCESS || state->which == CPU_FAILURE ||
        state->which == CPU_CANCEL_BETWEEN) {
        if (atomic_load_explicit(&state->returned, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->calls, memory_order_acquire) != 1U) {
            cpu_fail(state, "tile boundary did not let another G run with one P");
        }
        /* The running activation must retain its plan across this release/yield. */
        llam_magic2_cpu_plan_release(&state->plan);
        if (atomic_load_explicit(&state->owner_refs, memory_order_acquire) != 1U) {
            cpu_fail(state, "CPU plan was not retained across tile yields");
        }
    }
    if (state->which == CPU_CANCEL_BETWEEN && llam_cancel_token_cancel(state->token) != 0) {
        cpu_fail(state, "could not cancel CPU activation between tiles");
    }
    if (llam_join(request) != 0) {
        cpu_fail(state, "CPU task join failed");
    }
    if (llam_cancel_token_destroy(state->token) != 0) {
        cpu_fail(state, "CPU cancellation token did not drain");
    }
    state->token = NULL;
}

static void *cpu_native_cancel(void *arg) {
    cpu_state_t *state = arg;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);

    while (atomic_load_explicit(&state->started, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        llam_pause_cpu();
    }
    if (atomic_load_explicit(&state->started, memory_order_acquire) == 0U ||
        llam_cancel_token_cancel(state->token) != 0) {
        cpu_fail(state, "host could not cancel the running CPU callback");
    }
    if (atomic_load_explicit(&state->returned, memory_order_acquire) != 0U) {
        cpu_fail(state, "cancellation completed before the running callback returned");
    }
    atomic_store_explicit(&state->native_action, 1U, memory_order_release);
    return NULL;
}

static void cpu_check_result(cpu_state_t *state) {
    bool cancelled = state->which == CPU_CANCEL_BEFORE || state->which == CPU_CANCEL_BETWEEN ||
                     state->which == CPU_CANCEL_RUNNING || state->which == CPU_CANCEL_FAILURE ||
                     state->which == CPU_SHUTDOWN;
    bool failed = state->which == CPU_FAILURE || state->which == CPU_CANCEL_FAILURE;
    unsigned expected_calls = state->which == CPU_CANCEL_BEFORE ? 0U :
                              state->which == CPU_FAILURE ? 3U :
                              cancelled ? 1U : CPU_TILES;
    int expected_status = state->which == CPU_CANCEL_BEFORE ? MAGIC2_ENOTPROCESSED :
                          failed ? MAGIC2_EIMPLEMENTATION :
                          cancelled ? MAGIC2_ECANCELLED : MAGIC2_OK;

    if (atomic_load_explicit(&state->returned, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state->calls, memory_order_acquire) != expected_calls ||
        state->bridge_result != (cancelled ? -1 : 0) ||
        (cancelled && state->bridge_errno != ECANCELED) ||
        state->run_status != expected_status) {
        cpu_fail(state, "CPU completion/cancellation status or exact callback count changed");
    }
    if (state->which != CPU_CANCEL_BEFORE &&
        (state->status.runtime_status != expected_status ||
         state->status.implementation_status != (failed ? 73 : 0) ||
         state->status.state != (failed ? MAGIC2_CPU_RUN_FAILED :
                                cancelled ? MAGIC2_CPU_RUN_CANCELLED : MAGIC2_CPU_RUN_COMPLETE) ||
         state->status.failed_tile != (failed ? (size_t)(expected_calls - 1U) : SIZE_MAX) ||
         state->status.total_tiles != CPU_TILES || state->status.used_workers != 1U ||
         state->status.completed_tiles != expected_calls - (failed ? 1U : 0U))) {
        cpu_fail(state, "CPU native results or partial-failure accounting changed");
    }
    for (unsigned i = 0U; i < CPU_TILES; ++i) {
        int expected = state->which == CPU_CANCEL_BEFORE ? -99 :
                       i < expected_calls ? 100 + (int)i : 0;

        if (state->output[i] != expected) {
            cpu_fail(state, "CPU writes were replayed, rolled back, or made before admission");
            break;
        }
    }
}

static int cpu_test_case(cpu_case_t which) {
    cpu_state_t state;
    llam_task_t *controller = NULL;
    llam_runtime_stats_t stats;
    pthread_t native_thread;
    bool native_started = false;

    if (cpu_state_init(&state, which) != 0 || cpu_runtime_create(&state.runtime) != 0) {
        cpu_fail(&state, "CPU runtime/plan fixture creation failed");
        goto done;
    }
    if (which == CPU_CANCEL_RUNNING || which == CPU_CANCEL_FAILURE) {
        if (pthread_create(&native_thread, NULL, cpu_native_cancel, &state) != 0) {
            cpu_fail(&state, "CPU native cancellation thread creation failed");
            goto done;
        }
        native_started = true;
    }
    controller = llam_runtime_spawn_ex(state.runtime, cpu_controller, &state, NULL, 0U);
    if (controller == NULL || llam_runtime_run_handle(state.runtime) != 0 ||
        llam_join(controller) != 0) {
        cpu_fail(&state, "CPU controller execution failed");
    }
    if (native_started) {
        (void)pthread_join(native_thread, NULL);
    }
    cpu_check_result(&state);
    if (llam_runtime_collect_stats_ex_handle(state.runtime, &stats, sizeof(stats)) != 0 ||
        stats.active_workers != 1U || stats.blocking_calls != 0U) {
        cpu_fail(&state, "direct CPU execution used extra workers or the blocking pool");
    }
done:
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    cpu_state_finish(&state);
    return atomic_load_explicit(&state.failures, memory_order_acquire) != 0U;
}

static int cpu_test_budget(void) {
    cpu_state_t states[CPU_TASKS];
    llam_task_t *tasks[CPU_TASKS] = {0};
    llam_runtime_t *runtime = NULL;
    cpu_budget_t budget;
    llam_runtime_stats_t stats;
    unsigned initialized = 0U;
    int failed = 0;

    atomic_init(&budget.active, 0U);
    atomic_init(&budget.peak, 0U);
    if (cpu_runtime_create(&runtime) != 0) {
        return 1;
    }
    for (unsigned i = 0U; i < CPU_TASKS; ++i) {
        initialized += 1U;
        if (cpu_state_init(&states[i], CPU_SUCCESS) != 0) {
            failed = 1;
            break;
        }
        states[i].runtime = runtime;
        states[i].budget = &budget;
        tasks[i] = llam_runtime_spawn_ex(runtime, cpu_request, &states[i], NULL, 0U);
        if (tasks[i] == NULL) {
            failed = 1;
            break;
        }
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        failed = 1;
    }
    for (unsigned i = 0U; i < initialized; ++i) {
        if (tasks[i] != NULL && llam_join(tasks[i]) != 0) {
            failed = 1;
        }
        cpu_check_result(&states[i]);
    }
    if (atomic_load_explicit(&budget.active, memory_order_acquire) != 0U ||
        atomic_load_explicit(&budget.peak, memory_order_acquire) != 1U ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0 ||
        stats.active_workers != 1U || stats.blocking_calls != 0U) {
        fprintf(stderr, "test_magic2_cpu_internal: concurrent activations exceeded one P\n");
        failed = 1;
    }
    llam_runtime_destroy(runtime);
    for (unsigned i = 0U; i < initialized; ++i) {
        cpu_state_finish(&states[i]);
        failed |= atomic_load_explicit(&states[i].failures, memory_order_acquire) != 0U;
    }
    return failed;
}

static void *cpu_runtime_driver(void *arg) {
    cpu_state_t *state = arg;

    state->driver_result = llam_runtime_run_handle(state->runtime);
    return NULL;
}

static int cpu_test_shutdown(void) {
    cpu_state_t state;
    llam_runtime_t *peer = NULL;
    llam_task_t *task;
    pthread_t driver;
    uint64_t deadline;

    if (cpu_state_init(&state, CPU_SHUTDOWN) != 0 ||
        cpu_runtime_create(&state.runtime) != 0 || cpu_runtime_create(&peer) != 0) {
        cpu_fail(&state, "CPU shutdown fixture creation failed");
        goto done;
    }
    task = llam_runtime_spawn_ex(state.runtime, cpu_request, &state, NULL, 0U);
    if (task == NULL || pthread_create(&driver, NULL, cpu_runtime_driver, &state) != 0) {
        cpu_fail(&state, "CPU shutdown task/driver creation failed");
        goto done;
    }
    deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&state.started, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        llam_pause_cpu();
    }
    llam_runtime_destroy(state.runtime);
    (void)pthread_join(driver, NULL);
    state.runtime = NULL;
    cpu_check_result(&state);
    if (state.driver_result != 0 ||
        llam_magic2_cpu_plan_get_info(state.plan, &state.info) != MAGIC2_OK ||
        atomic_load_explicit(&state.owner_refs, memory_order_acquire) != 1U) {
        cpu_fail(&state, "destroy returned before CPU drain or consumed the caller's plan");
    }
    /* A plan has no runtime owner; reuse it on the peer created before stop. */
    state.runtime = peer;
    peer = NULL;
    state.which = CPU_SUCCESS;
    state.status = magic2_cpu_run_status_initializer();
    atomic_store_explicit(&state.calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&state.started, 0U, memory_order_relaxed);
    atomic_store_explicit(&state.tile_finished, 0U, memory_order_relaxed);
    atomic_store_explicit(&state.returned, 0U, memory_order_relaxed);
    for (unsigned i = 0U; i < CPU_TILES; ++i) {
        state.output[i] = -99;
    }
    task = llam_runtime_spawn_ex(state.runtime, cpu_request, &state, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(state.runtime) != 0 || llam_join(task) != 0) {
        cpu_fail(&state, "stopping one CPU runtime stopped its peer");
    }
    cpu_check_result(&state);
done:
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    if (peer != NULL) {
        llam_runtime_destroy(peer);
    }
    cpu_state_finish(&state);
    return atomic_load_explicit(&state.failures, memory_order_acquire) != 0U;
}

static int cpu_test_contract(void) {
    cpu_state_t state;
    magic2_port port;
    magic2_cpu_plan_spec spec;
    magic2_cpu_plan *candidate = NULL;
    magic2_cpu_plan *retained;
    magic2_region region;
    int result;

    if (cpu_state_init(&state, CPU_SUCCESS) != 0) {
        cpu_state_finish(&state);
        return 1;
    }
    spec = cpu_spec(&state, &port);
    /* Unsupported schedules must not retain even an owner with side effects. */
    spec.schedule = MAGIC2_CPU_SCHEDULE_STATIC;
    if (llam_magic2_cpu_plan_create(&spec, &candidate) != MAGIC2_EUNSUPPORTED || candidate != NULL) {
        cpu_fail(&state, "static/native-pool CPU schedule was accepted");
    }
    spec.schedule = MAGIC2_CPU_SCHEDULE_DYNAMIC;
    if (llam_magic2_cpu_plan_create(&spec, &candidate) != MAGIC2_EUNSUPPORTED || candidate != NULL) {
        cpu_fail(&state, "dynamic/native-pool CPU schedule was accepted");
    }
    if (atomic_load_explicit(&state.owner_refs, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.owner_releases, memory_order_acquire) != 0U) {
        cpu_fail(&state, "unsupported CPU schedules invoked owner callbacks");
    }
    spec.schedule = MAGIC2_CPU_SCHEDULE_CALLER;
    spec.worker_limit = 2U;
    if (llam_magic2_cpu_plan_create(&spec, &candidate) != MAGIC2_EINVAL || candidate != NULL) {
        cpu_fail(&state, "caller CPU plan accepted a second worker slot");
    }
    spec.worker_limit = 1U;
    spec.tag = 0U;
    if (llam_magic2_cpu_plan_create(&spec, &candidate) != MAGIC2_EABI) {
        cpu_fail(&state, "malformed CPU plan escaped validation");
    }
    if (llam_magic2_cpu_plan_retain(state.plan) != MAGIC2_OK) {
        cpu_fail(&state, "CPU plan retain failed");
    } else {
        retained = state.plan;
        llam_magic2_cpu_plan_release(&retained);
        if (retained != NULL || llam_magic2_cpu_plan_get_info(state.plan, &state.info) != MAGIC2_OK) {
            cpu_fail(&state, "CPU plan release consumed another owning reference");
        }
    }
    region.base = state.output;
    region.capacity = sizeof(state.output);
    if (llam_magic2_cpu_run(state.plan, &region, 1U, state.workspace,
                           state.info.workspace.bytes, &state.status, &result) != -1 ||
        errno != EBUSY || result != MAGIC2_ENOTPROCESSED ||
        llam_magic2_cpu_run(state.plan, &region, 1U, state.workspace,
                           state.info.workspace.bytes, &state.status, NULL) != -1 ||
        errno != EINVAL || state.output[0] != -99) {
        cpu_fail(&state, "unmanaged CPU execution or missing result was accepted");
    }
    llam_magic2_cpu_plan_release(&candidate);
    cpu_state_finish(&state);
    return atomic_load_explicit(&state.failures, memory_order_acquire) != 0U;
}

int test_magic2_cpu_internal(void) {
    if (cpu_test_contract() != 0) {
        return 1;
    }
    for (cpu_case_t which = CPU_SUCCESS; which <= CPU_CANCEL_FAILURE; ++which) {
        if (cpu_test_case(which) != 0) {
            return 1;
        }
    }
    if (cpu_test_budget() != 0 || cpu_test_shutdown() != 0) {
        return 1;
    }
    puts("test_magic2_cpu_internal: managed tiles, budget, cancellation, ownership, and shutdown passed");
    return 0;
}
