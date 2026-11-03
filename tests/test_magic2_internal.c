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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "runtime_magic2.h"

int test_magic2_cpu_internal(void);
int test_magic2_auto_internal(void);

typedef struct magic2_test_state {
    llam_runtime_t *runtime;
    magic2_graph *graph;
    llam_cancel_token_t *token;
    atomic_uint failures;
    atomic_uint calls;
    atomic_uint started;
    atomic_uint release;
    atomic_uint finished;
    atomic_uint returned;
    bool cancel_before;
    bool cancel_running;
    int kernel_result;
    int output;
    int bridge_result;
    int bridge_errno;
    int dispatch_status;
    int implementation_status;
} magic2_test_state_t;

static void fail(magic2_test_state_t *state, const char *message) {
    fprintf(stderr, "test_magic2_internal: %s\n", message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static int dispatch(magic2_dispatch_request *request) {
    int status = MAGIC2_ENOTPROCESSED;

    if (llam_magic2_dispatch(request, &status) != 0) {
        fprintf(stderr, "test_magic2_internal: dispatch submission failed: %d\n", errno);
        return MAGIC2_ENOTPROCESSED;
    }
    return status;
}

static int graph_kernel(magic2_buffer_desc *buffers, size_t buffer_count,
                        size_t count, void *call_user, void *node_user) {
    magic2_test_state_t *state = node_user;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);

    (void)call_user;
    atomic_fetch_add_explicit(&state->calls, 1U, memory_order_relaxed);
    if (g_llam_tls_task != NULL || g_llam_tls_worker != NULL ||
        g_llam_tls_processor != NULL) {
        fail(state, "native callback executed on a managed scheduler worker");
    }
    if (buffer_count != 1U || count != 1U || buffers[0].bytes != sizeof(int)) {
        fail(state, "graph callback received an incorrect binding");
        return 90;
    }
    atomic_store_explicit(&state->started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
#if LLAM_PLATFORM_WINDOWS
        Sleep(1U);
#else
        struct timespec delay = {0, 1000000L};
        (void)nanosleep(&delay, NULL);
#endif
    }
    if (atomic_load_explicit(&state->release, memory_order_acquire) == 0U) {
        fail(state, "one-P controller could not run while the kernel was blocked");
    }
    *(int *)buffers[0].data = 123;
    atomic_store_explicit(&state->finished, 1U, memory_order_release);
    return state->kernel_result;
}

static int create_graph(magic2_test_state_t *state) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    magic2_graph_config config = MAGIC2_GRAPH_CONFIG_INIT;
    magic2_graph_value_desc value = MAGIC2_GRAPH_VALUE_DESC_INIT;
    magic2_graph_port port = MAGIC2_GRAPH_PORT_INIT;
    magic2_graph_node_desc node = MAGIC2_GRAPH_NODE_DESC_INIT;
    magic2_graph_info info = MAGIC2_GRAPH_INFO_INIT;
    uint32_t index = UINT32_MAX;

    config.maximum_nodes = 1U;
    config.maximum_values = 1U;
    config.maximum_ports = 1U;
    config.async_execution_capacity = 1U;
    request.operation = MAGIC2_OP_GRAPH_CREATE;
    request.args.graph_graph_create.config = &config;
    request.args.graph_graph_create.out_graph = &state->graph;
    if (dispatch(&request) != MAGIC2_OK) {
        return -1;
    }
    value.flags = MAGIC2_GRAPH_VALUE_EXTERNAL;
    value.bytes = sizeof(int);
    value.alignment = _Alignof(int);
    request = magic2_dispatch_request_initializer();
    request.operation = MAGIC2_OP_GRAPH_ADD_REGION;
    request.args.graph_graph_add_value.graph = state->graph;
    request.args.graph_graph_add_value.value = &value;
    request.args.graph_graph_add_value.out_value_index = &index;
    if (dispatch(&request) != MAGIC2_OK || index != 0U) {
        return -1;
    }
    port.value_index = index;
    port.access = MAGIC2_BUFFER_WRITE;
    port.alignment = _Alignof(int);
    node.kind = MAGIC2_GRAPH_NODE_CALLBACK;
    node.count = 1U;
    node.ports = &port;
    node.port_count = 1U;
    node.callback = graph_kernel;
    node.node_user = state;
    request = magic2_dispatch_request_initializer();
    request.operation = MAGIC2_OP_GRAPH_ADD_NODE;
    request.args.graph_graph_add_node.graph = state->graph;
    request.args.graph_graph_add_node.node = &node;
    request.args.graph_graph_add_node.out_node_index = &index;
    if (dispatch(&request) != MAGIC2_OK || index != 0U) {
        return -1;
    }
    request = magic2_dispatch_request_initializer();
    request.operation = MAGIC2_OP_GRAPH_COMPILE;
    request.args.graph_graph_compile.graph = state->graph;
    request.args.graph_graph_compile.info = &info;
    if (dispatch(&request) != MAGIC2_OK || info.node_count != 1U ||
        info.scratch_bytes != 0U) {
        return -1;
    }
    return 0;
}

