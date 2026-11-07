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
#include "structured_internal.h"

int llam_scope_copy_input(void *out, size_t size, const void *input, size_t input_size) {
    if (input == NULL || input_size < size) { errno = EINVAL; return -1; }
    memcpy(out, input, size);
    return 0;
}

bool llam_type_is_unit(const llam_type_descriptor_t *type) {
    return type->id_high == 0U && type->id_low == 0U;
}

int llam_type_validate(const llam_type_descriptor_t *type) {
    if (type->reserved != 0U || type->alignment == 0U ||
        (type->alignment & (type->alignment - 1U)) != 0U || type->size > SIZE_MAX ||
        (llam_type_is_unit(type) && (type->size != 0U || type->alignment != 1U))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

bool llam_type_equal(const llam_type_descriptor_t *a, const llam_type_descriptor_t *b) {
    return a->id_high == b->id_high && a->id_low == b->id_low && a->size == b->size &&
           a->alignment == b->alignment && a->reserved == b->reserved;
}

int llam_contract_validate(const llam_result_contract_t *contract) {
    return llam_type_validate(&contract->success) != 0 ? -1 : llam_type_validate(&contract->error);
}

bool llam_contract_equal(const llam_result_contract_t *a, const llam_result_contract_t *b) {
    return llam_type_equal(&a->success, &b->success) && llam_type_equal(&a->error, &b->error);
}

static int llam_scope_init_output(void *out, size_t size, const void *defaults, size_t required) {
    if (out == NULL || size < required) { errno = EINVAL; return -1; }
    memcpy(out, defaults, required);
    return 0;
}

int llam_result_contract_init(llam_result_contract_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    const llam_result_contract_t defaults = {.success = {.alignment = 1U}, .error = {.alignment = 1U}};
    return llam_scope_init_output(out, size, &defaults, sizeof(defaults));
}

int llam_value_spec_init(llam_value_spec_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    const llam_value_spec_t defaults = {.type = {.alignment = 1U}};
    return llam_scope_init_output(out, size, &defaults, sizeof(defaults));
}

int llam_scope_opts_init(llam_scope_opts_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    const llam_scope_opts_t defaults = {0};
    return llam_scope_init_output(out, size, &defaults, sizeof(defaults));
}

int llam_typed_task_spec_init(llam_typed_task_spec_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    const llam_typed_task_spec_t defaults = {
        .result = {.success = {.alignment = 1U}, .error = {.alignment = 1U}}
    };
    return llam_scope_init_output(out, size, &defaults, sizeof(defaults));
}

int llam_outcome_init(llam_outcome_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    const llam_outcome_t defaults = {0};
    return llam_scope_init_output(out, size, &defaults, sizeof(defaults));
}
