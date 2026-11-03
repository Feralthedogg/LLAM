/* Ordinary graph ownership, dependency, and snapshot regressions. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int dispatch_context_create(
    const magic2_adaptive_config *config, magic2_adaptive_context **context) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_CONTEXT_CREATE;
    request.args.adaptive_create.config = config;
    request.args.adaptive_create.out_context = context;
    return magic2(&request);
}

static int dispatch_context_destroy(
    magic2_adaptive_context **context, uint32_t policy) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_CONTEXT_DESTROY;
    request.args.adaptive_destroy.context = context;
    request.args.adaptive_destroy.policy = policy;
    return magic2(&request);
}

static int dispatch_buffer_create(
    const magic2_adaptive_buffer_config *config,
    magic2_adaptive_buffer_context **context) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_BUFFER_CONTEXT_CREATE;
    request.args.buffer_create.config = config;
    request.args.buffer_create.out_context = context;
    return magic2(&request);
}

static int dispatch_buffer_destroy(
    magic2_adaptive_buffer_context **context, uint32_t policy) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_BUFFER_CONTEXT_DESTROY;
    request.args.buffer_destroy.context = context;
    request.args.buffer_destroy.policy = policy;
    return magic2(&request);
}

static int dispatch_graph_create(
    const magic2_graph_config *config, magic2_graph **graph) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_CREATE;
    request.args.graph_graph_create.config = config;
    request.args.graph_graph_create.out_graph = graph;
    return magic2(&request);
}

static int dispatch_graph_add_value(
    magic2_graph *graph, const magic2_graph_value_desc *value,
    uint32_t *value_index) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ADD_REGION;
    request.args.graph_graph_add_value.graph = graph;
    request.args.graph_graph_add_value.value = value;
    request.args.graph_graph_add_value.out_value_index = value_index;
    return magic2(&request);
}

static int dispatch_graph_add_node(
    magic2_graph *graph, const magic2_graph_node_desc *node,
    uint32_t *node_index) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ADD_NODE;
    request.args.graph_graph_add_node.graph = graph;
    request.args.graph_graph_add_node.node = node;
    request.args.graph_graph_add_node.out_node_index = node_index;
    return magic2(&request);
}

static int dispatch_graph_compile(magic2_graph *graph, magic2_graph_info *info) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_COMPILE;
    request.args.graph_graph_compile.graph = graph;
    request.args.graph_graph_compile.info = info;
    return magic2(&request);
}

static int dispatch_graph_run(
    magic2_graph *graph, const magic2_graph_binding *bindings,
    size_t binding_count, void *scratch, size_t scratch_bytes,
    magic2_graph_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_RUN;
    request.args.graph_graph_run.graph = graph;
    request.args.graph_graph_run.bindings = bindings;
    request.args.graph_graph_run.binding_count = binding_count;
    request.args.graph_graph_run.scratch = scratch;
    request.args.graph_graph_run.scratch_bytes = scratch_bytes;
    request.args.graph_graph_run.status = status;
    return magic2(&request);
}

static int dispatch_graph_query(magic2_graph *graph, magic2_graph_info *info) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_QUERY;
    request.args.graph_graph_query.graph = graph;
    request.args.graph_graph_query.info = info;
    return magic2(&request);
}

static int dispatch_graph_destroy(magic2_graph **graph, uint32_t policy) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_DESTROY;
    request.args.graph_graph_destroy.graph = graph;
    request.args.graph_graph_destroy.policy = policy;
    return magic2(&request);
}

static int adaptive_increment(
    void *output, const void *input, size_t count, void *call_user,
    void *candidate_user) {
    unsigned char *out = (unsigned char *)output;
    const unsigned char *in = (const unsigned char *)input;
    (void)count;
    (void)call_user;
    (void)candidate_user;
    out[0] = (unsigned char)(in[0] + 1u);
    return 0;
}

static int buffer_fill(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user) {
    (void)count;
    (void)call_user;
    (void)candidate_user;
    assert(buffer_count == 1u);
    ((unsigned char *)buffers[0].data)[0] = 0x5au;
    return 0;
}

static magic2_adaptive_context *make_adaptive_context(void) {
    magic2_candidate candidate = MAGIC2_ADAPTIVE_CANDIDATE_INIT;
    magic2_adaptive_config config = MAGIC2_ADAPTIVE_CONFIG_INIT;
    magic2_adaptive_context *context = NULL;
    candidate.fn = adaptive_increment;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.output_bytes = 8u;
    config.maximum_input_bytes = 8u;
    assert(dispatch_context_create(&config, &context) == MAGIC2_OK);
    return context;
}

static magic2_adaptive_buffer_context *make_buffer_context(void) {
    magic2_adaptive_buffer_candidate candidate =
        MAGIC2_ADAPTIVE_BUFFER_CANDIDATE_INIT;
    magic2_adaptive_buffer_config config = MAGIC2_ADAPTIVE_BUFFER_CONFIG_INIT;
    magic2_adaptive_buffer_context *context = NULL;
    candidate.fn = buffer_fill;
    candidate.capabilities = MAGIC2_CAP_FULL_OUTPUT_WRITE |
        MAGIC2_CAP_THREAD_SAFE | MAGIC2_CAP_DETERMINISTIC |
        MAGIC2_CAP_REPEATABLE;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.maximum_buffer_count = 1u;
    config.session_count = 1u;
    config.maximum_total_buffer_bytes = 8u;
    config.async_operation_capacity = 1u;
    assert(dispatch_buffer_create(&config, &context) == MAGIC2_OK);
    return context;
}

static magic2_graph *make_graph(
    uint32_t maximum_nodes, uint32_t maximum_values, uint32_t maximum_ports) {
    magic2_graph_config config = MAGIC2_GRAPH_CONFIG_INIT;
    magic2_graph *graph = NULL;
    config.maximum_nodes = maximum_nodes;
    config.maximum_values = maximum_values;
    config.maximum_ports = maximum_ports;
    config.async_execution_capacity = 1u;
    assert(dispatch_graph_create(&config, &graph) == MAGIC2_OK);
    return graph;
}

static magic2_graph_value_desc value_desc(uint32_t flags) {
    magic2_graph_value_desc value = MAGIC2_GRAPH_VALUE_DESC_INIT;
    value.flags = flags;
    value.bytes = 8u;
    return value;
}

static magic2_graph_port port_desc(uint32_t value_index, uint32_t access) {
    magic2_graph_port port = MAGIC2_GRAPH_PORT_INIT;
    port.value_index = value_index;
    port.access = access;
    return port;
}

static magic2_graph_node_desc callback_node(
    const magic2_graph_port *ports, uint32_t port_count) {
    magic2_graph_node_desc node = MAGIC2_GRAPH_NODE_DESC_INIT;
    node.kind = MAGIC2_GRAPH_NODE_CALLBACK;
    node.count = 8u;
    node.ports = ports;
    node.port_count = port_count;
    return node;
}

static int graph_noop(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *node_user) {
    (void)buffers;
    (void)buffer_count;
    (void)count;
    (void)call_user;
    (void)node_user;
    return 0;
}

static int graph_write_42(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *node_user) {
    (void)count;
    (void)call_user;
    (void)node_user;
    assert(buffer_count == 1u);
    ((unsigned char *)buffers[0].data)[0] = 42u;
    return 0;
}

static int graph_write_99(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *node_user) {
    (void)count;
    (void)call_user;
    (void)node_user;
    assert(buffer_count == 2u);
    ((unsigned char *)buffers[1].data)[0] = 99u;
    return 0;
}

static int graph_copy_first(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *node_user) {
    (void)count;
    (void)call_user;
    (void)node_user;
    assert(buffer_count == 2u);
    ((unsigned char *)buffers[1].data)[0] =
        ((const unsigned char *)buffers[0].data)[0];
    return 0;
}

static void test_context_lifetime_retention(void) {
    magic2_adaptive_context *adaptive = make_adaptive_context();
    magic2_adaptive_buffer_context *buffer_context = make_buffer_context();
    magic2_graph *graph = make_graph(2u, 3u, 3u);
    magic2_graph_value_desc input_value = value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_value_desc output_value = value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_value_desc filled_value = value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_port adaptive_ports[2];
    magic2_graph_port buffer_ports[1];
    magic2_graph_node_desc adaptive_node = MAGIC2_GRAPH_NODE_DESC_INIT;
    magic2_graph_node_desc buffer_node = MAGIC2_GRAPH_NODE_DESC_INIT;
    uint32_t input_index;
    uint32_t output_index;
    uint32_t filled_index;
    uint32_t node_index;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    magic2_graph_binding bindings[3];
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    unsigned char input[8] = { 41u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    unsigned char output[8] = { 0u };
    unsigned char filled[8] = { 0u };

    assert(dispatch_graph_add_value(graph, &input_value, &input_index) == MAGIC2_OK);
    assert(dispatch_graph_add_value(graph, &output_value, &output_index) == MAGIC2_OK);
    assert(dispatch_graph_add_value(graph, &filled_value, &filled_index) == MAGIC2_OK);
    adaptive_ports[0] = port_desc(input_index, MAGIC2_BUFFER_READ);
    adaptive_ports[1] = port_desc(output_index, MAGIC2_BUFFER_WRITE);
    adaptive_node.kind = MAGIC2_GRAPH_NODE_ADAPTIVE;
    adaptive_node.count = 8u;
    adaptive_node.ports = adaptive_ports;
    adaptive_node.port_count = 2u;
    adaptive_node.adaptive_context = adaptive;
    assert(dispatch_graph_add_node(graph, &adaptive_node, &node_index) == MAGIC2_OK);

    /* Graph ownership keeps the node context alive. */
    assert(dispatch_context_destroy(&adaptive, MAGIC2_DESTROY_DRAIN) == MAGIC2_EBUSY);
    assert(adaptive != NULL);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);
    bindings[0] = MAGIC2_GRAPH_BINDING_INIT;
    bindings[0].value_index = input_index;
    bindings[0].data = input;
    bindings[0].bytes = sizeof(input);
    bindings[1] = MAGIC2_GRAPH_BINDING_INIT;
    bindings[1].value_index = output_index;
    bindings[1].data = output;
    bindings[1].bytes = sizeof(output);
    assert(dispatch_graph_run(graph, bindings, 2u, NULL, 0u, &status) == MAGIC2_OK);
    assert(output[0] == 42u);

    /* Buffer nodes retain their context as well. */
    buffer_ports[0] = port_desc(filled_index, MAGIC2_BUFFER_WRITE);
    buffer_node.kind = MAGIC2_GRAPH_NODE_BUFFERS;
    buffer_node.count = 8u;
    buffer_node.ports = buffer_ports;
    buffer_node.port_count = 1u;
    buffer_node.buffer_context = buffer_context;
    assert(dispatch_graph_add_node(graph, &buffer_node, &node_index) == MAGIC2_OK);
    assert(dispatch_buffer_destroy(
        &buffer_context, MAGIC2_DESTROY_DRAIN) == MAGIC2_EBUSY);
    assert(buffer_context != NULL);
    /* Recompile after mutation. */
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);
    bindings[2] = MAGIC2_GRAPH_BINDING_INIT;
    bindings[2].value_index = filled_index;
    bindings[2].data = filled;
    bindings[2].bytes = sizeof(filled);
    assert(dispatch_graph_run(graph, bindings, 3u, NULL, 0u, &status) == MAGIC2_OK);
    assert(filled[0] == 0x5au);

    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
    assert(graph == NULL);
    assert(dispatch_buffer_destroy(
        &buffer_context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
    assert(buffer_context == NULL);
    assert(dispatch_context_destroy(&adaptive, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
    assert(adaptive == NULL);
}

static void test_self_read_dependency(void) {
    magic2_graph *graph = make_graph(1u, 1u, 2u);
    magic2_graph_value_desc value = value_desc(0u);
    magic2_graph_port ports[2];
    magic2_graph_node_desc node;
    uint32_t value_index;
    uint32_t node_index;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;

    assert(dispatch_graph_add_value(graph, &value, &value_index) == MAGIC2_OK);
    ports[0] = port_desc(value_index, MAGIC2_BUFFER_READ);
    ports[1] = port_desc(value_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(ports, 2u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_EDEPENDENCY);
    assert(info.compiled == 0u);
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);

    /* Caller-initialized values may be read and written in place. */
    graph = make_graph(1u, 1u, 2u);
    value = value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    assert(dispatch_graph_add_value(graph, &value, &value_index) == MAGIC2_OK);
    ports[0] = port_desc(value_index, MAGIC2_BUFFER_READ);
    ports[1] = port_desc(value_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(ports, 2u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);
    {
        unsigned char data[8] = { 0u };
        magic2_graph_binding binding = MAGIC2_GRAPH_BINDING_INIT;
        magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
        binding.value_index = value_index;
        binding.data = data;
        binding.bytes = sizeof(data);
        assert(dispatch_graph_run(graph, &binding, 1u, NULL, 0u, &status) == MAGIC2_OK);
        assert(status.completed_nodes == 1u);
    }
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
}

static void test_compiled_info_snapshot(void) {
    magic2_graph *graph = make_graph(3u, 2u, 3u);
    magic2_graph_value_desc value = value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_port port;
    magic2_graph_node_desc node;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    magic2_graph_binding binding = MAGIC2_GRAPH_BINDING_INIT;
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    unsigned char data[8] = { 0u };
    uint32_t first_value;
    uint32_t second_value;
    uint32_t node_index;

    assert(dispatch_graph_add_value(graph, &value, &first_value) == MAGIC2_OK);
    port = port_desc(first_value, MAGIC2_BUFFER_WRITE);
    node = callback_node(&port, 1u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);
    assert(info.compiled == 1u);
    assert(info.node_count == 1u && info.value_count == 1u && info.port_count == 1u);

    assert(dispatch_graph_add_value(graph, &value, &second_value) == MAGIC2_OK);
    port = port_desc(second_value, MAGIC2_BUFFER_WRITE);
    node = callback_node(&port, 1u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_query(graph, &info) == MAGIC2_OK);
    assert(info.compiled == 1u);
    assert(info.node_count == 1u && info.value_count == 1u && info.port_count == 1u);

    /* A failed compile preserves the last valid snapshot. */
    port = port_desc(first_value, MAGIC2_BUFFER_WRITE);
    node = callback_node(&port, 1u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_EDEPENDENCY);
    assert(info.compiled == 1u);
    assert(info.node_count == 1u && info.value_count == 1u && info.port_count == 1u);

    binding.value_index = first_value;
    binding.data = data;
    binding.bytes = sizeof(data);
    assert(dispatch_graph_run(graph, &binding, 1u, NULL, 0u, &status) == MAGIC2_OK);
    assert(status.completed_nodes == 1u);
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
}

static void test_external_scratch_alias(void) {
    magic2_graph *graph = make_graph(3u, 3u, 5u);
    magic2_graph_value_desc internal_value = value_desc(0u);
    magic2_graph_value_desc external_value =
        value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_port node0_port;
    magic2_graph_port node1_ports[2];
    magic2_graph_port node2_ports[2];
    magic2_graph_node_desc node;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    magic2_graph_binding bindings[2];
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    unsigned char scratch[16] = { 0u };
    unsigned char separate_output[8] = { 0u };
    unsigned char final_output[8] = { 0u };
    uint32_t internal_index;
    uint32_t output_index;
    uint32_t final_index;
    uint32_t node_index;

    assert(dispatch_graph_add_value(
        graph, &internal_value, &internal_index) == MAGIC2_OK);
    assert(dispatch_graph_add_value(
        graph, &external_value, &output_index) == MAGIC2_OK);
    assert(dispatch_graph_add_value(
        graph, &external_value, &final_index) == MAGIC2_OK);

    node0_port = port_desc(internal_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(&node0_port, 1u);
    node.callback = graph_write_42;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);

    node1_ports[0] = port_desc(internal_index, MAGIC2_BUFFER_READ);
    node1_ports[1] = port_desc(output_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(node1_ports, 2u);
    node.callback = graph_write_99;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);

    node2_ports[0] = port_desc(internal_index, MAGIC2_BUFFER_READ);
    node2_ports[1] = port_desc(final_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(node2_ports, 2u);
    node.callback = graph_copy_first;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);

    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);
    assert(info.scratch_bytes == 8u);
    bindings[0] = MAGIC2_GRAPH_BINDING_INIT;
    bindings[0].value_index = output_index;
    bindings[0].data = separate_output;
    bindings[0].bytes = sizeof(separate_output);
    bindings[1] = MAGIC2_GRAPH_BINDING_INIT;
    bindings[1].value_index = final_index;
    bindings[1].data = final_output;
    bindings[1].bytes = sizeof(final_output);
    assert(dispatch_graph_run(
        graph, bindings, 2u, scratch, sizeof(scratch), &status) == MAGIC2_OK);
    assert(separate_output[0] == 99u && final_output[0] == 42u);

    /* External outputs must stay disjoint from graph scratch. */
    memset(scratch, 0, sizeof(scratch));
    bindings[0].data = scratch;
    memset(final_output, 0, sizeof(final_output));
    assert(dispatch_graph_run(
        graph, bindings, 2u, scratch, sizeof(scratch), &status) ==
        MAGIC2_EOVERLAP);
    assert(final_output[0] == 0u);
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
}

static void test_status_external_binding_error(void) {
    magic2_graph *graph = make_graph(1u, 2u, 2u);
    magic2_graph_value_desc value =
        value_desc(MAGIC2_GRAPH_VALUE_EXTERNAL);
    magic2_graph_port ports[2];
    magic2_graph_node_desc node;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    magic2_graph_binding binding = MAGIC2_GRAPH_BINDING_INIT;
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    union {
        uintptr_t alignment;
        unsigned char bytes[128];
    } storage;
    uint32_t input_index;
    uint32_t output_index;
    uint32_t node_index;

    assert(dispatch_graph_add_value(graph, &value, &input_index) == MAGIC2_OK);
    assert(dispatch_graph_add_value(graph, &value, &output_index) == MAGIC2_OK);
    ports[0] = port_desc(input_index, MAGIC2_BUFFER_READ);
    ports[1] = port_desc(output_index, MAGIC2_BUFFER_WRITE);
    node = callback_node(ports, 2u);
    node.callback = graph_noop;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, &info) == MAGIC2_OK);

    *(magic2_graph_run_status *)(void *)storage.bytes = status;
    binding.value_index = input_index;
    binding.data = storage.bytes;
    binding.bytes = 8u;
    assert(dispatch_graph_run(
        graph, &binding, 1u, NULL, 0u,
        (magic2_graph_run_status *)(void *)storage.bytes) ==
        MAGIC2_EOVERLAP);
    assert(((magic2_graph_run_status *)(void *)storage.bytes)->state ==
        MAGIC2_GRAPH_IDLE);
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
}

int main(void) {
    test_context_lifetime_retention();
    test_self_read_dependency();
    test_compiled_info_snapshot();
    test_external_scratch_alias();
    test_status_external_binding_error();
    puts("magic2 ordinary graph regressions passed");
    return 0;
}
