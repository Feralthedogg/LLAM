/* Differential dependency and failure testing over deterministic random DAGs. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct random_node_state {
    uint32_t delta;
    int fail;
} random_node_state;

static int random_node_kernel(
    const void *opaque, void *const *ports, size_t count, void *scratch) {
    const random_node_state *state = (const random_node_state *)opaque;
    const uint32_t *input = (const uint32_t *)ports[0];
    uint32_t *output = (uint32_t *)ports[1];
    size_t index;
    (void)scratch;
    if (state->fail != 0) return state->fail;
    for (index = 0u; index < count; ++index)
        output[index] = input[index] + state->delta;
    return 0;
}

static magic2_sealed_plan *make_random_plan(random_node_state *state) {
    magic2_port ports[2];
    magic2_buffer_contract contracts[2];
    magic2_sealed_plan_spec spec = MAGIC2_SEALED_PLAN_SPEC_INIT;
    magic2_sealed_plan *plan = NULL;
    memset(ports, 0, sizeof(ports));
    ports[0].bytes = 64u;
    ports[0].stride = sizeof(uint32_t);
    ports[0].alignment = sizeof(uint32_t);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_READ;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_PRESERVE;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_WRITE;
    memset(contracts, 0, sizeof(contracts));
    contracts[0].bytes = ports[0].bytes;
    contracts[0].alignment = ports[0].alignment;
    contracts[0].stride = ports[0].stride;
    contracts[0].access = ports[0].access;
    contracts[0].alias_group = 0u;
    contracts[1].bytes = ports[1].bytes;
    contracts[1].alignment = ports[1].alignment;
    contracts[1].stride = ports[1].stride;
    contracts[1].access = ports[1].access;
    contracts[1].alias_group = 1u;
    spec.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.shape.count = 16u;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.product.tag = MAGIC2_TAG_SEALED;
    spec.product.struct_size = (uint32_t)sizeof(spec.product);
    spec.product.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.product.failure_contract = MAGIC2_FAILURE_ATOMIC;
    spec.product.fn.many = random_node_kernel;
    spec.product.environment = state;
    spec.product.environment_bytes = sizeof(*state);
    spec.buffer_contracts = contracts;
    spec.buffer_contract_count = 2u;
    assert(magic2_sealed_plan_create(&spec, &plan) == MAGIC2_OK);
    return plan;
}

static uint32_t random_next(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int status_bit(const uint64_t *bits, uint32_t index) {
    return (bits[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static void run_case(magic2_cpu_executor *executor, uint32_t seed) {
    random_node_state states[MAGIC2_GRAPH_MAX_NODES];
    magic2_sealed_plan *plans[MAGIC2_GRAPH_MAX_NODES];
    uint32_t parents[MAGIC2_GRAPH_MAX_NODES];
    magic2_region bindings[MAGIC2_GRAPH_MAX_VALUES];
    unsigned char reference[MAGIC2_GRAPH_MAX_VALUES][64];
    magic2_sealed_graph_config config;
    magic2_sealed_graph_builder *builder = NULL;
    magic2_sealed_graph *graph = NULL;
    magic2_sealed_graph_info info;
    magic2_sealed_graph_region region;
    magic2_sealed_graph_node node;
    magic2_sealed_graph_frame frame;
    magic2_sealed_graph_status sequential;
    magic2_sealed_graph_parallel_status parallel =
        MAGIC2_SEALED_GRAPH_PARALLEL_STATUS_INIT;
    uint32_t random_state = seed;
    const uint32_t node_count = 8u + random_next(&random_state) % 57u;
    const uint32_t worker_count = node_count < 8u ? node_count : 8u;
    const uint32_t failed_node = node_count / 2u;
    void *workspace_allocation;
    void *workspace;
    uintptr_t raw;
    uint32_t index;
    memset(states, 0, sizeof(states));
    memset(plans, 0, sizeof(plans));
    memset(bindings, 0, sizeof(bindings));
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.tag = MAGIC2_TAG_SEALED;
    config.maximum_nodes = node_count;
    config.maximum_regions = node_count + 1u;
    config.maximum_ports = node_count * 2u;
    config.maximum_region_maps = node_count * 2u;
    config.flags = MAGIC2_SEALED_GRAPH_PARALLEL_SAFE;
    config.parallel_workers = worker_count;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    memset(&region, 0, sizeof(region));
    region.bytes = 64u;
    region.alignment = sizeof(uint32_t);
    region.flags = MAGIC2_SEALED_GRAPH_REGION_EXTERNAL;
    for (index = 0u; index <= node_count; ++index) {
        uint32_t region_index;
        assert(magic2_sealed_graph_add_region(
            builder, &region, &region_index) == MAGIC2_OK);
        bindings[index].base = calloc(1u, 64u);
        bindings[index].capacity = 64u;
        assert(bindings[index].base != NULL);
    }
    for (index = 0u; index < 16u; ++index)
        ((uint32_t *)bindings[0].base)[index] = seed + index;
    memset(&node, 0, sizeof(node));
    for (index = 0u; index < node_count; ++index) {
        uint32_t map[2];
        uint32_t node_index;
        parents[index] = index == 0u || random_next(&random_state) % 4u == 0u ?
            UINT32_MAX : random_next(&random_state) % index;
        states[index].delta = random_next(&random_state) | 1u;
        plans[index] = make_random_plan(&states[index]);
        map[0] = parents[index] == UINT32_MAX ? 0u : parents[index] + 1u;
        map[1] = index + 1u;
        node.plan = plans[index];
        node.region_map = map;
        node.region_map_count = 2u;
        assert(magic2_sealed_graph_add_node(
            builder, &node, &node_index) == MAGIC2_OK);
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_compile(builder, &graph, &info) == MAGIC2_OK);
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
    workspace_allocation = malloc(info.workspace_bytes + info.workspace_alignment - 1u);
    assert(workspace_allocation != NULL);
    raw = (uintptr_t)workspace_allocation;
    workspace = (void *)((raw + info.workspace_alignment - 1u) &
        ~(uintptr_t)(info.workspace_alignment - 1u));
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = (uint32_t)sizeof(frame);
    frame.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_frame_init(
        &frame, workspace, info.workspace_bytes) == MAGIC2_OK);
    assert(magic2_sealed_graph_frame_bind(
        graph, bindings, node_count + 1u, &frame) == MAGIC2_OK);
    memset(&sequential, 0, sizeof(sequential));
    sequential.struct_size = (uint32_t)sizeof(sequential);
    sequential.tag = MAGIC2_TAG_SEALED;
    assert(magic2_sealed_graph_run_bound(&frame, &sequential) == MAGIC2_OK);
    for (index = 0u; index <= node_count; ++index)
        memcpy(reference[index], bindings[index].base, 64u);
    for (index = 1u; index <= node_count; ++index)
        memset(bindings[index].base, 0, 64u);
    assert(magic2_sealed_graph_run_parallel_bound(
        &frame, executor, worker_count, &parallel) == MAGIC2_OK);
    assert(parallel.completed_nodes == node_count);
    for (index = 0u; index <= node_count; ++index)
        assert(memcmp(reference[index], bindings[index].base, 64u) == 0);

    states[failed_node].fail = -73;
    for (index = 1u; index <= node_count; ++index)
        memset(bindings[index].base, 0, 64u);
    parallel = magic2_sealed_graph_parallel_status_initializer();
    assert(magic2_sealed_graph_run_parallel_bound(
        &frame, executor, worker_count, &parallel) == MAGIC2_EIMPLEMENTATION);
    assert(status_bit(parallel.failed, failed_node));
    assert(parallel.node_status[failed_node] == -73);
    for (index = 0u; index < node_count; ++index) {
        uint32_t ancestor = parents[index];
        while (ancestor != UINT32_MAX && ancestor != failed_node)
            ancestor = parents[ancestor];
        if (ancestor == failed_node) assert(status_bit(parallel.cancelled, index));
    }
    assert(parallel.completed_nodes + parallel.failed_nodes +
        parallel.cancelled_nodes == node_count);
    assert(magic2_sealed_graph_frame_unbind(&frame) == MAGIC2_OK);
    magic2_sealed_graph_release(&graph);
    for (index = 0u; index < node_count; ++index)
        magic2_sealed_plan_release(&plans[index]);
    for (index = 0u; index <= node_count; ++index)
        free(bindings[index].base);
    free(workspace_allocation);
}

int main(void) {
    magic2_cpu_executor_config config = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor *executor = NULL;
    uint32_t seed;
    config.max_workers = 8u;
    assert(magic2_cpu_executor_create(&config, &executor) == MAGIC2_OK);
    for (seed = 1u; seed <= 40u; ++seed) run_case(executor, seed);
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    puts("magic2 random parallel graph differential passed: 40 cases");
    return 0;
}
