/* Verify dependency-ready parallel graph execution and drained failure sets. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct graph_kernel_state {
    volatile uint32_t *barrier;
    int fail_status;
    volatile uint32_t calls;
    uint32_t worker_claim;
} graph_kernel_state;

static int graph_copy(
    const void *opaque, void *const *ports, size_t count, void *scratch) {
    graph_kernel_state *state = (graph_kernel_state *)opaque;
    assert(count == 1u);
    assert(scratch != NULL && ((uintptr_t)scratch & 15u) == 0u);
    memset(scratch, 0x3c, 16u);
    (void)__atomic_fetch_add(&state->calls, 1u, __ATOMIC_ACQ_REL);
    if (state->barrier != NULL) {
        (void)__atomic_fetch_add(state->barrier, 1u, __ATOMIC_ACQ_REL);
        while (__atomic_load_n(state->barrier, __ATOMIC_ACQUIRE) < 2u)
            sched_yield();
    }
    if (state->fail_status != 0) return state->fail_status;
    memcpy(ports[1], ports[0], 8u);
    return 0;
}

static magic2_sealed_plan *make_graph_plan(graph_kernel_state *state) {
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
    contracts[0].alignment = 8u;
    contracts[0].stride = 8u;
    contracts[0].access = MAGIC2_BUFFER_READ;
    contracts[0].alias_group = 0u;
    contracts[1].bytes = 8u;
    contracts[1].alignment = 8u;
    contracts[1].stride = 8u;
    contracts[1].access = MAGIC2_BUFFER_WRITE;
    contracts[1].alias_group = 1u;
    spec.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.shape.count = 1u;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.product.tag = MAGIC2_TAG_SEALED;
    spec.product.struct_size = (uint32_t)sizeof(spec.product);
    spec.product.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.product.failure_contract = MAGIC2_FAILURE_ATOMIC;
    spec.product.fn.many = graph_copy;
    spec.product.environment = state;
    spec.product.environment_bytes = sizeof(*state);
    spec.product.scratch.bytes = 16u;
    spec.product.scratch.alignment = 16u;
    spec.product.worker_claim = state->worker_claim != 0u ?
        state->worker_claim : 1u;
    spec.buffer_contracts = contracts;
    spec.buffer_contract_count = 2u;
    assert(magic2_sealed_plan_create(&spec, &plan) == MAGIC2_OK);
    return plan;
}

typedef struct aligned_memory {
    void *allocation;
    void *pointer;
} aligned_memory;

static aligned_memory allocate_aligned(size_t bytes, size_t alignment) {
    aligned_memory value;
    uintptr_t raw;
    value.allocation = malloc(bytes + alignment - 1u);
    assert(value.allocation != NULL);
    raw = (uintptr_t)value.allocation;
    value.pointer = (void *)((raw + alignment - 1u) &
        ~(uintptr_t)(alignment - 1u));
    return value;
}

int main(void) {
    volatile uint32_t barrier = 0u;
    graph_kernel_state states[4];
    magic2_sealed_plan *plans[4];
    magic2_sealed_graph_config config;
    magic2_sealed_graph_builder *builder = NULL;
    magic2_sealed_graph *graph = NULL;
    magic2_sealed_graph_info info;
    magic2_sealed_graph_region region;
    magic2_sealed_graph_node node;
    magic2_sealed_graph_frame frame;
    magic2_sealed_graph_parallel_status parallel_status =
        MAGIC2_SEALED_GRAPH_PARALLEL_STATUS_INIT;
    magic2_sealed_graph_status sequential_status;
    magic2_cpu_executor_config executor_config = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor *executor = NULL;
    magic2_region external[3];
    aligned_memory workspace;
    uint32_t maps[4][2] = {
        { 0u, 1u }, { 0u, 2u }, { 1u, 3u }, { 2u, 4u }
    };
    unsigned char source[8] = { 1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u };
    unsigned char output_a[8];
    unsigned char output_b[8];
    uint32_t index;
    memset(states, 0, sizeof(states));
    states[0].barrier = &barrier;
    states[1].barrier = &barrier;
    for (index = 0u; index < 4u; ++index) plans[index] = make_graph_plan(&states[index]);
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.tag = MAGIC2_TAG_SEALED;
    config.maximum_nodes = 4u;
    config.maximum_regions = 5u;
    config.maximum_ports = 8u;
    config.maximum_region_maps = 8u;
    config.flags = MAGIC2_SEALED_GRAPH_PARALLEL_SAFE;
    config.parallel_workers = 2u;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    memset(&region, 0, sizeof(region));
    region.bytes = 8u;
    region.alignment = 8u;
    for (index = 0u; index < 5u; ++index) {
        uint32_t region_index = UINT32_MAX;
        region.flags = index == 0u || index >= 3u ?
            MAGIC2_SEALED_GRAPH_REGION_EXTERNAL : 0u;
        assert(magic2_sealed_graph_add_region(
            builder, &region, &region_index) == MAGIC2_OK);
        assert(region_index == index);
    }
    memset(&node, 0, sizeof(node));
    for (index = 0u; index < 4u; ++index) {
        uint32_t node_index = UINT32_MAX;
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
    assert(info.flags == MAGIC2_SEALED_GRAPH_PARALLEL_SAFE);
    assert(info.parallel_workers == 2u);
    assert(info.graph_scratch_bytes >= 16u);
    assert(info.kernel_scratch_stride >= 16u);
    assert(info.workspace_bytes > info.graph_scratch_bytes);
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
    workspace = allocate_aligned(info.workspace_bytes, info.workspace_alignment);
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = (uint32_t)sizeof(frame);
    frame.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_frame_init(
        &frame, workspace.pointer, info.workspace_bytes) == MAGIC2_OK);
    external[0].base = source;
    external[0].capacity = sizeof(source);
    external[1].base = output_a;
    external[1].capacity = sizeof(output_a);
    external[2].base = output_b;
    external[2].capacity = sizeof(output_b);
    memset(output_a, 0u, sizeof(output_a));
    memset(output_b, 0u, sizeof(output_b));
    assert(magic2_sealed_graph_frame_bind(
        graph, external, 3u, &frame) == MAGIC2_OK);
    executor_config.max_workers = 2u;
    assert(magic2_cpu_executor_create(
        &executor_config, &executor) == MAGIC2_OK);
    assert(magic2_sealed_graph_run_parallel_bound(
        &frame, executor, 2u, &parallel_status) == MAGIC2_OK);
    assert(parallel_status.state == MAGIC2_SEALED_GRAPH_COMPLETE);
    assert(parallel_status.completed_nodes == 4u);
    assert(parallel_status.failed_nodes == 0u);
    assert(parallel_status.cancelled_nodes == 0u);
    assert((parallel_status.completed[0] & UINT64_C(0xf)) == UINT64_C(0xf));
    assert(memcmp(source, output_a, sizeof(source)) == 0);
    assert(memcmp(source, output_b, sizeof(source)) == 0);
    assert(__atomic_load_n(&barrier, __ATOMIC_ACQUIRE) == 2u);

    states[0].barrier = NULL;
    states[1].barrier = NULL;
    states[0].fail_status = -44;
    parallel_status = magic2_sealed_graph_parallel_status_initializer();
    assert(magic2_sealed_graph_run_parallel_bound(
        &frame, executor, 2u, &parallel_status) == MAGIC2_EIMPLEMENTATION);
    assert(parallel_status.state == MAGIC2_SEALED_GRAPH_FAILED);
    assert(parallel_status.first_failed_node == 0u);
    assert(parallel_status.kernel_status == -44);
    assert(parallel_status.node_status[0] == -44);
    assert((parallel_status.failed[0] & UINT64_C(1)) != 0u);
    assert((parallel_status.cancelled[0] & (UINT64_C(1) << 2u)) != 0u);
    assert(parallel_status.completed_nodes + parallel_status.failed_nodes +
        parallel_status.cancelled_nodes == 4u);

    states[0].fail_status = 0;
    memset(&sequential_status, 0, sizeof(sequential_status));
    sequential_status.struct_size = (uint32_t)sizeof(sequential_status);
    sequential_status.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_run_bound(
        &frame, &sequential_status) == MAGIC2_OK);
    assert(sequential_status.completed_nodes == 4u);
    assert(magic2_sealed_graph_frame_unbind(&frame) == MAGIC2_OK);
    magic2_sealed_graph_release(&graph);
    free(workspace.allocation);

    /* Two internal lifetimes separated by a dependency may share storage. */
    {
        uint32_t chain_maps[4][2] = {
            { 0u, 1u }, { 1u, 2u }, { 2u, 3u }, { 3u, 4u }
        };
        memset(&config, 0, sizeof(config));
        config.struct_size = (uint32_t)sizeof(config);
        config.tag = MAGIC2_TAG_SEALED;
        config.maximum_nodes = 4u;
        config.maximum_regions = 5u;
        config.maximum_ports = 8u;
        config.maximum_region_maps = 8u;
        config.flags = MAGIC2_SEALED_GRAPH_PARALLEL_SAFE;
        config.parallel_workers = 2u;
        assert(magic2_sealed_graph_builder_create(
            &config, &builder) == MAGIC2_OK);
        memset(&region, 0, sizeof(region));
        region.bytes = 8u;
        region.alignment = 8u;
        for (index = 0u; index < 5u; ++index) {
            uint32_t region_index;
            region.flags = index == 0u || index == 2u || index == 4u ?
                MAGIC2_SEALED_GRAPH_REGION_EXTERNAL : 0u;
            assert(magic2_sealed_graph_add_region(
                builder, &region, &region_index) == MAGIC2_OK);
        }
        memset(&node, 0, sizeof(node));
        for (index = 0u; index < 4u; ++index) {
            uint32_t node_index;
            node.plan = plans[index];
            node.region_map = chain_maps[index];
            node.region_map_count = 2u;
            assert(magic2_sealed_graph_add_node(
                builder, &node, &node_index) == MAGIC2_OK);
        }
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.tag = MAGIC2_TAG_SEALED;
        assert(magic2_sealed_graph_compile(
            builder, &graph, &info) == MAGIC2_OK);
        assert(info.graph_scratch_bytes == 8u);
        assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
        magic2_sealed_graph_release(&graph);
    }
    {
        graph_kernel_state wide_state;
        magic2_sealed_plan *wide_plan;
        uint32_t wide_map[2] = { 0u, 1u };
        uint32_t region_index;
        uint32_t node_index;
        memset(&wide_state, 0, sizeof(wide_state));
        wide_state.worker_claim = 3u;
        wide_plan = make_graph_plan(&wide_state);
        memset(&config, 0, sizeof(config));
        config.struct_size = (uint32_t)sizeof(config);
        config.tag = MAGIC2_TAG_SEALED;
        config.maximum_nodes = 1u;
        config.maximum_regions = 2u;
        config.maximum_ports = 2u;
        config.maximum_region_maps = 2u;
        config.flags = MAGIC2_SEALED_GRAPH_PARALLEL_SAFE;
        config.parallel_workers = 2u;
        assert(magic2_sealed_graph_builder_create(
            &config, &builder) == MAGIC2_OK);
        memset(&region, 0, sizeof(region));
        region.bytes = 8u;
        region.alignment = 8u;
        region.flags = MAGIC2_SEALED_GRAPH_REGION_EXTERNAL;
        assert(magic2_sealed_graph_add_region(
            builder, &region, &region_index) == MAGIC2_OK);
        assert(magic2_sealed_graph_add_region(
            builder, &region, &region_index) == MAGIC2_OK);
        memset(&node, 0, sizeof(node));
        node.plan = wide_plan;
        node.region_map = wide_map;
        node.region_map_count = 2u;
        assert(magic2_sealed_graph_add_node(
            builder, &node, &node_index) == MAGIC2_OK);
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.tag = MAGIC2_TAG_SEALED;
        assert(magic2_sealed_graph_compile(
            builder, &graph, &info) == MAGIC2_ECAPACITY);
        assert(graph == NULL);
        assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
        magic2_sealed_plan_release(&wide_plan);
    }
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    for (index = 0u; index < 4u; ++index)
        magic2_sealed_plan_release(&plans[index]);
    puts("magic2 parallel sealed graph passed");
    return 0;
}
