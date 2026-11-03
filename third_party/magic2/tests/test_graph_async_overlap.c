/* Graph async metadata and wait regressions. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct async_probe {
    uint32_t submits;
    uint32_t polls;
    uint32_t waits;
    uint32_t cancels;
    uint32_t releases;
    uint32_t pending_waits;
} async_probe;

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

static int dispatch_buffer_run(
    magic2_adaptive_buffer_context *context,
    const magic2_adaptive_buffer_call *call, magic2_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_BUFFER_CONTEXT_RUN;
    request.args.buffer_run.context = context;
    request.args.buffer_run.call = call;
    request.args.buffer_run.status = status;
    return magic2(&request);
}

static int dispatch_buffer_async_submit(
    magic2_adaptive_buffer_context *context,
    const magic2_adaptive_buffer_call *call,
    magic2_async_handle *handle, magic2_async_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_ASYNC_SUBMIT;
    request.args.buffer_async_submit.context = context;
    request.args.buffer_async_submit.call = call;
    request.args.buffer_async_submit.handle = handle;
    request.args.buffer_async_submit.status = status;
    return magic2(&request);
}

static int dispatch_buffer_async_poll(
    magic2_adaptive_buffer_context *context,
    const magic2_async_handle *handle, magic2_async_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_ASYNC_POLL;
    request.args.buffer_async_poll.context = context;
    request.args.buffer_async_poll.handle = handle;
    request.args.buffer_async_poll.status = status;
    return magic2(&request);
}

static int dispatch_buffer_async_wait(
    magic2_adaptive_buffer_context *context,
    const magic2_async_handle *handle, uint64_t timeout_ns,
    magic2_async_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_ASYNC_WAIT;
    request.args.buffer_async_wait.context = context;
    request.args.buffer_async_wait.handle = handle;
    request.args.buffer_async_wait.timeout_ns = timeout_ns;
    request.args.buffer_async_wait.status = status;
    return magic2(&request);
}

static int dispatch_buffer_async_cancel(
    magic2_adaptive_buffer_context *context,
    const magic2_async_handle *handle, magic2_async_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_ASYNC_CANCEL;
    request.args.buffer_async_cancel.context = context;
    request.args.buffer_async_cancel.handle = handle;
    request.args.buffer_async_cancel.status = status;
    return magic2(&request);
}

static int dispatch_buffer_async_release(
    magic2_adaptive_buffer_context *context, magic2_async_handle *handle) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_ASYNC_RELEASE;
    request.args.buffer_async_release.context = context;
    request.args.buffer_async_release.handle = handle;
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

static int dispatch_graph_async_submit(
    magic2_graph *graph, const magic2_graph_binding *bindings,
    size_t binding_count, void *scratch, size_t scratch_bytes,
    magic2_graph_execution_handle *handle, magic2_graph_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ASYNC_SUBMIT;
    request.args.graph_graph_async_submit.graph = graph;
    request.args.graph_graph_async_submit.bindings = bindings;
    request.args.graph_graph_async_submit.binding_count = binding_count;
    request.args.graph_graph_async_submit.scratch = scratch;
    request.args.graph_graph_async_submit.scratch_bytes = scratch_bytes;
    request.args.graph_graph_async_submit.handle = handle;
    request.args.graph_graph_async_submit.status = status;
    return magic2(&request);
}

static int dispatch_graph_async_poll(
    magic2_graph *graph, const magic2_graph_execution_handle *handle,
    magic2_graph_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ASYNC_POLL;
    request.args.graph_graph_async_poll.graph = graph;
    request.args.graph_graph_async_poll.handle = handle;
    request.args.graph_graph_async_poll.status = status;
    return magic2(&request);
}

static int dispatch_graph_async_wait(
    magic2_graph *graph, const magic2_graph_execution_handle *handle,
    uint64_t timeout_ns, magic2_graph_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ASYNC_WAIT;
    request.args.graph_graph_async_wait.graph = graph;
    request.args.graph_graph_async_wait.handle = handle;
    request.args.graph_graph_async_wait.timeout_ns = timeout_ns;
    request.args.graph_graph_async_wait.status = status;
    return magic2(&request);
}

static int dispatch_graph_async_cancel(
    magic2_graph *graph, const magic2_graph_execution_handle *handle,
    magic2_graph_run_status *status) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ASYNC_CANCEL;
    request.args.graph_graph_async_cancel.graph = graph;
    request.args.graph_graph_async_cancel.handle = handle;
    request.args.graph_graph_async_cancel.status = status;
    return magic2(&request);
}

static int dispatch_graph_async_release(
    magic2_graph *graph, magic2_graph_execution_handle *handle) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_ASYNC_RELEASE;
    request.args.graph_graph_async_release.graph = graph;
    request.args.graph_graph_async_release.handle = handle;
    return magic2(&request);
}

static int dispatch_graph_destroy(magic2_graph **graph, uint32_t policy) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    request.operation = MAGIC2_OP_GRAPH_DESTROY;
    request.args.graph_graph_destroy.graph = graph;
    request.args.graph_graph_destroy.policy = policy;
    return magic2(&request);
}

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

static int async_submit_pending(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user, void **operation_user,
    uint32_t *state, int *implementation_status) {
    async_probe *probe = (async_probe *)candidate_user;
    (void)buffers;
    (void)buffer_count;
    (void)count;
    (void)call_user;
    assert(operation_user != NULL && state != NULL && implementation_status != NULL);
    *operation_user = probe;
    *state = MAGIC2_ASYNC_PENDING;
    ++probe->submits;
    return 0;
}

static int async_poll_complete(
    void *operation_user, uint32_t *state, int *implementation_status,
    void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    assert(operation_user != NULL && state != NULL && implementation_status != NULL);
    ++probe->polls;
    *state = MAGIC2_ASYNC_COMPLETE;
    return 0;
}

static int async_wait_complete(
    void *operation_user, uint64_t timeout_ns, uint32_t *state,
    int *implementation_status, void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    (void)timeout_ns;
    assert(operation_user != NULL && state != NULL && implementation_status != NULL);
    ++probe->waits;
    if (probe->pending_waits != 0u) {
        --probe->pending_waits;
        *state = MAGIC2_ASYNC_PENDING;
    } else {
        *state = MAGIC2_ASYNC_COMPLETE;
    }
    return 0;
}

static int async_cancel(
    void *operation_user, void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    assert(operation_user != NULL);
    ++probe->cancels;
    return 0;
}

static void async_release(void *operation_user, void *candidate_user) {
    async_probe *probe = (async_probe *)candidate_user;
    assert(operation_user != NULL);
    ++probe->releases;
}

static magic2_adaptive_buffer_context *make_async_context(async_probe *probe) {
    magic2_adaptive_buffer_candidate candidate =
        MAGIC2_ADAPTIVE_BUFFER_CANDIDATE_INIT;
    magic2_adaptive_buffer_config config = MAGIC2_ADAPTIVE_BUFFER_CONFIG_INIT;
    magic2_adaptive_buffer_context *context = NULL;
    candidate.fn = async_kernel;
    candidate.candidate_user = probe;
    candidate.capabilities = MAGIC2_CAP_FULL_OUTPUT_WRITE |
        MAGIC2_CAP_THREAD_SAFE | MAGIC2_CAP_DETERMINISTIC |
        MAGIC2_CAP_REPEATABLE;
    candidate.async_ops.submit = async_submit_pending;
    candidate.async_ops.poll = async_poll_complete;
    candidate.async_ops.wait = async_wait_complete;
    candidate.async_ops.cancel = async_cancel;
    candidate.async_ops.release = async_release;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.maximum_buffer_count = 1u;
    config.session_count = 1u;
    config.maximum_total_buffer_bytes = 8u;
    config.async_operation_capacity = 1u;
    assert(dispatch_buffer_create(&config, &context) == MAGIC2_OK);
    return context;
}

static magic2_graph *make_async_graph(
    magic2_adaptive_buffer_context *context, magic2_graph_info *info) {
    magic2_graph_config config = MAGIC2_GRAPH_CONFIG_INIT;
    magic2_graph_value_desc value = MAGIC2_GRAPH_VALUE_DESC_INIT;
    magic2_graph_port port = MAGIC2_GRAPH_PORT_INIT;
    magic2_graph_node_desc node = MAGIC2_GRAPH_NODE_DESC_INIT;
    magic2_graph *graph = NULL;
    uint32_t value_index;
    uint32_t node_index;
    config.maximum_nodes = 1u;
    config.maximum_values = 1u;
    config.maximum_ports = 1u;
    config.async_execution_capacity = 1u;
    assert(dispatch_graph_create(&config, &graph) == MAGIC2_OK);
    value.flags = MAGIC2_GRAPH_VALUE_ZERO;
    value.bytes = 8u;
    assert(dispatch_graph_add_value(graph, &value, &value_index) == MAGIC2_OK);
    port.value_index = value_index;
    port.access = MAGIC2_BUFFER_WRITE;
    node.kind = MAGIC2_GRAPH_NODE_BUFFERS;
    node.count = 8u;
    node.ports = &port;
    node.port_count = 1u;
    node.buffer_context = context;
    assert(dispatch_graph_add_node(graph, &node, &node_index) == MAGIC2_OK);
    assert(dispatch_graph_compile(graph, info) == MAGIC2_OK);
    assert(info->scratch_bytes == 8u);
    return graph;
}

typedef union aligned_storage {
    uintptr_t alignment;
    unsigned char bytes[128];
} aligned_storage;

static void initialize_status_at(
    aligned_storage *storage, magic2_graph_run_status **status) {
    *status = (magic2_graph_run_status *)(void *)storage->bytes;
    **status = MAGIC2_GRAPH_RUN_STATUS_INIT;
}

static void buffer_submit_pending(
    magic2_adaptive_buffer_context *context,
    magic2_adaptive_buffer_call *call,
    magic2_async_handle *handle, magic2_async_status *status) {
    *handle = MAGIC2_ASYNC_HANDLE_INIT;
    *status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_submit(context, call, handle, status) == MAGIC2_OK);
    assert(status->state == MAGIC2_ASYNC_PENDING);
}

static void test_direct_buffer_metadata_overlap(void) {
    async_probe probe = { 0u, 0u, 0u, 0u, 0u, 0u };
    magic2_adaptive_buffer_context *context = make_async_context(&probe);
    magic2_adaptive_buffer_call call = MAGIC2_ADAPTIVE_BUFFER_CALL_INIT;
    magic2_buffer_desc buffer = MAGIC2_BUFFER_INIT;
    magic2_async_handle handle = MAGIC2_ASYNC_HANDLE_INIT;
    magic2_async_status status = MAGIC2_ASYNC_STATUS_INIT;
    magic2_run_status sync_status = MAGIC2_RUN_STATUS_INIT;
    aligned_storage storage;
    aligned_storage output_storage;
    magic2_async_handle *overlap_handle;
    magic2_async_status *overlap_status;

    buffer.data = storage.bytes;
    buffer.bytes = 8u;
    buffer.alignment = 1u;
    buffer.stride = 1u;
    buffer.access = MAGIC2_BUFFER_WRITE;
    call.buffers = &buffer;
    call.buffer_count = 1u;
    call.count = 8u;

    *(magic2_run_status *)(void *)storage.bytes = sync_status;
    assert(dispatch_buffer_run(
        context, &call, (magic2_run_status *)(void *)storage.bytes) ==
        MAGIC2_EOVERLAP);

    overlap_handle = (magic2_async_handle *)(void *)storage.bytes;
    *overlap_handle = MAGIC2_ASYNC_HANDLE_INIT;
    status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_submit(
        context, &call, overlap_handle, &status) == MAGIC2_EOVERLAP);

    overlap_status = (magic2_async_status *)(void *)storage.bytes;
    *overlap_status = MAGIC2_ASYNC_STATUS_INIT;
    handle = MAGIC2_ASYNC_HANDLE_INIT;
    assert(dispatch_buffer_async_submit(
        context, &call, &handle, overlap_status) == MAGIC2_EOVERLAP);

    buffer.data = output_storage.bytes;
    buffer_submit_pending(context, &call, &handle, &status);
    overlap_status = (magic2_async_status *)(void *)output_storage.bytes;
    *overlap_status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_poll(context, &handle, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_wait(
        context, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    assert(dispatch_buffer_async_release(context, &handle) == MAGIC2_OK);

    buffer_submit_pending(context, &call, &handle, &status);
    overlap_status = (magic2_async_status *)(void *)output_storage.bytes;
    *overlap_status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_wait(
        context, &handle, 0u, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_wait(
        context, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    assert(dispatch_buffer_async_release(context, &handle) == MAGIC2_OK);

    buffer_submit_pending(context, &call, &handle, &status);
    overlap_status = (magic2_async_status *)(void *)output_storage.bytes;
    *overlap_status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_cancel(context, &handle, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_ASYNC_STATUS_INIT;
    assert(dispatch_buffer_async_cancel(context, &handle, &status) == MAGIC2_OK);
    assert(dispatch_buffer_async_release(context, &handle) == MAGIC2_OK);

    buffer_submit_pending(context, &call, &handle, &status);
    assert(dispatch_buffer_async_wait(
        context, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    memcpy(output_storage.bytes, &handle, sizeof(handle));
    overlap_handle = (magic2_async_handle *)(void *)output_storage.bytes;
    assert(dispatch_buffer_async_release(context, overlap_handle) == MAGIC2_EOVERLAP);
    assert(dispatch_buffer_async_release(context, &handle) == MAGIC2_OK);

    assert(probe.submits == 4u);
    assert(probe.releases == 4u);
    assert(dispatch_buffer_destroy(&context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
    assert(context == NULL);
}

static void test_submit_metadata_overlap(
    magic2_graph *graph, uint32_t scratch_bytes) {
    aligned_storage storage;
    magic2_graph_execution_handle handle = MAGIC2_GRAPH_EXECUTION_HANDLE_INIT;
    magic2_graph_run_status *status;
    magic2_graph_execution_handle *overlap_handle;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    initialize_status_at(&storage, &status);
    assert(dispatch_graph_async_submit(
        graph, NULL, 0u, storage.bytes, scratch_bytes, &handle, status) ==
        MAGIC2_EOVERLAP);
    assert(handle.execution_id == 0u && handle.slot_index == UINT32_MAX);
    assert(dispatch_graph_query(graph, &info) == MAGIC2_OK);
    assert(info.async_execution_occupancy == 0u);

    handle = MAGIC2_GRAPH_EXECUTION_HANDLE_INIT;
    status = (magic2_graph_run_status *)(void *)(&storage.bytes[64]);
    *status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    overlap_handle = (magic2_graph_execution_handle *)(void *)storage.bytes;
    *overlap_handle = MAGIC2_GRAPH_EXECUTION_HANDLE_INIT;
    assert(dispatch_graph_async_submit(
        graph, NULL, 0u, storage.bytes, scratch_bytes, overlap_handle, status) ==
        MAGIC2_EOVERLAP);
    assert(dispatch_graph_query(graph, &info) == MAGIC2_OK);
    assert(info.async_execution_occupancy == 0u);
}

static void test_sync_metadata_overlap(
    magic2_graph *graph, uint32_t scratch_bytes) {
    aligned_storage storage;
    magic2_graph_run_status *status;
    initialize_status_at(&storage, &status);
    assert(dispatch_graph_run(
        graph, NULL, 0u, storage.bytes, 0u, status) == MAGIC2_EOVERLAP);
    assert(status->state == MAGIC2_GRAPH_IDLE);
    initialize_status_at(&storage, &status);
    assert(dispatch_graph_run(
        graph, NULL, 0u, storage.bytes, scratch_bytes, status) ==
        MAGIC2_EOVERLAP);
    assert(status->state == MAGIC2_GRAPH_IDLE);
}

static void submit_pending(
    magic2_graph *graph, unsigned char *scratch, uint32_t scratch_bytes,
    magic2_graph_execution_handle *handle, magic2_graph_run_status *status) {
    *handle = MAGIC2_GRAPH_EXECUTION_HANDLE_INIT;
    *status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    assert(dispatch_graph_async_submit(
        graph, NULL, 0u, scratch, scratch_bytes, handle, status) == MAGIC2_OK);
    assert(status->state == MAGIC2_GRAPH_PENDING);
    assert(handle->execution_id != 0u);
}

static void test_status_overlap_on_progress(
    magic2_graph *graph, uint32_t scratch_bytes) {
    aligned_storage scratch_storage;
    unsigned char *scratch = scratch_storage.bytes;
    magic2_graph_execution_handle handle;
    magic2_graph_run_status status;
    magic2_graph_run_status *overlap_status;

    submit_pending(graph, scratch, scratch_bytes, &handle, &status);
    initialize_status_at(&scratch_storage, &overlap_status);
    memcpy(overlap_status, &status, sizeof(status));
    assert(dispatch_graph_async_poll(graph, &handle, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    assert(dispatch_graph_async_wait(
        graph, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    assert(status.state == MAGIC2_GRAPH_COMPLETE);
    assert(dispatch_graph_async_release(graph, &handle) == MAGIC2_OK);

    submit_pending(graph, scratch, scratch_bytes, &handle, &status);
    initialize_status_at(&scratch_storage, &overlap_status);
    memcpy(overlap_status, &status, sizeof(status));
    assert(dispatch_graph_async_wait(
        graph, &handle, UINT64_MAX, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    assert(dispatch_graph_async_wait(
        graph, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    assert(dispatch_graph_async_release(graph, &handle) == MAGIC2_OK);

    submit_pending(graph, scratch, scratch_bytes, &handle, &status);
    initialize_status_at(&scratch_storage, &overlap_status);
    memcpy(overlap_status, &status, sizeof(status));
    assert(dispatch_graph_async_cancel(graph, &handle, overlap_status) == MAGIC2_EOVERLAP);
    status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    assert(dispatch_graph_async_cancel(graph, &handle, &status) == MAGIC2_OK);
    assert(dispatch_graph_async_release(graph, &handle) == MAGIC2_OK);
}

static void test_release_handle_overlap(
    magic2_graph *graph, uint32_t scratch_bytes) {
    aligned_storage scratch_storage;
    unsigned char *scratch = scratch_storage.bytes;
    magic2_graph_execution_handle handle;
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;
    magic2_graph_execution_handle *overlap_handle;

    submit_pending(graph, scratch, scratch_bytes, &handle, &status);
    assert(dispatch_graph_async_wait(
        graph, &handle, UINT64_MAX, &status) == MAGIC2_OK);
    memcpy(scratch_storage.bytes, &handle, sizeof(handle));
    overlap_handle = (magic2_graph_execution_handle *)(void *)scratch_storage.bytes;
    assert(dispatch_graph_async_release(graph, overlap_handle) == MAGIC2_EOVERLAP);
    assert(dispatch_graph_async_release(graph, &handle) == MAGIC2_OK);
}

int main(void) {
    async_probe probe = { 0u, 0u, 0u, 0u, 0u, 0u };
    magic2_adaptive_buffer_context *context = make_async_context(&probe);
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    magic2_graph *graph = make_async_graph(context, &info);
    test_submit_metadata_overlap(graph, (uint32_t)info.scratch_bytes);
    test_sync_metadata_overlap(graph, (uint32_t)info.scratch_bytes);
    test_status_overlap_on_progress(graph, (uint32_t)info.scratch_bytes);
    test_release_handle_overlap(graph, (uint32_t)info.scratch_bytes);
    {
        aligned_storage scratch_storage;
        magic2_graph_execution_handle handle;
        magic2_graph_run_status status;
        const uint32_t waits_before = probe.waits;
        probe.pending_waits = 3u;
        submit_pending(graph, scratch_storage.bytes, (uint32_t)info.scratch_bytes,
                       &handle, &status);
        status = MAGIC2_GRAPH_RUN_STATUS_INIT;
        assert(dispatch_graph_async_wait(
            graph, &handle, 0u, &status) == MAGIC2_OK);
        assert(status.state == MAGIC2_GRAPH_PENDING);
        assert(probe.waits == waits_before + 1u);
        probe.pending_waits = 0u;
        status = MAGIC2_GRAPH_RUN_STATUS_INIT;
        assert(dispatch_graph_async_wait(
            graph, &handle, 0u, &status) == MAGIC2_OK);
        assert(status.state == MAGIC2_GRAPH_COMPLETE);
        assert(dispatch_graph_async_release(graph, &handle) == MAGIC2_OK);
    }
    assert(probe.submits == 5u);
    assert(probe.waits >= 5u);
    assert(probe.releases == 5u);
    assert(dispatch_graph_destroy(&graph, MAGIC2_DESTROY_TRY) == MAGIC2_OK);
    assert(graph == NULL);
    assert(dispatch_buffer_destroy(&context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
    assert(context == NULL);
    test_direct_buffer_metadata_overlap();
    puts("magic2 graph async overlap regressions passed");
    return 0;
}
