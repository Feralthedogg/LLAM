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
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "test_scope_values:%u: %s errno=%d\n", (unsigned)__LINE__, #test, errno); \
    return false; \
} } while (0)

_Static_assert(sizeof(llam_type_descriptor_t) == 32U, "fixed type ABI");
_Static_assert(sizeof(llam_result_contract_t) == 64U, "fixed result contract ABI");
static const llam_type_descriptor_t integer_type = {UINT64_C(0x6c6c616d74657374), 1U, 8U, 8U, 0U};

static void counted_drop(void *data, void *context) {
    unsigned *count = context;
    *count += 1U;
    free(data);
}

static bool copied_value_contract(void) {
    uint64_t input = UINT64_C(0xfedcba9876543210);
    llam_value_t *value = NULL;
    CHECK(llam_value_copy_ex(&integer_type, sizeof(integer_type), &input, &value) == 0);
    input = 0U;
    void *data = NULL;
    CHECK(llam_value_data_ex(value, &integer_type, sizeof(integer_type), &data) == 0);
    CHECK(*(uint64_t *)data == UINT64_C(0xfedcba9876543210));
    llam_type_descriptor_t wrong = integer_type;
    wrong.id_low = 2U;
    void *unchanged = (void *)(uintptr_t)1U;
    CHECK(llam_value_data_ex(value, &wrong, sizeof(wrong), &unchanged) == -1 && errno == EINVAL);
    CHECK(unchanged == (void *)(uintptr_t)1U);
    CHECK(llam_value_release(value) == 0);
    CHECK(llam_value_release(value) == -1 && errno == EINVAL);
    CHECK(llam_value_release((llam_value_t *)(uintptr_t)1U) == -1 && errno == EINVAL);
    return true;
}

static bool adoption_and_rollback(void) {
    unsigned drops = 0U;
    llam_value_spec_t spec;
    CHECK(llam_value_spec_init(&spec, sizeof(spec)) == 0);
    spec.type = integer_type;
    spec.data = malloc(sizeof(uint64_t));
    CHECK(spec.data != NULL);
    *(uint64_t *)spec.data = 42U;
    spec.drop = counted_drop;
    spec.drop_context = &drops;
    llam_value_t *value = (llam_value_t *)(uintptr_t)1U;
    CHECK(llam_value_create_ex(&spec, sizeof(spec) - 1U, &value) == -1 && errno == EINVAL);
    CHECK(value == (llam_value_t *)(uintptr_t)1U && drops == 0U);
    CHECK(*(uint64_t *)spec.data == 42U);
    CHECK(llam_value_create_ex(&spec, sizeof(spec), &value) == 0);
    CHECK(llam_value_release(value) == 0 && drops == 1U);
    CHECK(llam_value_release(value) == -1 && drops == 1U);
    return true;
}

static bool alignment_and_bounds(void) {
    unsigned char bytes[64];
    memset(bytes, 0x3c, sizeof(bytes));
    llam_type_descriptor_t type = {1U, 2U, sizeof(bytes), 64U, 0U};
    llam_value_t *value = NULL;
    void *data = NULL;
    CHECK(llam_value_copy_ex(&type, sizeof(type), bytes, &value) == 0);
    CHECK(llam_value_data_ex(value, &type, sizeof(type), &data) == 0);
    CHECK(((uintptr_t)data & 63U) == 0U && memcmp(data, bytes, sizeof(bytes)) == 0);
    CHECK(llam_value_release(value) == 0);
    value = (llam_value_t *)(uintptr_t)1U;
    type.size = UINT64_MAX;
    CHECK(llam_value_copy_ex(&type, sizeof(type), bytes, &value) == -1 && errno == EINVAL);
    CHECK(value == (llam_value_t *)(uintptr_t)1U);
    type.size = 8U;
    type.alignment = 3U;
    CHECK(llam_value_copy_ex(&type, sizeof(type), bytes, &value) == -1 && errno == EINVAL);
    return true;
}

static bool initialization_and_unit(void) {
    struct { llam_result_contract_t contract; uint64_t tail; } output;
    output.tail = UINT64_C(0x12345678);
    CHECK(llam_result_contract_init(&output.contract, sizeof(output)) == 0);
    CHECK(output.tail == UINT64_C(0x12345678));
    CHECK(output.contract.success.alignment == 1U && output.contract.error.alignment == 1U);
    llam_value_t *unit = NULL;
    CHECK(llam_value_copy_ex(&output.contract.success, sizeof(output.contract.success), NULL, &unit) == 0);
    void *data = (void *)(uintptr_t)1U;
    CHECK(llam_value_data_ex(unit, &output.contract.success, sizeof(output.contract.success), &data) == 0);
    CHECK(data == NULL);
    CHECK(llam_value_release(unit) == 0);
    output.contract.success.size = 8U;
    CHECK(llam_value_copy_ex(&output.contract.success, sizeof(output.contract.success), &data, &unit) == -1);
    return true;
}

int main(void) {
    if (!copied_value_contract() || !adoption_and_rollback() ||
        !alignment_and_bounds() || !initialization_and_unit()) return 1;
    puts("typed values: ownership, layout, prefix, rollback and stale handles passed");
    return 0;
}
