/* Verify permit-aware ready-node selection overlaps heterogeneous claims. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct fairness_shared {
    volatile uint32_t a_started;
    volatile uint64_t a_finished_ns;
    volatile uint64_t c_started_ns;
} fairness_shared;

typedef struct fairness_environment {
    fairness_shared *shared;
    uint32_t role;
} fairness_environment;

enum {
    FAIRNESS_A = 0u,
    FAIRNESS_PREDECESSOR = 1u,
    FAIRNESS_B = 2u,
    FAIRNESS_C = 3u
};

static uint64_t fairness_now_ns(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64() * UINT64_C(1000000);
#elif defined(CLOCK_MONOTONIC)
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#elif defined(TIME_UTC)
    struct timespec value;
    assert(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#else
    const clock_t value = clock();
    assert(value != (clock_t)-1);
    return (uint64_t)value * UINT64_C(1000000000) /
        (uint64_t)CLOCKS_PER_SEC;
#endif
}

static void fairness_sleep_ms(uint32_t milliseconds) {
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec request;
    request.tv_sec = (time_t)(milliseconds / 1000u);
    request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&request, &request) != 0 && errno == EINTR) { }
#endif
}

static void fairness_started_store(fairness_shared *shared, uint32_t value) {
#if defined(_WIN32)
    (void)InterlockedExchange(
        (volatile LONG *)&shared->a_started, (LONG)value);
#else
    __atomic_store_n(&shared->a_started, value, __ATOMIC_RELEASE);
#endif
}

static uint32_t fairness_started_load(const fairness_shared *shared) {
#if defined(_WIN32)
    return (uint32_t)InterlockedCompareExchange(
        (volatile LONG *)&shared->a_started, 0, 0);
#else
    return __atomic_load_n(&shared->a_started, __ATOMIC_ACQUIRE);
#endif
}

static int fairness_kernel(
    const void *opaque, void *const *ports, size_t count, void *scratch) {
    const fairness_environment *environment =
        (const fairness_environment *)opaque;
    fairness_shared *shared = environment->shared;
    (void)scratch;
    assert(count == 1u);
    assert(ports != NULL && ports[1] != NULL);
    if (environment->role == FAIRNESS_A) {
        fairness_started_store(shared, 1u);
        fairness_sleep_ms(20u);
        shared->a_finished_ns = fairness_now_ns();
    } else if (environment->role == FAIRNESS_PREDECESSOR) {
        const uint64_t deadline = fairness_now_ns() + UINT64_C(1000000000);
        while (fairness_started_load(shared) == 0u &&
               fairness_now_ns() < deadline)
            fairness_sleep_ms(1u);
        assert(fairness_started_load(shared) != 0u);
    } else if (environment->role == FAIRNESS_B) {
        fairness_sleep_ms(1u);
    } else {
        shared->c_started_ns = fairness_now_ns();
        fairness_sleep_ms(15u);
    }
    memset(ports[1], (int)environment->role, 8u);
    return 0;
}

static magic2_sealed_plan *make_fairness_plan(
    fairness_environment *environment, uint32_t worker_claim,
    uint64_t stable_id) {
    magic2_port ports[2];
    magic2_buffer_contract contracts[2];
    magic2_sealed_plan_spec spec = MAGIC2_SEALED_PLAN_SPEC_INIT;
    magic2_sealed_plan *plan = NULL;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = 8u;
    ports[0].stride = 8u;
    ports[0].alignment = 8u;
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_READ;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_PRESERVE;
    ports[1].bytes = 8u;
    ports[1].stride = 8u;
    ports[1].alignment = 8u;
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_WRITE;
    ports[1].present = 1u;
    ports[1].initialization = MAGIC2_PRESERVE;
    memset(contracts, 0, sizeof(contracts));
    contracts[0].bytes = 8u;
    contracts[0].stride = 8u;
    contracts[0].alignment = 8u;
    contracts[0].access = MAGIC2_BUFFER_READ;
    contracts[0].alias_group = 0u;
    contracts[1].bytes = 8u;
    contracts[1].stride = 8u;
    contracts[1].alignment = 8u;
    contracts[1].access = MAGIC2_BUFFER_WRITE;
    contracts[1].alias_group = 1u;
    spec.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.semantic_key.low = stable_id;
    spec.shape.operation_id.low = UINT64_C(0x666169726e657373);
    spec.shape.semantic_id.low = UINT64_C(0x7065726d697473);
    spec.shape.count = 1u;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.product.tag = MAGIC2_TAG_SEALED;
    spec.product.struct_size = (uint32_t)sizeof(spec.product);
    spec.product.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.product.failure_contract = MAGIC2_FAILURE_ATOMIC;
    spec.product.fn.many = fairness_kernel;
    spec.product.environment = environment;
    spec.product.environment_bytes = sizeof(*environment);
    spec.product.scratch.bytes = 0u;
    spec.product.scratch.alignment = 1u;
    spec.product.worker_claim = worker_claim;
    spec.buffer_contracts = contracts;
    spec.buffer_contract_count = 2u;
    assert(magic2_sealed_plan_create(&spec, &plan) == MAGIC2_OK);
    return plan;
}

static void assign_region(magic2_region *region, void *base) {
    region->base = base;
    region->capacity = 8u;
}

int main(void) {
    fairness_shared shared = { 0u, 0u, 0u };
    fairness_environment environments[4];
    magic2_sealed_plan *plans[4];
    magic2_sealed_graph_config config;
    magic2_sealed_graph_builder *builder = NULL;
    magic2_sealed_graph *graph = NULL;
    magic2_sealed_graph_info info;
    magic2_sealed_graph_region region;
    magic2_sealed_graph_node node;
    magic2_sealed_graph_frame frame;
    magic2_sealed_graph_parallel_status status =
        MAGIC2_SEALED_GRAPH_PARALLEL_STATUS_INIT;
    magic2_cpu_executor_config executor_config =
        MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor *executor = NULL;
    magic2_region external[5];
    uint32_t maps[4][2] = {
        { 0u, 1u }, { 2u, 3u }, { 3u, 4u }, { 3u, 5u }
    };
    unsigned char a_input[8] = { 0u };
    unsigned char a_output[8] = { 0u };
    unsigned char predecessor_input[8] = { 0u };
    unsigned char b_output[8] = { 0u };
    unsigned char c_output[8] = { 0u };
    void *workspace_allocation;
    void *workspace;
    uintptr_t raw;
    uint32_t index;

    for (index = 0u; index < 4u; ++index) {
        environments[index].shared = &shared;
        environments[index].role = index;
        plans[index] = make_fairness_plan(
            &environments[index], index == FAIRNESS_B ? 2u : 1u,
            (uint64_t)index + UINT64_C(1));
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.tag = MAGIC2_TAG_SEALED;
    config.maximum_nodes = 4u;
    config.maximum_regions = 6u;
    config.maximum_ports = 8u;
    config.maximum_region_maps = 8u;
    config.flags = MAGIC2_SEALED_GRAPH_PARALLEL_SAFE;
    config.parallel_workers = 2u;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    memset(&region, 0, sizeof(region));
    region.bytes = 8u;
    region.alignment = 8u;
    for (index = 0u; index < 6u; ++index) {
        uint32_t region_index;
        region.flags = index == 0u || index == 1u || index == 2u ||
            index == 4u || index == 5u ?
            MAGIC2_SEALED_GRAPH_REGION_EXTERNAL : 0u;
        assert(magic2_sealed_graph_add_region(
            builder, &region, &region_index) == MAGIC2_OK);
        assert(region_index == index);
    }
    memset(&node, 0, sizeof(node));
    for (index = 0u; index < 4u; ++index) {
        uint32_t node_index;
        node.plan = plans[index];
        node.region_map = maps[index];
        node.region_map_count = 2u;
        assert(magic2_sealed_graph_add_node(
            builder, &node, &node_index) == MAGIC2_OK);
        assert(node_index == index);
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_compile(builder, &graph, &info) == MAGIC2_OK);
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
    workspace_allocation = malloc(
        info.workspace_bytes + info.workspace_alignment - 1u);
    assert(workspace_allocation != NULL);
    raw = (uintptr_t)workspace_allocation;
    workspace = (void *)((raw + info.workspace_alignment - 1u) &
        ~(uintptr_t)(info.workspace_alignment - 1u));
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = (uint32_t)sizeof(frame);
    frame.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_frame_init(
        &frame, workspace, info.workspace_bytes) == MAGIC2_OK);
    assign_region(&external[0], a_input);
    assign_region(&external[1], a_output);
    assign_region(&external[2], predecessor_input);
    assign_region(&external[3], b_output);
    assign_region(&external[4], c_output);
    assert(magic2_sealed_graph_frame_bind(
        graph, external, 5u, &frame) == MAGIC2_OK);
    executor_config.max_workers = 2u;
    assert(magic2_cpu_executor_create(
        &executor_config, &executor) == MAGIC2_OK);
    assert(magic2_sealed_graph_run_parallel_bound(
        &frame, executor, 2u, &status) == MAGIC2_OK);
    assert(status.state == MAGIC2_SEALED_GRAPH_COMPLETE);
    assert(status.completed_nodes == 4u);
    assert(shared.a_started != 0u);
    assert(shared.a_finished_ns != 0u);
    assert(shared.c_started_ns != 0u);
    assert(shared.c_started_ns < shared.a_finished_ns);
    assert(magic2_sealed_graph_frame_unbind(&frame) == MAGIC2_OK);
    magic2_sealed_graph_release(&graph);
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    for (index = 0u; index < 4u; ++index)
        magic2_sealed_plan_release(&plans[index]);
    free(workspace_allocation);
    puts("magic2 parallel fairness regression passed");
    return 0;
}
