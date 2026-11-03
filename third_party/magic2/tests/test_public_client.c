/* Link and execute the public CPU runtime from a separate implementation TU. */
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int client_tile(
    const void *environment, void *const *ports, size_t begin, size_t end,
    uint32_t slot, void *scratch) {
    int *output = (int *)ports[0];
    const int *input = (const int *)ports[1];
    size_t index;
    (void)environment;
    (void)slot;
    (void)scratch;
    for (index = begin; index < end; ++index) output[index] = input[index] + 9;
    return 0;
}

static magic2_cpu_plan *client_plan(
    const magic2_port *ports, uint32_t schedule, uint32_t workers) {
    magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;
    magic2_cpu_plan *plan = NULL;
    spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
    spec.schedule = schedule;
    spec.stable_id.low = schedule;
    spec.stable_id.high = workers;
    spec.shape.operation_id.low = 1u;
    spec.shape.semantic_id.low = 2u;
    spec.shape.count = 1024u;
    spec.shape.port_count = 2u;
    spec.shape.region_count = 2u;
    spec.shape.ports = ports;
    spec.tile = client_tile;
    spec.grain = 256u;
    spec.worker_limit = workers;
    spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
    assert(magic2_cpu_plan_create(&spec, &plan) == MAGIC2_OK);
    return plan;
}

int main(void) {
    magic2_cpu_executor_config executor_config = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
    magic2_cpu_executor_info executor_info = MAGIC2_CPU_EXECUTOR_INFO_INIT;
    magic2_cpu_resource_info resources = MAGIC2_CPU_RESOURCE_INFO_INIT;
    magic2_cpu_executor *executor = NULL;
    magic2_cpu_plan *caller;
    magic2_cpu_plan *worker;
    magic2_cpu_plan *plans[2];
    magic2_cpu_family_config family_config = MAGIC2_CPU_FAMILY_CONFIG_INIT;
    magic2_cpu_family_info family_info = MAGIC2_CPU_FAMILY_INFO_INIT;
    magic2_cpu_family_status family_status = MAGIC2_CPU_FAMILY_STATUS_INIT;
    magic2_cpu_family *family = NULL;
    magic2_cpu_evidence evidence = MAGIC2_CPU_EVIDENCE_INIT;
    magic2_cpu_plan_info plan_info = MAGIC2_CPU_PLAN_INFO_INIT;
    magic2_cpu_run_status run_status = MAGIC2_CPU_RUN_STATUS_INIT;
    magic2_cpu_frame frame = MAGIC2_CPU_FRAME_INIT;
    magic2_cpu_ticket *ticket = NULL;
    magic2_port ports[2];
    magic2_region regions[2];
    int input[1024];
    int output[1024];
    void *allocation;
    void *workspace;
    uintptr_t raw;
    size_t index;
    size_t profile_size = 0u;
    unsigned char *profile = NULL;
    magic2_cpu_profile_import_result import_result =
        MAGIC2_CPU_PROFILE_IMPORT_RESULT_INIT;
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
    for (index = 0u; index < 1024u; ++index) input[index] = (int)index;
    regions[0].base = output;
    regions[0].capacity = sizeof(output);
    regions[1].base = input;
    regions[1].capacity = sizeof(input);
    executor_config.max_workers = 2u;
    assert(magic2_cpu_query_resources(&resources) == MAGIC2_OK);
    assert(magic2_cpu_executor_create(
        &executor_config, &executor) == MAGIC2_OK);
    assert(magic2_cpu_executor_get_info(executor, &executor_info) == MAGIC2_OK);
    caller = client_plan(ports, MAGIC2_CPU_SCHEDULE_CALLER, 1u);
    worker = client_plan(ports, MAGIC2_CPU_SCHEDULE_DYNAMIC, 2u);
    plans[0] = caller;
    plans[1] = worker;
    family_config.initial_samples = 1u;
    assert(magic2_cpu_family_create(
        &family_config, plans, 2u, &family) == MAGIC2_OK);
    assert(magic2_cpu_family_get_info(family, &family_info) == MAGIC2_OK);
    allocation = malloc(
        family_info.workspace.bytes + family_info.workspace.alignment - 1u);
    assert(allocation != NULL);
    raw = (uintptr_t)allocation;
    workspace = (void *)((raw + family_info.workspace.alignment - 1u) &
        ~(uintptr_t)(family_info.workspace.alignment - 1u));
    assert(magic2_cpu_family_run(
        family, executor, NULL, regions, 2u, workspace,
        family_info.workspace.bytes, &family_status) == MAGIC2_OK);
    assert(magic2_cpu_family_get_evidence(
        family, 0u, family_status.executed_index, &evidence) == MAGIC2_OK);
    assert(evidence.samples == 1u);
    assert(magic2_cpu_family_profile_export(
        family, NULL, 0u, &profile_size) == MAGIC2_OK);
    profile = (unsigned char *)malloc(profile_size);
    assert(profile != NULL);
    assert(magic2_cpu_family_profile_export(
        family, profile, profile_size, &profile_size) == MAGIC2_OK);
    assert(magic2_cpu_family_reset(family) == MAGIC2_OK);
    assert(magic2_cpu_family_profile_import(
        family, profile, profile_size, 500u, &import_result) == MAGIC2_OK);

    assert(magic2_cpu_plan_get_info(caller, &plan_info) == MAGIC2_OK);
    assert(magic2_cpu_frame_init(
        &frame, workspace, family_info.workspace.bytes) == MAGIC2_OK);
    assert(magic2_cpu_frame_bind(caller, regions, 2u, &frame) == MAGIC2_OK);
    assert(magic2_cpu_frame_run(&frame, NULL, &run_status) == MAGIC2_OK);
    assert(magic2_cpu_frame_unbind(&frame) == MAGIC2_OK);

    run_status = magic2_cpu_run_status_initializer();
    assert(magic2_cpu_plan_submit(
        worker, executor, regions, 2u, workspace,
        family_info.workspace.bytes, &ticket) == MAGIC2_OK);
    assert(magic2_cpu_ticket_wait(
        ticket, UINT64_MAX, &run_status) == MAGIC2_OK);
    assert(magic2_cpu_ticket_poll(ticket, &run_status) == MAGIC2_OK);
    assert(magic2_cpu_ticket_cancel(ticket) == MAGIC2_OK);
    assert(magic2_cpu_ticket_release(&ticket) == MAGIC2_OK);
    for (index = 0u; index < 1024u; ++index)
        assert(output[index] == input[index] + 9);
    assert(magic2_cpu_family_destroy(&family) == MAGIC2_OK);
    magic2_cpu_plan_release(&worker);
    magic2_cpu_plan_release(&caller);
    assert(magic2_cpu_executor_destroy(&executor) == MAGIC2_OK);
    free(profile);
    free(allocation);
    puts("magic2 public CPU client passed");
    return 0;
}
