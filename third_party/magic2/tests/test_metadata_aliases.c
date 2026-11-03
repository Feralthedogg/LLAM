/* Regression coverage for metadata writes that must not alias execution data. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int pair_kernel(
    void *output, const void *input, size_t count,
    void *call_user, void *candidate_user) {
    (void)input;
    (void)count;
    (void)call_user;
    (void)candidate_user;
    *(int *)output = 42;
    return 0;
}

static int buffer_kernel(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user) {
    (void)count;
    (void)call_user;
    (void)candidate_user;
    if (buffer_count < 2u) return -1;
    *(int *)buffers[1].data = *(const int *)buffers[0].data + 1;
    return 0;
}

static magic2_tuner *make_pair_tuner(void) {
    magic2_pair_candidate candidate;
    magic2_adaptive_config tuning = magic2_adaptive_config_initializer();
    magic2_tuner_spec spec = magic2_tuner_spec_initializer();
    magic2_tuner *tuner = NULL;
    memset(&candidate, 0, sizeof(candidate));
    candidate.struct_size = (uint32_t)sizeof(candidate);
    candidate.general_fn = pair_kernel;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    tuning.output_bytes = sizeof(int);
    tuning.maximum_input_bytes = sizeof(int);
    tuning.maximum_overlap_span = sizeof(int) * 2u;
    tuning.candidate_count = 1u;
    spec.kind = MAGIC2_PLAN_KIND_PAIR;
    spec.candidate_count = 1u;
    spec.u.pair.candidates = &candidate;
    spec.u.pair.tuning = tuning;
    assert(magic2_tuner_create(&spec, &tuner) == MAGIC2_OK);
    return tuner;
}

static magic2_tuner *make_buffer_tuner(void) {
    magic2_buffer_candidate candidate;
    magic2_adaptive_buffer_config tuning =
        magic2_adaptive_buffer_config_initializer();
    magic2_tuner_spec spec = magic2_tuner_spec_initializer();
    magic2_tuner *tuner = NULL;
    memset(&candidate, 0, sizeof(candidate));
    candidate.struct_size = (uint32_t)sizeof(candidate);
    candidate.general_fn = buffer_kernel;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    tuning.maximum_buffer_count = 2u;
    tuning.session_count = 1u;
    tuning.maximum_total_buffer_bytes = 128u;
    tuning.async_operation_capacity = 0u;
    spec.kind = MAGIC2_PLAN_KIND_BUFFERS;
    spec.candidate_count = 1u;
    spec.u.buffers.candidates = &candidate;
    spec.u.buffers.tuning = tuning;
    assert(magic2_tuner_create(&spec, &tuner) == MAGIC2_OK);
    return tuner;
}

static void test_dispatch_batch_alias(void) {
    magic2_dispatch_request request = MAGIC2_REQUEST_INIT;
    magic2_dispatch_request child = MAGIC2_REQUEST_INIT;
    magic2_capabilities_info capabilities = MAGIC2_CAPABILITIES_INFO_INIT;
    void *requests[1];
    void *before;
    size_t processed = SIZE_MAX;
    int result;

    child.operation = MAGIC2_OP_GET_CAPABILITIES;
    child.args.get_capabilities.out_info = &capabilities;
    requests[0] = &child;
    before = requests[0];
    request.operation = MAGIC2_OP_BATCH;
    request.implementation_status = 77;
    request.args.batch.requests = requests;
    request.args.batch.request_count = 1u;
    request.args.batch.results = (int *)requests;
    request.args.batch.processed_count = &processed;

    result = magic2(&request);
    assert(result == MAGIC2_EOVERLAP);
    assert(requests[0] == before);
    assert(processed == SIZE_MAX);
    assert(request.implementation_status == 77);
}

static void test_pair_status_alias(void) {
    magic2_tuner *tuner = make_pair_tuner();
    magic2_pair_call call = magic2_pair_call_initializer();
    int input = 5;
    int output = 10;
    int implementation_status = -99;

    call.output = &output;
    call.output_bytes = sizeof(output);
    call.input = &input;
    call.input_bytes = sizeof(input);
    call.count = 1u;
    assert(magic2_tuner_run_pair(
               tuner, &call, &implementation_status) == MAGIC2_OK);
    assert(output == 42);
    assert(implementation_status == 0);

    output = 10;
    assert(magic2_tuner_run_pair(
               tuner, &call, &output) == MAGIC2_EOVERLAP);
    assert(output == 10);

    input = 5;
    assert(magic2_tuner_run_pair(
               tuner, &call, &input) == MAGIC2_EOVERLAP);
    assert(input == 5);

    assert(magic2_tuner_destroy(&tuner, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
}

static void test_buffer_status_alias(void) {
    magic2_tuner *tuner = make_buffer_tuner();
    magic2_buffer buffers[2];
    magic2_tuner_buffer_call call = magic2_tuner_buffer_call_initializer();
    magic2_run_status status = magic2_run_status_initializer();
    magic2_run_status before;
    union {
        uintptr_t alignment;
        unsigned char bytes[128];
    } output_storage;
    int input = 41;

    status.generation = UINT64_C(0x123456789abcdef0);
    status.implementation_status = 91;
    before = status;
    memcpy(output_storage.bytes, &status, sizeof(status));
    memset(buffers, 0, sizeof(buffers));
    buffers[0].data = &input;
    buffers[0].bytes = sizeof(input);
    buffers[0].alignment = 1u;
    buffers[0].access = MAGIC2_BUFFER_READ;
    buffers[0].alias_group = MAGIC2_ALIAS_GROUP_NONE;
    buffers[1].data = output_storage.bytes;
    buffers[1].bytes = sizeof(output_storage.bytes);
    buffers[1].alignment = 1u;
    buffers[1].access = MAGIC2_BUFFER_WRITE;
    buffers[1].alias_group = MAGIC2_ALIAS_GROUP_NONE;
    call.buffers = buffers;
    call.buffer_count = 2u;
    call.count = 1u;

    assert(magic2_tuner_run_buffers(
               tuner, &call, (magic2_run_status *)output_storage.bytes) ==
           MAGIC2_EOVERLAP);
    memcpy(&status, output_storage.bytes, sizeof(status));
    assert(memcmp(&status, &before, sizeof(status)) == 0);
    assert(magic2_tuner_destroy(&tuner, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
}

int main(void) {
    test_dispatch_batch_alias();
    test_pair_status_alias();
    test_buffer_status_alias();
    puts("magic2 metadata alias regressions passed");
    return 0;
}
