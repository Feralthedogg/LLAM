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
 * See LICENSE.
 */

#include "llam/scope.h"
#include <inttypes.h>
#include <stdio.h>

/* Plain-C single-file example: explicitly use the compatibility constructor.
 * The installed SDK test compiles managed C task code separately from its C++
 * host and uses the default ASYNC_REQUIRED constructor with the same scope API. */
static const llam_type_descriptor_t number_type = LLAM_TYPE_U64_INIT;
typedef struct session {
    llam_runtime_t *runtime;
    llam_scope_t *scope;
    llam_future_t *future;
    llam_value_t *argument;
    llam_outcome_t result;
    llam_result_contract_t contract;
} session_t;

static void double_value(void *data) {
    uint64_t number = *(uint64_t *)data * 2U;
    llam_value_t *value = NULL;
    if (llam_value_copy_ex(&number_type, sizeof(number_type), &number, &value) != 0) return;
    llam_outcome_t outcome = {LLAM_OUTCOME_OK, 0, value, 0U};
    if (llam_task_set_outcome_ex(&outcome, sizeof(outcome)) != 0) {
        if (llam_value_release(value) != 0) perror("release result");
    }
}
static int prepare(session_t *session) {
    llam_runtime_opts_t options;
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return -1;
    options.deterministic = 1U;
    if (llam_runtime_create_compat(&options, sizeof(options), &session->runtime) != 0) return -1;
    if (llam_runtime_scope_create_ex(session->runtime, NULL, 0U, &session->scope) != 0) return -1;
    llam_typed_task_spec_t spec;
    if (llam_typed_task_spec_init(&spec, sizeof(spec)) != 0) return -1;
    spec.entry = double_value;
    spec.result.success = number_type;
    session->contract = spec.result;
    uint64_t input = 21U;
    if (llam_value_copy_ex(&number_type, sizeof(number_type), &input, &session->argument) != 0) return -1;
    spec.argument = session->argument;
    if (llam_scope_spawn_ex(session->scope, &spec, sizeof(spec), &session->future) != 0) return -1;
    session->argument = NULL;
    return 0;
}
static int execute(session_t *session) {
    if (llam_runtime_run_handle(session->runtime) != 0) return -1;
    if (llam_scope_drain_until(session->scope, 0U, NULL, 0U) != 0) return -1;
    if (llam_future_take_ex(session->future, &session->contract, sizeof(session->contract),
                            &session->result, sizeof(session->result)) != 0) return -1;
    session->future = NULL;
    if (session->result.kind != LLAM_OUTCOME_OK) {
        fprintf(stderr, "task outcome=%u runtime_error=%d\n", session->result.kind, session->result.runtime_error);
        return -1;
    }
    void *data = NULL;
    if (llam_value_data_ex(session->result.value, &number_type, sizeof(number_type), &data) != 0) return -1;
    printf("scoped result: %" PRIu64 "\n", *(uint64_t *)data);
    return 0;
}
static int dispose(session_t *session) {
    int failed = 0;
    if (session->scope && llam_scope_cancel(session->scope) != 0) failed = -1;
    /* This host is the only runner. Teardown completes pending execution before
     * releasing remaining handles; a cleanup timeout alone would not allow it. */
    if (session->runtime) llam_runtime_destroy(session->runtime);
    if (session->future && llam_future_release(session->future) != 0) failed = -1;
    if (session->argument && llam_value_release(session->argument) != 0) failed = -1;
    if (session->result.value && llam_value_release(session->result.value) != 0) failed = -1;
    if (session->scope) {
        if (llam_scope_drain_until(session->scope, 0U, NULL, 0U) != 0) failed = -1;
        if (llam_scope_destroy(session->scope) != 0) failed = -1;
    }
    return failed;
}
int main(void) {
    session_t session = {0};
    int failed = prepare(&session);
    if (failed == 0) failed = execute(&session);
    if (failed != 0) perror("scoped result");
    if (dispose(&session) != 0) { perror("scope cleanup"); failed = -1; }
    return failed != 0;
}
