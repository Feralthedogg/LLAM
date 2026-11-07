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
#ifndef LLAM_STRUCTURED_INTERNAL_H
#define LLAM_STRUCTURED_INTERNAL_H

#include "runtime_internal.h"
#include "runtime_public_slot.h"
#include "llam/scope.h"

typedef struct llam_structured_object llam_structured_object_t;
struct llam_structured_object {
    llam_runtime_t *owner;
    size_t slot;
    size_t refs;
    size_t public_ops;
    uint32_t generation;
    uint32_t family;
    bool public_live;
    bool claimed;
    void (*dispose)(llam_structured_object_t *object);
};

struct llam_value {
    llam_structured_object_t object;
    llam_value_spec_t spec;
};

struct llam_scope {
    llam_structured_object_t object;
    llam_cancel_token_t *cancel_token;
    llam_channel_t *done;
    llam_scope_report_t report;
    bool draining;
};

struct llam_future {
    llam_structured_object_t object;
    llam_scope_t *scope;
    llam_channel_t *done;
    llam_result_contract_t contract;
    llam_value_t *argument;
    llam_value_t *result;
    uint64_t task_id;
    uint32_t kind;
    int32_t runtime_error;
    bool entry_returned;
    bool ready;
    llam_value_t *discarded_result;
    llam_future_t *retired_next;
};

extern pthread_mutex_t g_llam_structured_lock;

void llam_structured_init(llam_structured_object_t *object, uint32_t family,
                          llam_runtime_t *owner, void (*dispose)(llam_structured_object_t *));
int llam_structured_publish_locked(llam_structured_object_t *object);
void *llam_structured_handle(const llam_structured_object_t *object);
llam_structured_object_t *llam_structured_resolve_locked(const void *handle, uint32_t family);
int llam_structured_retain_locked(llam_structured_object_t *object);
llam_structured_object_t *llam_structured_drop_locked(llam_structured_object_t *object);
void llam_structured_unpublish_locked(llam_structured_object_t *object);
int llam_structured_begin(const void *handle, uint32_t family, llam_structured_object_t **out);
void llam_structured_end(llam_structured_object_t *object);
void llam_structured_release(llam_structured_object_t *object);
int llam_structured_consume_locked(llam_structured_object_t *object);

int llam_scope_copy_input(void *out, size_t size, const void *input, size_t input_size);
bool llam_type_is_unit(const llam_type_descriptor_t *type);
int llam_type_validate(const llam_type_descriptor_t *type);
bool llam_type_equal(const llam_type_descriptor_t *a, const llam_type_descriptor_t *b);
int llam_contract_validate(const llam_result_contract_t *contract);
bool llam_contract_equal(const llam_result_contract_t *a, const llam_result_contract_t *b);
llam_value_t *llam_value_claim_locked(llam_value_t *handle);
int llam_value_take_locked(llam_value_t *handle, const llam_type_descriptor_t *expected,
                           llam_value_t **out);
void llam_value_commit_claim(llam_value_t *value);
void llam_value_rollback_claim(llam_value_t *value);

llam_future_t *llam_future_prepare(llam_scope_t *scope, const llam_result_contract_t *contract);
void llam_future_dispose(llam_structured_object_t *object);
void llam_scope_dispose(llam_structured_object_t *object);
void llam_structured_notify(llam_runtime_t *owner, llam_channel_t *channel);
int llam_structured_wait(llam_runtime_t *owner, llam_channel_t *done, uint64_t deadline);
bool llam_scope_finish_closing_locked(llam_scope_t *scope);
int llam_scope_request_cancel(llam_scope_t *scope);
void llam_scope_copy_report_locked(const llam_scope_t *scope, llam_scope_report_t *out, size_t size);
llam_task_t *llam_runtime_spawn_scoped(llam_runtime_t *runtime, llam_future_t *completion,
                                      const llam_typed_task_spec_t *spec, void *argument);

#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_scope_test_spawn_hook(void (*hook)(unsigned point, void *argument), void *argument);
#endif

#endif
