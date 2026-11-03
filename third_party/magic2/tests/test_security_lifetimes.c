/* Regressions for allocator-owner ordering and failed async submission cleanup. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocator_state {
    uint32_t refs;
    uint32_t frees;
} allocator_state;

static void *tracked_alloc(size_t bytes, void *opaque) {
    allocator_state *state = (allocator_state *)opaque;
    assert(state != NULL && state->refs != 0u);
    return malloc(bytes);
}

static void tracked_free(void *memory, void *opaque) {
    allocator_state *state = (allocator_state *)opaque;
    /* Reading refs is intentional: stale allocator_user use trips ASan. */
    assert(state != NULL && state->refs != 0u);
    ++state->frees;
    free(memory);
}

static int tracked_retain(void *opaque) {
    allocator_state *state = (allocator_state *)opaque;
    assert(state != NULL && state->refs != 0u);
    assert(state->refs != UINT32_MAX);
    ++state->refs;
    return 0;
}

static void tracked_release(void *opaque) {
    allocator_state *state = (allocator_state *)opaque;
    assert(state != NULL && state->refs != 0u);
    --state->refs;
    if (state->refs == 0u) free(state);
}

static allocator_state *allocator_state_create(void) {
    allocator_state *state = (allocator_state *)calloc(1u, sizeof(*state));
    assert(state != NULL);
    state->refs = 1u;
    return state;
}

static magic2_sealed_graph_config graph_config(allocator_state *state) {
    magic2_sealed_graph_config config;
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.tag = MAGIC2_TAG_SEALED;
    config.maximum_nodes = 1u;
    config.maximum_regions = 2u;
    config.maximum_ports = 2u;
    config.maximum_region_maps = 2u;
    config.alloc = tracked_alloc;
    config.dealloc = tracked_free;
    config.allocator_user = state;
    config.allocator_user_bytes = sizeof(*state);
    config.allocator_owner.token = state;
    config.allocator_owner.retain = tracked_retain;
    config.allocator_owner.release = tracked_release;
    return config;
}

static int graph_noop(
    const void *environment, void *const *ports, size_t count, void *scratch) {
    (void)environment;
    (void)ports;
    (void)count;
    (void)scratch;
    return 0;
}

static magic2_sealed_plan *make_dependency_plan(void) {
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
    ports[1].initialization = MAGIC2_ZERO;
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
    spec.shape.count = 1u;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.product.tag = MAGIC2_TAG_SEALED;
    spec.product.struct_size = (uint32_t)sizeof(spec.product);
    spec.product.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.product.failure_contract = MAGIC2_FAILURE_ATOMIC;
    spec.product.fn.many = graph_noop;
    spec.product.scratch.bytes = 0u;
    spec.product.scratch.alignment = 1u;
    spec.product.worker_claim = 1u;
    spec.buffer_contracts = contracts;
    spec.buffer_contract_count = 2u;
    assert(magic2_sealed_plan_create(&spec, &plan) == MAGIC2_OK);
    return plan;
}

static void test_builder_owner_order(void) {
    allocator_state *state = allocator_state_create();
    magic2_sealed_graph_config config = graph_config(state);
    magic2_sealed_graph_builder *builder = NULL;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    tracked_release(state); /* The builder is now the final owner. */
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
}

static void test_graph_owner_order(void) {
    allocator_state *state = allocator_state_create();
    magic2_sealed_graph_config config = graph_config(state);
    magic2_sealed_graph_builder *builder = NULL;
    magic2_sealed_graph *graph = NULL;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    tracked_release(state); /* The builder is now the final owner. */
    assert(magic2_sealed_graph_compile(builder, &graph, NULL) == MAGIC2_OK);
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
    magic2_sealed_graph_release(&graph);
}

static void test_compile_failure_owner_order(void) {
    allocator_state *state = allocator_state_create();
    magic2_sealed_graph_config config = graph_config(state);
    magic2_sealed_graph_builder *builder = NULL;
    magic2_sealed_graph *graph = NULL;
    magic2_sealed_plan *plan = make_dependency_plan();
    magic2_sealed_graph_region region;
    magic2_sealed_graph_node node;
    uint32_t region_map[2] = { 0u, 1u };
    uint32_t index;
    memset(&region, 0, sizeof(region));
    region.bytes = 8u;
    region.alignment = 8u;
    assert(magic2_sealed_graph_builder_create(&config, &builder) == MAGIC2_OK);
    tracked_release(state); /* The builder is now the final owner. */
    assert(magic2_sealed_graph_add_region(builder, &region, &index) == MAGIC2_OK);
    assert(magic2_sealed_graph_add_region(builder, &region, &index) == MAGIC2_OK);
    memset(&node, 0, sizeof(node));
    node.plan = plan;
    node.region_map = region_map;
    node.region_map_count = 2u;
    assert(magic2_sealed_graph_add_node(builder, &node, &index) == MAGIC2_OK);
    assert(magic2_sealed_graph_compile(builder, &graph, NULL) == MAGIC2_EDEPENDENCY);
    assert(graph == NULL);
    assert(magic2_sealed_graph_builder_destroy(&builder) == MAGIC2_OK);
    magic2_sealed_plan_release(&plan);
}

typedef struct async_probe {
    uint32_t submits;
    uint32_t cancels;
    uint32_t waits;
    uint32_t releases;
} async_probe;

