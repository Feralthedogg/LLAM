/*
 * @file example.c
 * @brief Minimal exact-shape CPU plan example.
 */
#define MAGIC2_IMPLEMENTATION
#include "magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int multiply_tile(
    const void *environment, void *const *ports, size_t begin, size_t end,
    uint32_t slot, void *scratch) {
    float *output = (float *)ports[0];
    const float *input = (const float *)ports[1];
    size_t index;
    (void)environment;
    (void)slot;
    (void)scratch;
    for (index = begin; index < end; ++index) output[index] = input[index] * 2.0f;
    return 0;
}

int main(void) {
    enum { count = 4096 };
    float input[count];
    float output[count];
    magic2_port ports[2] = { { 0 } };
    magic2_region regions[2];
    magic2_cpu_plan_spec plan_spec = MAGIC2_CPU_PLAN_SPEC_INIT;
    magic2_cpu_plan_info plan_info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status run_status = MAGIC2_CPU_RUN_STATUS_INIT;
    magic2_cpu_executor_config executor_config = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor *executor = NULL;
    magic2_cpu_plan *plan = NULL;
    void *workspace_allocation;
    void *workspace;
    uintptr_t raw;
    size_t index;

    for (index = 0u; index < count; ++index) input[index] = (float)index;
    ports[0].bytes = sizeof(output);
    ports[0].stride = sizeof(float);
    ports[0].alignment = sizeof(float);
    ports[0].region = 0u;
    ports[0].access = MAGIC2_BUFFER_WRITE;
    ports[0].present = 1u;
    ports[0].initialization = MAGIC2_ZERO;
    ports[1] = ports[0];
    ports[1].region = 1u;
    ports[1].access = MAGIC2_BUFFER_READ;
    ports[1].initialization = MAGIC2_PRESERVE;
    regions[0].base = output;
    regions[0].capacity = sizeof(output);
    regions[1].base = input;
    regions[1].capacity = sizeof(input);

    plan_spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
    plan_spec.schedule = MAGIC2_CPU_SCHEDULE_DYNAMIC;
    plan_spec.stable_id.low = UINT64_C(0x6578616d706c65);
    plan_spec.shape.operation_id.low = UINT64_C(0x6d6167696332);
    plan_spec.shape.semantic_id.low = UINT64_C(0x6d756c7469706c79);
    plan_spec.shape.count = count;
    plan_spec.shape.port_count = 2u;
    plan_spec.shape.region_count = 2u;
    plan_spec.shape.ports = ports;
    plan_spec.tile = multiply_tile;
    plan_spec.grain = 256u;
    plan_spec.worker_limit = 2u;
    plan_spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
    executor_config.max_workers = 2u;
    assert(magic2_cpu_executor_create(&executor_config, &executor) == MAGIC2_OK);
    assert(magic2_cpu_plan_create(&plan_spec, &plan) == MAGIC2_OK);
    assert(magic2_cpu_plan_get_info(plan, &plan_info) == MAGIC2_OK);
    workspace_allocation = malloc(
        plan_info.workspace.bytes + plan_info.workspace.alignment - 1u);
    assert(workspace_allocation != NULL);
    raw = (uintptr_t)workspace_allocation;
    workspace = (void *)((raw + plan_info.workspace.alignment - 1u) &
        ~(uintptr_t)(plan_info.workspace.alignment - 1u));
    assert(magic2_cpu_plan_run(
        plan, executor, regions, 2u, workspace,
        plan_info.workspace.bytes, &run_status) == MAGIC2_OK);
    for (index = 0u; index < count; ++index) assert(output[index] == input[index] * 2.0f);
    printf("magic2 example: %zu tiles, %u workers, output[0]=%.1f\n",
        run_status.total_tiles, run_status.used_workers, output[0]);
    free(workspace_allocation);
    magic2_cpu_plan_release(&plan);
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    return 0;
}