static void request_task(void *arg) {
    magic2_test_state_t *state = arg;
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    magic2_graph_binding binding = MAGIC2_GRAPH_BINDING_INIT;
    magic2_graph_run_status status = MAGIC2_GRAPH_RUN_STATUS_INIT;

    binding.value_index = 0U;
    binding.data = &state->output;
    binding.bytes = sizeof(state->output);
    request.operation = MAGIC2_OP_GRAPH_RUN;
    request.args.graph_graph_run.graph = state->graph;
    request.args.graph_graph_run.bindings = &binding;
    request.args.graph_graph_run.binding_count = 1U;
    request.args.graph_graph_run.status = &status;
    state->bridge_result = llam_magic2_dispatch(&request, &state->dispatch_status);
    state->bridge_errno = errno;
    state->implementation_status = request.implementation_status;
    if (!state->cancel_before &&
        atomic_load_explicit(&state->finished, memory_order_acquire) == 0U) {
        fail(state, "bridge returned while a native callback still held its arguments");
    }
    atomic_store_explicit(&state->returned, 1U, memory_order_release);
}

static void controller_task(void *arg) {
    magic2_test_state_t *state = arg;
    llam_spawn_opts_t options;
    llam_task_t *request;
    uint64_t deadline;
    magic2_dispatch_request probe = MAGIC2_REQUEST_INIT;
    magic2_capabilities_info capabilities = MAGIC2_CAPABILITIES_INFO_INIT;
    int status = 0;

    probe.operation = MAGIC2_OP_GET_CAPABILITIES;
    probe.args.get_capabilities.out_info = &capabilities;
    if (!llam_worker_runtime_critical_enter(g_llam_tls_worker)) {
        fail(state, "could not enter runtime-critical test region");
        return;
    }
    if (llam_magic2_dispatch(&probe, &status) != -1 || errno != EBUSY ||
        status != MAGIC2_ENOTPROCESSED || capabilities.features != 0U) {
        fail(state, "runtime-critical dispatch was not rejected before execution");
    }
    if (!llam_worker_runtime_critical_leave(g_llam_tls_worker)) {
        fail(state, "could not leave runtime-critical test region");
        return;
    }
    if (!llam_worker_preempt_disable(g_llam_tls_worker)) {
        fail(state, "could not enter preemption-disabled test region");
        return;
    }
    if (llam_magic2_dispatch(&probe, &status) != -1 || errno != EBUSY ||
        status != MAGIC2_ENOTPROCESSED || capabilities.features != 0U) {
        fail(state, "preemption-disabled dispatch was not rejected before execution");
    }
    if (!llam_worker_preempt_enable(g_llam_tls_worker)) {
        fail(state, "could not leave preemption-disabled test region");
        return;
    }

    state->token = llam_cancel_token_create();
    if (state->token == NULL ||
        llam_spawn_opts_init(&options, sizeof(options)) != 0) {
        fail(state, "could not create cancellation fixture");
        return;
    }
    options.cancel_token = state->token;
    if (state->cancel_before && llam_cancel_token_cancel(state->token) != 0) {
        fail(state, "could not cancel before submission");
    }
    request = llam_spawn_ex(request_task, state, &options, sizeof(options));
    if (request == NULL) {
        fail(state, "could not spawn request task");
        (void)llam_cancel_token_destroy(state->token);
        state->token = NULL;
        return;
    }
    if (!state->cancel_before) {
        deadline = llam_now_ns() + UINT64_C(5000000000);
        while (atomic_load_explicit(&state->started, memory_order_acquire) == 0U &&
               atomic_load_explicit(&state->returned, memory_order_acquire) == 0U &&
               llam_now_ns() < deadline) {
            llam_yield();
        }
        if (atomic_load_explicit(&state->started, memory_order_acquire) == 0U) {
            fail(state, "native callback did not start");
        }
        if (g_llam_tls_processor == NULL ||
            atomic_load_explicit(&g_llam_tls_worker->current_p, memory_order_acquire) !=
                g_llam_tls_processor) {
            fail(state, "controller lost its processor ownership");
        }
        if (state->cancel_running) {
            if (llam_cancel_token_cancel(state->token) != 0) {
                fail(state, "could not cancel running dispatch");
            }
            for (unsigned i = 0U; i < 8U; ++i) {
                llam_yield();
            }
            if (atomic_load_explicit(&state->returned, memory_order_acquire) != 0U) {
                fail(state, "cancellation resumed request before native completion");
            }
        }
        atomic_store_explicit(&state->release, 1U, memory_order_release);
    }
    if (llam_join(request) != 0) {
        fail(state, "request task join failed");
    }
    if (llam_cancel_token_destroy(state->token) != 0) {
        fail(state, "cancellation token did not drain");
    }
    state->token = NULL;
}