static int async_kernel(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user) {
    (void)buffers;
    (void)buffer_count;
    (void)count;
    (void)call_user;
    (void)candidate_user;
    return 0;
}

static int async_submit_without_handle(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user, void **operation_user,
    uint32_t *state, int *implementation_status) {
    async_probe *probe = (async_probe *)candidate_user;
    (void)buffers;
    (void)buffer_count;
    (void)count;
    (void)call_user;
    assert(operation_user != NULL && *operation_user == NULL);
    assert(state != NULL && implementation_status != NULL);
    ++probe->submits;
    *state = MAGIC2_ASYNC_PENDING;
    return 77;
}

static int async_submit_with_handle(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user, void **operation_user,
    uint32_t *state, int *implementation_status) {
    async_probe *probe = (async_probe *)candidate_user;
    (void)buffers;
    (void)buffer_count;
    (void)count;
    (void)call_user;
    assert(operation_user != NULL && state != NULL &&
        implementation_status != NULL);
    ++probe->submits;
    *operation_user = candidate_user;
    *state = MAGIC2_ASYNC_PENDING;
    return 77;
}

static int async_poll_requires_handle(
    void *operation_user, uint32_t *state, int *implementation_status,
    void *candidate_user) {
    (void)state;
    (void)implementation_status;
    (void)candidate_user;
    assert(operation_user != NULL);
    return 0;
}

static int async_wait_requires_handle(
    void *operation_user, uint64_t timeout_ns, uint32_t *state,
    int *implementation_status, void *candidate_user) {
    (void)timeout_ns;
    async_probe *probe = (async_probe *)candidate_user;
    (void)implementation_status;
    assert(operation_user != NULL);
    assert(state != NULL);
    ++probe->waits;
    *state = MAGIC2_ASYNC_CANCELLED;
    return 0;
}

static int async_cancel_requires_handle(
    void *operation_user, void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    assert(operation_user != NULL);
    ++probe->cancels;
    return 0;
}

static void async_release_requires_handle(
    void *operation_user, void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    assert(operation_user != NULL);
    ++probe->releases;
}

static void test_failed_submit_case(
    magic2_async_submit_fn submit, uint32_t expected_cancels,
    uint32_t expected_waits, uint32_t expected_releases) {
    async_probe probe = { 0u, 0u, 0u, 0u };
    magic2_adaptive_buffer_candidate candidate =
        MAGIC2_ADAPTIVE_BUFFER_CANDIDATE_INIT;
    magic2_adaptive_buffer_config config = MAGIC2_ADAPTIVE_BUFFER_CONFIG_INIT;
    magic2_adaptive_buffer_context *context = NULL;
    magic2_adaptive_buffer_call call = MAGIC2_ADAPTIVE_BUFFER_CALL_INIT;
    magic2_async_handle handle = MAGIC2_ASYNC_HANDLE_INIT;
    magic2_async_status status = MAGIC2_ASYNC_STATUS_INIT;
    magic2_buffer_desc buffer = MAGIC2_BUFFER_INIT;
    unsigned char output[16];

    candidate.fn = async_kernel;
    candidate.candidate_user = &probe;
    candidate.capabilities = MAGIC2_CAP_FULL_OUTPUT_WRITE |
        MAGIC2_CAP_THREAD_SAFE | MAGIC2_CAP_DETERMINISTIC |
        MAGIC2_CAP_REPEATABLE;
    candidate.async_ops = magic2_async_candidate_ops_initializer();
    candidate.async_ops.submit = submit;
    candidate.async_ops.poll = async_poll_requires_handle;
    candidate.async_ops.wait = async_wait_requires_handle;
    candidate.async_ops.cancel = async_cancel_requires_handle;
    candidate.async_ops.release = async_release_requires_handle;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.maximum_buffer_count = 1u;
    config.session_count = 1u;
    config.maximum_total_buffer_bytes = sizeof(output);
    config.async_operation_capacity = 1u;
    assert(magic2_buffer_context_create(&config, &context) == MAGIC2_OK);
    buffer.data = output;
    buffer.bytes = sizeof(output);
    buffer.alignment = 1u;
    buffer.stride = 1u;
    buffer.access = MAGIC2_BUFFER_WRITE;
    call.buffers = &buffer;
    call.buffer_count = 1u;
    call.count = sizeof(output);
    assert(magic2_buffer_async_submit(
        context, &call, &handle, &status) == MAGIC2_EIMPLEMENTATION);
    assert(probe.submits == 1u);
    assert(probe.cancels == expected_cancels);
    assert(probe.waits == expected_waits);
    assert(probe.releases == expected_releases);
    assert(magic2_buffer_context_destroy(
        &context, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
    assert(context == NULL);
}

static void test_failed_submit_without_handle(void) {
    test_failed_submit_case(async_submit_without_handle, 0u, 0u, 0u);
}

static void test_failed_submit_with_handle(void) {
    test_failed_submit_case(async_submit_with_handle, 1u, 1u, 1u);
}

int main(void) {
    test_builder_owner_order();
    test_graph_owner_order();
    test_compile_failure_owner_order();
    test_failed_submit_without_handle();
    test_failed_submit_with_handle();
    puts("magic2 security lifetime regressions passed");
    return 0;
}
