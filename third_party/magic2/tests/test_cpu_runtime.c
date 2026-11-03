/* Verify persistent CPU plans, ownership, binding, failure, and admission. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_environment {
    size_t fail_begin;
    int fail_status;
    int block;
    uint32_t delay_iterations;
} test_environment;

typedef struct test_owner {
    int refs;
} test_owner;

static volatile uint32_t test_started;
static volatile uint32_t test_gate;

static int test_owner_retain(void *opaque) {
    test_owner *owner = (test_owner *)opaque;
    ++owner->refs;
    return 0;
}

static void test_owner_release(void *opaque) {
    test_owner *owner = (test_owner *)opaque;
    --owner->refs;
}

static int test_tile(
    const void *opaque_environment, void *const *ports,
    size_t begin, size_t end, uint32_t slot, void *scratch) {
    const test_environment *environment =
        (const test_environment *)opaque_environment;
    int *output = (int *)ports[0];
    const int *input = (const int *)ports[1];
    size_t index;
    assert(slot < 8u);
    assert(scratch != NULL);
    assert(((uintptr_t)scratch & 63u) == 0u);
    memset(scratch, (int)(slot + 1u), 64u);
    if (environment->block) {
        __atomic_store_n(&test_started, 1u, __ATOMIC_RELEASE);
        while (__atomic_load_n(&test_gate, __ATOMIC_ACQUIRE) == 0u)
            sched_yield();
    }
    if (environment->delay_iterations != 0u) {
        volatile uint32_t value = slot + 1u;
        uint32_t iteration;
        for (iteration = 0u; iteration < environment->delay_iterations;
             ++iteration)
            value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        if (value == 0u) return -999;
    }
    if (environment->fail_status != 0 && begin == environment->fail_begin)
        return environment->fail_status;
    for (index = begin; index < end; ++index) {
        assert(output[index] == 0);
        output[index] = input[index] * 3 + 1;
    }
    return 0;
}

typedef struct aligned_block {
    void *allocation;
    void *pointer;
} aligned_block;

static aligned_block test_aligned_alloc(size_t bytes, size_t alignment) {
    aligned_block block;
    uintptr_t raw;
    block.allocation = malloc(bytes + alignment - 1u);
    assert(block.allocation != NULL);
    raw = (uintptr_t)block.allocation;
    block.pointer = (void *)((raw + alignment - 1u) & ~(alignment - 1u));
    return block;
}

static magic2_cpu_plan *test_make_plan(
    const test_environment *environment, test_owner *owner,
    magic2_port *ports, size_t count, size_t grain,
    uint32_t workers, uint32_t schedule) {
    magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;
    magic2_cpu_plan *plan = NULL;
    spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
    spec.schedule = schedule;
    spec.stable_id.low = count;
    spec.stable_id.high = grain;
    spec.shape.operation_id.low = UINT64_C(0x1001);
    spec.shape.semantic_id.low = UINT64_C(0x2001);
    spec.shape.count = count;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.tile = test_tile;
    spec.environment = environment;
    spec.environment_bytes = sizeof(*environment);
    spec.owner.token = owner;
    spec.owner.retain = test_owner_retain;
    spec.owner.release = test_owner_release;
    spec.grain = grain;
    spec.worker_limit = workers;
    spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
    spec.per_slot_scratch.bytes = 64u;
    spec.per_slot_scratch.alignment = 64u;
    assert(magic2_cpu_plan_create(&spec, &plan) == MAGIC2_OK);
    assert(plan != NULL);
    return plan;
}

static void test_run_success(
    magic2_cpu_executor *executor, uint32_t schedule) {
    enum { count = 100003 };
    test_environment environment = { SIZE_MAX, 0, 0, 0u };
    test_owner owner = { 0 };
    magic2_port ports[2];
    magic2_region regions[2];
    magic2_cpu_plan *plan;
    magic2_cpu_plan_info info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status status = MAGIC2_CPU_RUN_STATUS_INIT;
    magic2_cpu_frame frame = MAGIC2_CPU_FRAME_INIT;
    aligned_block workspace;
    int *input = (int *)malloc((size_t)count * sizeof(*input));
    int *output = (int *)malloc((size_t)count * sizeof(*output));
    size_t index;
    const uint32_t workers =
        schedule == MAGIC2_CPU_SCHEDULE_CALLER ? 1u : 4u;
    assert(input != NULL && output != NULL);
    for (index = 0u; index < count; ++index) input[index] = (int)index;
    memset(output, 0x5a, (size_t)count * sizeof(*output));
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = (size_t)count * sizeof(*output);
    ports[0].stride = sizeof(*output);
    ports[0].alignment = sizeof(*output);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1].bytes = (size_t)count * sizeof(*input);
    ports[1].stride = sizeof(*input);
    ports[1].alignment = sizeof(*input);
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].present = 1u;
    plan = test_make_plan(
        &environment, &owner, ports, count, 1024u, workers, schedule);
    assert(owner.refs == 1);
    assert(magic2_cpu_plan_get_info(plan, &info) == MAGIC2_OK);
    assert(info.tile_count == 98u);
    assert(info.worker_limit == workers);
    assert(info.workspace.alignment == 64u);
    regions[0].base = output;
    regions[0].capacity = (size_t)count * sizeof(*output);
    regions[1].base = input;
    regions[1].capacity = (size_t)count * sizeof(*input);
    workspace = test_aligned_alloc(info.workspace.bytes, info.workspace.alignment);
    assert(magic2_cpu_plan_run(
        plan, executor, regions, 2u, workspace.pointer,
        info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.state == MAGIC2_CPU_RUN_COMPLETE);
    assert(status.completed_tiles == info.tile_count);
    assert(status.total_tiles == info.tile_count);
    assert(status.used_workers == workers);
    assert(status.failed_tile == SIZE_MAX);
    for (index = 0u; index < count; ++index)
        assert(output[index] == input[index] * 3 + 1);
    assert(magic2_cpu_frame_init(
        &frame, workspace.pointer, info.workspace.bytes) == MAGIC2_OK);
    assert(magic2_cpu_frame_bind(plan, regions, 2u, &frame) == MAGIC2_OK);
    magic2_cpu_plan_release(&plan);
    assert(plan == NULL && owner.refs == 1);
    status = magic2_cpu_run_status_initializer();
    assert(magic2_cpu_frame_run(&frame, executor, &status) == MAGIC2_OK);
    assert(status.completed_tiles == info.tile_count);
    status = magic2_cpu_run_status_initializer();
    assert(magic2_cpu_frame_run(&frame, executor, &status) == MAGIC2_OK);
    assert(magic2_cpu_frame_unbind(&frame) == MAGIC2_OK);
    assert(owner.refs == 0);
    free(workspace.allocation);
    free(output);
    free(input);
}

static void test_overlap_and_failure(magic2_cpu_executor *executor) {
    enum { count = 4096 };
    test_environment environment = { 1024u, -77, 0, 0u };
    test_owner owner = { 0 };
    magic2_port ports[2];
    magic2_region regions[2];
    magic2_cpu_plan *plan;
    magic2_cpu_plan_info info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status status = MAGIC2_CPU_RUN_STATUS_INIT;
    aligned_block workspace;
    int *storage = (int *)calloc((size_t)count * 2u, sizeof(*storage));
    size_t index;
    assert(storage != NULL);
    for (index = 0u; index < count; ++index) storage[count + index] = (int)index;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = (size_t)count * sizeof(int);
    ports[0].stride = sizeof(int);
    ports[0].alignment = sizeof(int);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].initialization = MAGIC2_PRESERVE;
    plan = test_make_plan(
        &environment, &owner, ports, count, 1024u, 4u,
        MAGIC2_CPU_SCHEDULE_STATIC);
    assert(magic2_cpu_plan_get_info(plan, &info) == MAGIC2_OK);
    workspace = test_aligned_alloc(info.workspace.bytes, info.workspace.alignment);
    regions[0].base = storage;
    regions[0].capacity = (size_t)count * sizeof(int);
    regions[1].base = storage + count;
    regions[1].capacity = (size_t)count * sizeof(int);
    assert(magic2_cpu_plan_run(
        plan, executor, regions, 2u, workspace.pointer,
        info.workspace.bytes, &status) == MAGIC2_EIMPLEMENTATION);
    assert(status.state == MAGIC2_CPU_RUN_FAILED);
    assert(status.implementation_status == -77);
    assert(status.failed_tile == 1u);
    assert(status.completed_tiles < status.total_tiles);

    status = magic2_cpu_run_status_initializer();
    regions[1].base = storage + count / 2u;
    assert(magic2_cpu_plan_run(
        plan, executor, regions, 2u, workspace.pointer,
        info.workspace.bytes, &status) == MAGIC2_EOVERLAP);
    magic2_cpu_plan_release(&plan);
    assert(owner.refs == 0);
    free(workspace.allocation);
    free(storage);
}

typedef struct run_thread_args {
    magic2_cpu_plan *plan;
    magic2_cpu_executor *executor;
    magic2_region regions[2];
    void *workspace;
    size_t workspace_bytes;
    int result;
} run_thread_args;

static void *test_run_thread(void *opaque) {
    run_thread_args *args = (run_thread_args *)opaque;
    magic2_cpu_run_status status = MAGIC2_CPU_RUN_STATUS_INIT;
    args->result = magic2_cpu_plan_run(
        args->plan, args->executor, args->regions, 2u,
        args->workspace, args->workspace_bytes, &status);
    return NULL;
}

static void test_busy_and_drain(magic2_cpu_executor *executor) {
    enum { count = 32 };
    test_environment environment = { SIZE_MAX, 0, 1, 0u };
    test_owner owner = { 0 };
    magic2_port ports[2];
    magic2_cpu_plan *plan;
    magic2_cpu_plan_info info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status second_status = MAGIC2_CPU_RUN_STATUS_INIT;
    run_thread_args args;
    aligned_block first_workspace;
    aligned_block second_workspace;
    pthread_t thread;
    int first_input[count];
    int first_output[count];
    int second_input[count];
    int second_output[count];
    magic2_region second_regions[2];
    size_t index;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = sizeof(first_output);
    ports[0].stride = sizeof(int);
    ports[0].alignment = sizeof(int);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].initialization = MAGIC2_PRESERVE;
    plan = test_make_plan(
        &environment, &owner, ports, count, count, 1u,
        MAGIC2_CPU_SCHEDULE_STATIC);
    assert(magic2_cpu_plan_get_info(plan, &info) == MAGIC2_OK);
    first_workspace = test_aligned_alloc(
        info.workspace.bytes, info.workspace.alignment);
    second_workspace = test_aligned_alloc(
        info.workspace.bytes, info.workspace.alignment);
    for (index = 0u; index < count; ++index) {
        first_input[index] = (int)index;
        second_input[index] = (int)index + 10;
        first_output[index] = 7;
        second_output[index] = 9;
    }
    memset(&args, 0, sizeof(args));
    args.plan = plan;
    args.executor = executor;
    args.regions[0].base = first_output;
    args.regions[0].capacity = sizeof(first_output);
    args.regions[1].base = first_input;
    args.regions[1].capacity = sizeof(first_input);
    args.workspace = first_workspace.pointer;
    args.workspace_bytes = info.workspace.bytes;
    second_regions[0].base = second_output;
    second_regions[0].capacity = sizeof(second_output);
    second_regions[1].base = second_input;
    second_regions[1].capacity = sizeof(second_input);
    __atomic_store_n(&test_started, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&test_gate, 0u, __ATOMIC_RELAXED);
    assert(pthread_create(&thread, NULL, test_run_thread, &args) == 0);
    while (__atomic_load_n(&test_started, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();
    {
        test_environment fallback_environment = { SIZE_MAX, 0, 0, 0u };
        magic2_cpu_plan *parallel_plan = test_make_plan(
            &fallback_environment, &owner, ports, count, 8u, 4u,
            MAGIC2_CPU_SCHEDULE_STATIC);
        magic2_cpu_plan *caller_plan = test_make_plan(
            &fallback_environment, &owner, ports, count, count, 1u,
            MAGIC2_CPU_SCHEDULE_CALLER);
        magic2_cpu_plan *family_plans[2] = { parallel_plan, caller_plan };
        magic2_cpu_family_config family_config =
            MAGIC2_CPU_FAMILY_CONFIG_INIT;
        magic2_cpu_family_info family_info = MAGIC2_CPU_FAMILY_INFO_INIT;
        magic2_cpu_family_status family_status = MAGIC2_CPU_FAMILY_STATUS_INIT;
        magic2_cpu_family *family = NULL;
        aligned_block family_workspace;
        assert(magic2_cpu_family_create(
            &family_config, family_plans, 2u, &family) == MAGIC2_OK);
        assert(magic2_cpu_family_get_info(family, &family_info) == MAGIC2_OK);
        family_workspace = test_aligned_alloc(
            family_info.workspace.bytes, family_info.workspace.alignment);
        assert(magic2_cpu_family_run(
            family, executor, NULL, second_regions, 2u,
            family_workspace.pointer, family_info.workspace.bytes,
            &family_status) == MAGIC2_OK);
        assert(family_status.selected_index == 0u);
        assert(family_status.executed_index == 1u);
        assert(family_status.fallback_used == 1u);
        for (index = 0u; index < count; ++index) {
            assert(second_output[index] == second_input[index] * 3 + 1);
            second_output[index] = 9;
        }
        assert(magic2_cpu_family_destroy(&family) == MAGIC2_OK);
        magic2_cpu_plan_release(&caller_plan);
        magic2_cpu_plan_release(&parallel_plan);
        free(family_workspace.allocation);
    }
    assert(magic2_cpu_plan_run(
        plan, executor, second_regions, 2u, second_workspace.pointer,
        info.workspace.bytes, &second_status) == MAGIC2_EBUSY);
    for (index = 0u; index < count; ++index) assert(second_output[index] == 9);
    {
        magic2_cpu_executor *borrowed = executor;
        assert(magic2_cpu_executor_destroy(&borrowed) == MAGIC2_EBUSY);
        assert(borrowed == executor);
    }
    __atomic_store_n(&test_gate, 1u, __ATOMIC_RELEASE);
    assert(pthread_join(thread, NULL) == 0);
    assert(args.result == MAGIC2_OK);
    magic2_cpu_plan_release(&plan);
    assert(owner.refs == 0);
    free(second_workspace.allocation);
    free(first_workspace.allocation);
}

static void test_family_selection(magic2_cpu_executor *executor) {
    enum { count = 4096 };
    test_environment slow_environment = { SIZE_MAX, 0, 0, 2000000u };
    test_environment fast_environment = { SIZE_MAX, 0, 0, 0u };
    test_owner owner = { 0 };
    magic2_port ports[2];
    magic2_region regions[2];
    magic2_cpu_plan *caller_plan;
    magic2_cpu_plan *worker_plan;
    magic2_cpu_plan *plans[2];
    magic2_cpu_family_config config = MAGIC2_CPU_FAMILY_CONFIG_INIT;
    magic2_cpu_family_info info = MAGIC2_CPU_FAMILY_INFO_INIT;
    magic2_cpu_context context = MAGIC2_CPU_CONTEXT_INIT;
    magic2_cpu_family_status status = MAGIC2_CPU_FAMILY_STATUS_INIT;
    magic2_cpu_family *family = NULL;
    aligned_block workspace;
    int input[count];
    int output[count];
    size_t index;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = sizeof(output);
    ports[0].stride = sizeof(int);
    ports[0].alignment = sizeof(int);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].initialization = MAGIC2_PRESERVE;
    caller_plan = test_make_plan(
        &slow_environment, &owner, ports, count, count, 1u,
        MAGIC2_CPU_SCHEDULE_CALLER);
    worker_plan = test_make_plan(
        &fast_environment, &owner, ports, count, 1024u, 4u,
        MAGIC2_CPU_SCHEDULE_STATIC);
    plans[0] = caller_plan;
    plans[1] = worker_plan;
    config.bucket_count = 2u;
    config.initial_samples = 1u;
    config.exploration_interval = 1000u;
    config.ewma_alpha_permille = 1000u;
    config.switch_hysteresis_permille = 0u;
    {
        magic2_cpu_plan *duplicates[2] = { caller_plan, caller_plan };
        magic2_cpu_family *rejected = NULL;
        assert(magic2_cpu_family_create(
            &config, duplicates, 2u, &rejected) == MAGIC2_ECONFLICT);
        assert(rejected == NULL);
    }
    assert(magic2_cpu_family_create(
        &config, plans, 2u, &family) == MAGIC2_OK);
    assert(owner.refs == 2);
    assert(magic2_cpu_family_get_info(family, &info) == MAGIC2_OK);
    assert(info.plan_count == 2u && info.bucket_count == 2u);
    workspace = test_aligned_alloc(info.workspace.bytes, info.workspace.alignment);
    for (index = 0u; index < count; ++index) input[index] = (int)index;
    memset(output, 0x4c, sizeof(output));
    regions[0].base = output;
    regions[0].capacity = sizeof(output);
    regions[1].base = input;
    regions[1].capacity = sizeof(input);

    assert(magic2_cpu_family_run(
        family, executor, &context, regions, 2u,
        workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.selected_index == 0u && status.executed_index == 0u);
    status = magic2_cpu_family_status_initializer();
    assert(magic2_cpu_family_run(
        family, executor, &context, regions, 2u,
        workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.selected_index == 1u && status.executed_index == 1u);
    status = magic2_cpu_family_status_initializer();
    assert(magic2_cpu_family_run(
        family, executor, &context, regions, 2u,
        workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.selected_index == 1u && status.observation_valid == 1u);
    {
        magic2_cpu_evidence slow = MAGIC2_CPU_EVIDENCE_INIT;
        magic2_cpu_evidence fast = MAGIC2_CPU_EVIDENCE_INIT;
        assert(magic2_cpu_family_get_evidence(
            family, 0u, 0u, &slow) == MAGIC2_OK);
        assert(magic2_cpu_family_get_evidence(
            family, 0u, 1u, &fast) == MAGIC2_OK);
        assert(slow.samples == 1u);
        assert(fast.samples == 2u && fast.selected == 1u);
        assert(fast.ewma_ns < slow.ewma_ns);
    }
    for (index = 0u; index < count; ++index)
        assert(output[index] == input[index] * 3 + 1);

    context.slot_budget = 1u;
    status = magic2_cpu_family_status_initializer();
    assert(magic2_cpu_family_run(
        family, executor, &context, regions, 2u,
        workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.selected_index == 0u);
    context.bucket = 1u;
    context.slot_budget = 0u;
    status = magic2_cpu_family_status_initializer();
    assert(magic2_cpu_family_run(
        family, executor, &context, regions, 2u,
        workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
    assert(status.selected_index == 0u);
    {
        size_t profile_size = 0u;
        size_t actual_size = 0u;
        unsigned char *profile;
        magic2_cpu_family *imported_family = NULL;
        magic2_cpu_profile_import_result import_result =
            MAGIC2_CPU_PROFILE_IMPORT_RESULT_INIT;
        magic2_cpu_evidence imported_evidence = MAGIC2_CPU_EVIDENCE_INIT;
        assert(magic2_cpu_family_profile_export(
            family, NULL, 0u, &profile_size) == MAGIC2_OK);
        assert(profile_size > 80u);
        profile = (unsigned char *)malloc(profile_size);
        assert(profile != NULL);
        assert(magic2_cpu_family_profile_export(
            family, profile, profile_size, &actual_size) == MAGIC2_OK);
        assert(actual_size == profile_size);
        assert(magic2_cpu_family_create(
            &config, plans, 2u, &imported_family) == MAGIC2_OK);
        {
            size_t mutation;
            for (mutation = 0u; mutation < profile_size; ++mutation) {
                profile[mutation] ^= 1u;
                import_result = magic2_cpu_profile_import_result_initializer();
                assert(magic2_cpu_family_profile_import(
                    imported_family, profile, profile_size, 500u,
                    &import_result) == MAGIC2_EPROFILE);
                profile[mutation] ^= 1u;
            }
        }
        import_result = magic2_cpu_profile_import_result_initializer();
        assert(magic2_cpu_family_profile_import(
            imported_family, profile, profile_size, 500u,
            &import_result) == MAGIC2_OK);
        assert(import_result.imported_records >= 2u);
        assert(magic2_cpu_family_get_evidence(
            imported_family, 0u, 1u, &imported_evidence) == MAGIC2_OK);
        assert(imported_evidence.imported_samples >= 1u);
        assert(imported_evidence.local_samples == 0u);
        context.bucket = 0u;
        status = magic2_cpu_family_status_initializer();
        assert(magic2_cpu_family_run(
            imported_family, executor, &context, regions, 2u,
            workspace.pointer, info.workspace.bytes, &status) == MAGIC2_OK);
        assert(status.selected_index == 0u);
        profile[profile_size - 1u] ^= 1u;
        import_result = magic2_cpu_profile_import_result_initializer();
        assert(magic2_cpu_family_profile_import(
            imported_family, profile, profile_size, 500u,
            &import_result) == MAGIC2_EPROFILE);
        assert(magic2_cpu_family_destroy(&imported_family) == MAGIC2_OK);
        free(profile);
    }
    assert(magic2_cpu_family_reset(family) == MAGIC2_OK);
    assert(magic2_cpu_family_destroy(&family) == MAGIC2_OK);
    magic2_cpu_plan_release(&worker_plan);
    magic2_cpu_plan_release(&caller_plan);
    assert(owner.refs == 0);
    free(workspace.allocation);
}

static void test_async_ticket(magic2_cpu_executor *executor) {
    enum { count = 256 };
    test_environment environment = { SIZE_MAX, 0, 1, 0u };
    test_owner owner = { 0 };
    magic2_port ports[2];
    magic2_region regions[2];
    magic2_cpu_plan *plan;
    magic2_cpu_plan_info info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status status = MAGIC2_CPU_RUN_STATUS_INIT;
    magic2_cpu_ticket *ticket = NULL;
    aligned_block workspace;
    int input[count];
    int output[count];
    size_t index;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = sizeof(output);
    ports[0].stride = sizeof(int);
    ports[0].alignment = sizeof(int);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].initialization = MAGIC2_PRESERVE;
    plan = test_make_plan(
        &environment, &owner, ports, count, count, 1u,
        MAGIC2_CPU_SCHEDULE_STATIC);
    assert(magic2_cpu_plan_get_info(plan, &info) == MAGIC2_OK);
    workspace = test_aligned_alloc(info.workspace.bytes, info.workspace.alignment);
    for (index = 0u; index < count; ++index) {
        input[index] = (int)index;
        output[index] = 19;
    }
    regions[0].base = output;
    regions[0].capacity = sizeof(output);
    regions[1].base = input;
    regions[1].capacity = sizeof(input);
    __atomic_store_n(&test_started, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&test_gate, 0u, __ATOMIC_RELAXED);
    assert(magic2_cpu_plan_submit(
        plan, executor, regions, 2u, workspace.pointer,
        info.workspace.bytes, &ticket) == MAGIC2_OK);
    while (__atomic_load_n(&test_started, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();
    assert(magic2_cpu_ticket_poll(ticket, &status) == MAGIC2_OK);
    assert(status.state == MAGIC2_CPU_RUN_PENDING);
    status = magic2_cpu_run_status_initializer();
    assert(magic2_cpu_ticket_wait(ticket, 0u, &status) == MAGIC2_EBUSY);
    assert(status.state == MAGIC2_CPU_RUN_PENDING);
    assert(magic2_cpu_ticket_release(&ticket) == MAGIC2_EBUSY);
    {
        magic2_cpu_executor *borrowed = executor;
        assert(magic2_cpu_executor_destroy(&borrowed) == MAGIC2_EBUSY);
        assert(borrowed == executor);
    }
    assert(magic2_cpu_ticket_cancel(ticket) == MAGIC2_OK);
    assert(magic2_cpu_ticket_release(&ticket) == MAGIC2_EBUSY);
    __atomic_store_n(&test_gate, 1u, __ATOMIC_RELEASE);
    status = magic2_cpu_run_status_initializer();
    assert(magic2_cpu_ticket_wait(
        ticket, UINT64_MAX, &status) == MAGIC2_ECANCELLED);
    assert(status.state == MAGIC2_CPU_RUN_CANCELLED);
    assert(status.completed_tiles == 1u);
    assert(magic2_cpu_ticket_release(&ticket) == MAGIC2_OK);
    assert(ticket == NULL);
    for (index = 0u; index < count; ++index)
        assert(output[index] == input[index] * 3 + 1);
    magic2_cpu_plan_release(&plan);
    assert(owner.refs == 0);
    free(workspace.allocation);
}

int main(void) {
    magic2_cpu_executor_config config = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor_info info = MAGIC2_CPU_EXECUTOR_INFO_INIT;
    magic2_cpu_resource_info resources = MAGIC2_CPU_RESOURCE_INFO_INIT;
    magic2_cpu_executor *executor = NULL;
    assert(magic2_cpu_query_resources(&resources) == MAGIC2_OK);
    assert(resources.recommended_workers >= 1u);
    {
        magic2_cpu_executor_config automatic =
            MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
        magic2_cpu_executor_info automatic_info = MAGIC2_CPU_EXECUTOR_INFO_INIT;
        magic2_cpu_executor *automatic_executor = NULL;
        automatic.flags = MAGIC2_CPU_EXECUTOR_AUTO_WORKERS;
        automatic.max_workers = 2u;
        assert(magic2_cpu_executor_create(
            &automatic, &automatic_executor) == MAGIC2_OK);
        assert(magic2_cpu_executor_get_info(
            automatic_executor, &automatic_info) == MAGIC2_OK);
        assert(automatic_info.max_workers >= 1u &&
            automatic_info.max_workers <= 2u);
        assert(magic2_cpu_executor_destroy(&automatic_executor) == MAGIC2_OK);
    }
    config.max_workers = 4u;
    assert(magic2_cpu_executor_create(&config, &executor) == MAGIC2_OK);
    assert(magic2_cpu_executor_get_info(executor, &info) == MAGIC2_OK);
    assert(info.max_workers == 4u);
    assert(info.domain_id != 0u && info.domain_epoch != 0u);
    test_run_success(executor, MAGIC2_CPU_SCHEDULE_STATIC);
    test_run_success(executor, MAGIC2_CPU_SCHEDULE_DYNAMIC);
    test_run_success(executor, MAGIC2_CPU_SCHEDULE_CALLER);
    test_overlap_and_failure(executor);
    test_busy_and_drain(executor);
    test_family_selection(executor);
    test_async_ticket(executor);
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    assert(executor == NULL);
    puts("magic2 CPU execution checks passed");
    return 0;
}