static int test_case(bool cancel_before, bool cancel_running, int kernel_result) {
    magic2_test_state_t state;
    llam_runtime_opts_t options;
    llam_task_t *controller = NULL;
    magic2_dispatch_request destroy = MAGIC2_REQUEST_INIT;
    int result = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.calls, 0U);
    atomic_init(&state.started, 0U);
    atomic_init(&state.release, 0U);
    atomic_init(&state.finished, 0U);
    atomic_init(&state.returned, 0U);
    state.cancel_before = cancel_before;
    state.cancel_running = cancel_running;
    state.kernel_result = kernel_result;
    state.output = -99;
    state.bridge_result = -99;
    state.dispatch_status = -99;
    if (create_graph(&state) != 0 ||
        llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        fail(&state, "graph fixture creation failed");
        goto done;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create(&options, sizeof(options), &state.runtime) != 0) {
        fail(&state, "runtime creation failed");
        goto done;
    }
    controller = llam_runtime_spawn_ex(state.runtime, controller_task, &state, NULL, 0U);
    if (controller == NULL || llam_runtime_run_handle(state.runtime) != 0 ||
        llam_join(controller) != 0) {
        fail(&state, "controller execution failed");
        goto done;
    }
    if (atomic_load_explicit(&state.returned, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.calls, memory_order_acquire) !=
            (cancel_before ? 0U : 1U)) {
        fail(&state, "dispatch was missing or a kernel was replayed");
    }
    if (state.bridge_result != ((cancel_before || cancel_running) ? -1 : 0) ||
        ((cancel_before || cancel_running) && state.bridge_errno != ECANCELED)) {
        fail(&state, "LLAM submission/cancellation result was lost");
    }
    if (state.dispatch_status != (cancel_before ? MAGIC2_ENOTPROCESSED :
                                 kernel_result != 0 ? MAGIC2_EIMPLEMENTATION : MAGIC2_OK) ||
        state.implementation_status != (cancel_before ? 0 : kernel_result) ||
        state.output != (cancel_before ? -99 : 123)) {
        fail(&state, "magic2 result, kernel result, or prior writes were lost");
    }
    result = atomic_load_explicit(&state.failures, memory_order_acquire) != 0U;
done:
    llam_runtime_destroy(state.runtime);
    if (state.graph != NULL) {
        destroy.operation = MAGIC2_OP_GRAPH_DESTROY;
        destroy.args.graph_graph_destroy.graph = &state.graph;
        destroy.args.graph_graph_destroy.policy = MAGIC2_DESTROY_DRAIN;
        if (dispatch(&destroy) != MAGIC2_OK || state.graph != NULL) {
            fail(&state, "graph destruction failed");
            result = 1;
        }
    }
    return result;
}

int main(void) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    magic2_capabilities_info info = MAGIC2_CAPABILITIES_INFO_INIT;
    int status = 0;

    if (llam_magic2_dispatch(NULL, &status) != -1 || errno != EINVAL ||
        status != MAGIC2_ENOTPROCESSED ||
        llam_magic2_dispatch(&request, NULL) != -1 || errno != EINVAL) {
        fprintf(stderr, "test_magic2_internal: invalid bridge arguments accepted\n");
        return 1;
    }
    request.operation = MAGIC2_OP_GET_CAPABILITIES;
    request.args.get_capabilities.out_info = &info;
    if (dispatch(&request) != MAGIC2_OK ||
        (info.features & MAGIC2_FEATURE_SINGLE_ENTRY) == 0U) {
        fprintf(stderr, "test_magic2_internal: unmanaged capability dispatch failed\n");
        return 1;
    }
    request.tag = 0U;
    if (dispatch(&request) != MAGIC2_EABI) {
        fprintf(stderr, "test_magic2_internal: malformed request escaped validation\n");
        return 1;
    }
    if (test_case(false, false, 0) != 0 ||
        test_case(false, false, 73) != 0 ||
        test_case(false, true, 0) != 0 ||
        test_case(true, false, 0) != 0) {
        return 1;
    }
    puts("test_magic2_internal: dispatch, one-P progress, failure, and cancellation passed");
    return test_magic2_cpu_internal() != 0 || test_magic2_auto_internal() != 0;
}
