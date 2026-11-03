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

#include "runtime_internal.h"
#include "managed_catalog.h"
#include "managed_async_adapter.h"
#include "managed_image.h"
#if !LLAM_PLATFORM_WINDOWS
#include <dlfcn.h>
#endif

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(llam_preempt_opts_t) == 24U, "preempt options ABI");
_Static_assert(sizeof(llam_module_opts_t) == 24U, "module options ABI");
_Static_assert(sizeof(llam_preempt_info_t) == 168U, "preempt information ABI");
_Static_assert(offsetof(llam_preempt_opts_t, capsule_budget_bytes) == 8U, "preempt prefix ABI");
_Static_assert(offsetof(llam_module_opts_t, flags) == 16U, "module prefix ABI");

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "test_managed_catalog_internal:%d: %s (errno=%d)\n", \
                __LINE__, #expression, errno); \
        return -1; \
    } \
} while (0)

static void ordinary_task(void *argument) {
    unsigned *calls = argument;
    *calls += 1U;
}

static int check_options(void) {
    struct { llam_preempt_opts_t options; unsigned char tail[8]; } extended;
    memset(&extended, 0xa5, sizeof(extended));
    CHECK(llam_preempt_opts_init(&extended.options, sizeof(extended)) == 0);
    CHECK(extended.options.policy == LLAM_PREEMPT_POLICY_ASYNC_AUTO);
    CHECK(extended.options.flags == 0U && extended.options.reserved0 == 0U);
    CHECK(extended.options.native_affine_capacity == 4U);
    CHECK(extended.options.capsule_budget_bytes == UINT64_C(64) * 1024U * 1024U);
    for (size_t i = 0U; i < sizeof(extended.tail); ++i) CHECK(extended.tail[i] == 0xa5U);
    CHECK(llam_preempt_opts_init(NULL, sizeof(extended.options)) == -1 && errno == EINVAL);
    CHECK(llam_preempt_opts_init(&extended.options, 0U) == -1 && errno == EINVAL);
    llam_runtime_t *runtime = (llam_runtime_t *)(uintptr_t)1U;
    extended.options.flags = 1U;
    CHECK(llam_runtime_create_managed(NULL, 0U, &extended.options, sizeof(extended.options), &runtime) == -1);
    CHECK(errno == EINVAL && runtime == NULL);
    extended.options.flags = 0U;
    extended.options.reserved0 = 1U;
    CHECK(llam_runtime_create_managed(NULL, 0U, &extended.options, sizeof(extended.options), &runtime) == -1);
    CHECK(errno == EINVAL && runtime == NULL);
    extended.options.reserved0 = 0U;
    extended.options.native_affine_capacity = UINT32_MAX;
    CHECK(llam_runtime_create_managed(NULL, 0U, &extended.options, sizeof(extended.options), &runtime) == -1);
    CHECK(errno == EINVAL && runtime == NULL);
    CHECK(llam_managed_task_type_id(1U, 1U) == UINT64_C(0x7cf518d8f497ec78));
    CHECK(llam_managed_task_type_id(2U, 2U) == UINT64_C(0x639ba6d8e66a5014));
    CHECK(llam_managed_task_type_id(1U, 3U) == UINT64_C(0x7cf51ad8f497efde));
    CHECK(llam_managed_task_type_id(0U, 1U) == 0U);
    return 0;
}

typedef struct quantum_probe_state {
    llam_runtime_t *runtime;
    llam_preempt_info_t info;
    unsigned calls;
    int error;
} quantum_probe_state_t;

static void quantum_probe_task(void *argument) {
    quantum_probe_state_t *probe = argument;
    probe->error = llam_runtime_preempt_info_ex(probe->runtime, &probe->info, sizeof(probe->info));
    ++probe->calls;
}

static int check_admission_rollback_and_flags(const llam_runtime_opts_t *runtime_opts) {
    llam_preempt_opts_t opts;
    llam_preempt_info_t before, after;
    llam_runtime_t *runtime = NULL;
    CHECK(llam_preempt_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_runtime_create_managed(runtime_opts, sizeof(*runtime_opts), &opts, sizeof(opts), &runtime) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &before, sizeof(before)) == 0);
    union { llam_preempt_info_t alignment; unsigned char bytes[sizeof(llam_preempt_info_t)]; } prefix;
    memset(prefix.bytes, 0xa5, sizeof(prefix.bytes));
    CHECK(llam_runtime_preempt_info_ex(runtime, (llam_preempt_info_t *)prefix.bytes, 144U) == 0);
    for (size_t i = 144U; i < sizeof(prefix.bytes); ++i) CHECK(prefix.bytes[i] == 0xa5U);
    if (before.conservative_task_charge_bytes != 0U) {
        CHECK(before.prepared_capsules > 0U && before.available_capsules == before.prepared_capsules);
        CHECK(before.reserved_capsule_bytes == before.prepared_capsules * before.conservative_task_charge_bytes);
        CHECK(before.reserved_capsule_bytes <= before.capsule_budget_bytes && before.capsule_charge_bytes == 0U);
    } else {
        CHECK(before.prepared_capsules == 0U && before.available_capsules == 0U &&
              before.reserved_capsule_bytes == 0U);
    }
    for (unsigned i = 0U; i < 3U; ++i) CHECK(before.effective_quantum_ns[i] == 0U);
    unsigned calls = 0U;
    llam_spawn_opts_t spawn = {0};
    spawn.task_class = LLAM_TASK_CLASS_DEFAULT;
    spawn.flags = LLAM_SPAWN_F_NO_PREEMPT;
    spawn.cancel_token = (llam_cancel_token_t *)(uintptr_t)0x1234U;
    CHECK(llam_runtime_spawn_ex(runtime, ordinary_task, &calls, &spawn, sizeof(spawn)) == NULL && errno == EINVAL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0);
    CHECK(after.retained_references == before.retained_references);
    CHECK(after.excluded_tasks == 0U && after.reason == before.reason && calls == 0U);
    quantum_probe_state_t probe = {.runtime = runtime};
    llam_task_t *legacy = llam_runtime_spawn_ex(runtime, quantum_probe_task, &probe, NULL, 0U);
    CHECK(legacy != NULL);
    CHECK(llam_runtime_run_handle(runtime) == 0 && probe.calls == 1U && probe.error == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0);
    CHECK(after.legacy_tasks == 1U && after.compatibility_processors == 1U);
    if (before.reason == LLAM_PREEMPT_REASON_PROBE_PENDING) {
        CHECK(after.effective_policy == LLAM_PREEMPT_EFFECTIVE_MIXED);
        CHECK(after.reason == LLAM_PREEMPT_REASON_LEGACY_MIXED);
        CHECK((probe.info.flags & LLAM_PREEMPT_INFO_F_COMMITTED) != 0U);
        for (unsigned i = 0U; i < 3U; ++i) {
            CHECK(probe.info.effective_quantum_ns[i] > 0U);
            CHECK(probe.info.effective_quantum_ns[i] ==
                  llam_managed_async_effective_quantum(runtime, (llam_task_class_t)i));
        }
    } else {
        CHECK(after.effective_policy == LLAM_PREEMPT_POLICY_COOPERATIVE);
        for (unsigned i = 0U; i < 3U; ++i) CHECK(probe.info.effective_quantum_ns[i] == 0U);
    }
    for (unsigned i = 0U; i < 3U; ++i) CHECK(after.effective_quantum_ns[i] == 0U);
    CHECK(llam_join(legacy) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0 && after.legacy_tasks == 0U);
    CHECK(after.capsule_charge_bytes == 0U && after.available_capsules == after.prepared_capsules);
    /* A successful native barrier must not leak its effective policy into a
     * later startup that has not committed (or that is then aborted). */
    CHECK(llam_managed_catalog_run_begin(runtime) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0);
    CHECK(after.effective_policy == LLAM_PREEMPT_POLICY_COOPERATIVE);
    CHECK(after.flags == LLAM_PREEMPT_INFO_F_RUNNING);
    bool native_pending = after.reason == LLAM_PREEMPT_REASON_PROBE_PENDING;
    llam_managed_catalog_run_end(runtime);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0 && after.flags == 0U);
    CHECK(after.effective_policy == LLAM_PREEMPT_POLICY_COOPERATIVE);
    CHECK(after.reason == (native_pending ? LLAM_PREEMPT_REASON_STARTUP_FAILED
                                          : LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE));
    llam_runtime_destroy(runtime);
    runtime = NULL;
    CHECK(llam_runtime_create(runtime_opts, sizeof(*runtime_opts), &runtime) == 0);
    spawn.flags = LLAM_SPAWN_F_NATIVE_AFFINE;
    spawn.cancel_token = NULL;
    CHECK(llam_runtime_spawn_ex(runtime, ordinary_task, &calls, &spawn, sizeof(spawn)) == NULL && errno == ENOTSUP);
    CHECK(calls == 0U);
    llam_runtime_destroy(runtime);
    runtime = NULL;
    spawn.flags = LLAM_SPAWN_F_NO_PREEMPT;
    opts.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    int created = llam_runtime_create_managed(runtime_opts, sizeof(*runtime_opts), &opts, sizeof(opts), &runtime);
    if (created != 0) {
        CHECK(errno == ENOTSUP && runtime == NULL);
        puts("test_managed_catalog_internal: REQUIRED admission unavailable on this native profile");
        return 0;
    }
    spawn.cancel_token = NULL;
    CHECK(llam_runtime_spawn_ex(runtime, ordinary_task, &calls, &spawn, sizeof(spawn)) == NULL && errno == EINVAL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) == 0);
    CHECK(after.retained_references == 0U && after.excluded_tasks == 0U && calls == 0U);
    llam_runtime_destroy(runtime);
    return 0;
}

typedef struct cold_context_state {
    llam_runtime_t *runtime;
    bool passed;
} cold_context_state_t;

static void cold_context_task(void *argument) {
    cold_context_state_t *state = argument;
    llam_runtime_t *created = (llam_runtime_t *)(uintptr_t)1U;
    llam_module_t *module = (llam_module_t *)(uintptr_t)1U;
    unsigned char blob[128] = {0};
    llam_module_opts_t opts = {.metadata_address = (uintptr_t)blob, .metadata_bytes = sizeof(blob)};
    bool passed = llam_runtime_create_managed(NULL, 0U, NULL, 0U, &created) == -1 &&
                  errno == EBUSY && created == NULL;
    passed = passed && llam_runtime_module_register_ex(state->runtime, &opts, sizeof(opts), &module) == -1 &&
             errno == EBUSY && module == NULL;
    passed = passed && llam_runtime_module_unregister(state->runtime, (llam_module_t *)(uintptr_t)0x1234U) == -1 &&
             errno == EBUSY;
    state->passed = passed;
}

static llam_llpm2_t fixture(llam_llpm2_entry_t entries[5], llam_llpm2_range_t ranges[4]) {
    llam_llpm2_t record = {0};
    record.header.architecture = 2U;
    record.header.native_abi = LLAM_LLPM2_ABI_DARWIN;
    record.entries = entries;
    record.entry_count = 5U;
    record.ranges = ranges;
    record.range_count = 4U;
    for (size_t i = 0U; i < 4U; ++i) {
        entries[i] = (llam_llpm2_entry_t){
            .native_entry = 0x1000U + i * 0x100U,
            .native_end = 0x1020U + i * 0x100U,
            .managed_entry = 0x2000U + i * 0x100U,
            .code_begin = 0x2000U + i * 0x100U,
            .code_end = 0x2080U + i * 0x100U,
            .execution_class = LLAM_LLPM2_PORTABLE,
            .effects = 1U << LLAM_LLPM2_EFFECT_MANAGED,
            .required_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD,
            .type_id = llam_managed_task_type_id(record.header.architecture, record.header.native_abi),
        };
        ranges[i] = (llam_llpm2_range_t){
            .entry_index = (uint32_t)i, .kind = LLAM_LLPM2_SAFE,
            .begin = entries[i].code_begin, .end = entries[i].code_begin + 0x20U,
        };
    }
    entries[1].type_id ^= 1U;
    entries[2].flags = LLAM_LLPM2_ENTRY_INTERNAL;
    entries[3].execution_class = LLAM_LLPM2_NATIVE_AFFINE;
    entries[4] = (llam_llpm2_entry_t){
        .native_entry = (uintptr_t)ordinary_task,
        .managed_entry = 0U, /* LLPM2 legacy entries deliberately have no body. */
        .code_begin = (uintptr_t)ordinary_task,
        .code_end = (uintptr_t)ordinary_task + 1U,
        .execution_class = LLAM_LLPM2_LEGACY,
        .required_isa = LLAM_LLPM2_ISA_LSE,
        .type_id = llam_managed_task_type_id(record.header.architecture, record.header.native_abi),
    };
    return record;
}

static int check_catalog(llam_runtime_t *runtime, llam_runtime_t *peer) {
    llam_llpm2_entry_t entries[5];
    llam_llpm2_range_t ranges[4];
    llam_llpm2_t record = fixture(entries, ranges);
    llam_module_t *module = NULL;
    llam_module_t *duplicate = (llam_module_t *)(uintptr_t)1U;
    llam_managed_admission_t admission;
    llam_preempt_info_t info;
    CHECK(llam_managed_catalog_test_register(runtime, &record, &module) == 0 && module != NULL);
    CHECK(llam_managed_catalog_required_isa(runtime) == (LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD));
    CHECK(llam_managed_catalog_test_register(runtime, &record, &duplicate) == -1 && errno == EEXIST);
    CHECK(duplicate == NULL);
    CHECK(llam_runtime_module_unregister(peer, module) == -1 && errno == EXDEV);
    CHECK(llam_runtime_module_unregister(runtime, (llam_module_t *)(uintptr_t)0x1234U) == -1 && errno == EINVAL);
    CHECK(llam_managed_task_admit(runtime, (llam_task_fn)entries[0].native_entry, 0U, &admission) == 0);
    CHECK((uintptr_t)admission.entry == entries[0].managed_entry);
    CHECK(admission.execution_class == LLAM_MANAGED_EXECUTION_PORTABLE);
    CHECK(admission.catalog == runtime->managed_catalog);
    CHECK(llam_managed_catalog_find_safe(admission.catalog, ranges[0].begin) != NULL);
    CHECK(llam_managed_catalog_find_safe(admission.catalog, ranges[0].end - 1U) != NULL);
    CHECK(llam_managed_catalog_find_safe(admission.catalog, ranges[0].end) == NULL);
    CHECK(llam_managed_catalog_find_safe(admission.catalog, entries[0].native_entry) == NULL);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EBUSY);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.registered_modules == 1U && info.registered_entries == 5U && info.retained_references == 1U);
    _Alignas(llam_preempt_info_t) struct { uint32_t policy; uint32_t untouched; } prefix = {UINT32_MAX, UINT32_MAX};
    CHECK(llam_runtime_preempt_info_ex(runtime, (llam_preempt_info_t *)&prefix, sizeof(prefix.policy)) == 0);
    CHECK(prefix.policy == LLAM_PREEMPT_POLICY_COOPERATIVE && prefix.untouched == UINT32_MAX);
    _Alignas(llam_preempt_info_t) struct { unsigned char prefix[88]; uint64_t canary; } old_info;
    memset(&old_info, 0, sizeof(old_info));
    old_info.canary = UINT64_C(0xc5f7348ba90d123e);
    CHECK(llam_runtime_preempt_info_ex(runtime, (llam_preempt_info_t *)&old_info, sizeof(old_info.prefix)) == 0);
    CHECK(old_info.canary == UINT64_C(0xc5f7348ba90d123e));
    llam_managed_admission_release(&admission);
    CHECK(llam_managed_task_admit(runtime, (llam_task_fn)entries[0].native_entry,
                                   LLAM_SPAWN_F_NO_PREEMPT, &admission) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 && info.excluded_tasks == 1U);
    llam_managed_admission_release(&admission);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 && info.excluded_tasks == 0U);
    const uintptr_t denied[] = {entries[0].managed_entry, entries[0].managed_entry + 4U,
                               entries[0].native_entry + 4U, entries[1].native_entry,
                               entries[2].native_entry};
    for (size_t i = 0U; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        CHECK(llam_managed_task_admit(runtime, (llam_task_fn)denied[i], 0U, &admission) == -1 && errno == ENOTSUP);
        CHECK(admission.catalog == NULL && admission.entry == NULL);
    }
    /* Descriptor class and explicit promotion consume the same bounded pool.
     * No user entry is executed by this synthetic metadata admission fixture. */
    llam_managed_admission_t affine[4];
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.native_affine_capacity == 4U && info.native_affine_available == 4U);
    CHECK(info.portable_processors >= 1U && info.compatibility_processors == 1U);
    for (size_t i = 0U; i < 4U; ++i) {
        uintptr_t entry = i == 0U ? entries[3].native_entry : entries[0].native_entry;
        uint32_t flags = i == 0U ? 0U : LLAM_SPAWN_F_NATIVE_AFFINE;
        CHECK(llam_managed_task_admit(runtime, (llam_task_fn)entry, flags, &affine[i]) == 0);
        CHECK(affine[i].execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE && affine[i].execution_worker != NULL);
        for (size_t j = 0U; j < i; ++j) CHECK(affine[i].execution_worker != affine[j].execution_worker);
    }
    CHECK(llam_managed_task_admit(runtime, (llam_task_fn)entries[3].native_entry, 0U, &admission) == -1 && errno == EAGAIN);
    CHECK(admission.catalog == NULL && admission.execution_worker == NULL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.native_affine_tasks == 4U && info.native_affine_available == 0U);
    for (size_t i = 0U; i < 4U; ++i) llam_managed_admission_release(&affine[i]);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.native_affine_tasks == 0U && info.native_affine_available == 4U);
    llam_spawn_opts_t failing_affine = {.task_class = LLAM_TASK_CLASS_DEFAULT,
        .cancel_token = (llam_cancel_token_t *)(uintptr_t)0x1234U};
    CHECK(llam_runtime_spawn_ex(runtime, (llam_task_fn)entries[3].native_entry, NULL,
                                 &failing_affine, sizeof(failing_affine)) == NULL && errno == EINVAL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.native_affine_tasks == 0U && info.native_affine_available == 4U && info.retained_references == 0U);
    CHECK(llam_managed_task_admit(runtime, ordinary_task, LLAM_SPAWN_F_NATIVE_AFFINE, &admission) == -1 && errno == ENOTSUP);
    CHECK(llam_managed_task_admit(runtime, cold_context_task, LLAM_SPAWN_F_NATIVE_AFFINE, &admission) == -1 && errno == ENOTSUP);
    CHECK(llam_managed_task_admit(runtime, ordinary_task, 0U, &admission) == 0);
    CHECK(admission.execution_class == LLAM_MANAGED_EXECUTION_LEGACY && admission.execution_worker != NULL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 && info.legacy_tasks == 1U);
    llam_managed_admission_release(&admission);
    CHECK(llam_managed_catalog_run_begin(runtime) == 0);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EBUSY);
    CHECK(llam_managed_catalog_run_commit(runtime, false, 0U) == 0);
    CHECK(!llam_managed_catalog_async_enabled(runtime));
    llam_managed_catalog_run_end(runtime);
    entries[0].native_entry = 0x5000U;
    entries[0].native_end = 0x5020U;
    CHECK(llam_managed_catalog_test_register(runtime, &record, &duplicate) == -1 && errno == EINVAL);
    entries[0].native_entry = 0x1000U;
    entries[0].native_end = 0x1020U;
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 && info.registered_modules == 1U);
    unsigned calls = 0U;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, ordinary_task, &calls, NULL, 0U);
    CHECK(task != NULL);
    cold_context_state_t cold = {.runtime = runtime};
    llam_task_t *cold_task = llam_runtime_spawn_ex(runtime, cold_context_task, &cold, NULL, 0U);
    CHECK(cold_task != NULL);
    CHECK(llam_runtime_module_unregister(runtime, (llam_module_t *)task) == -1 && errno == EINVAL);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EBUSY);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(calls == 1U);
    CHECK(llam_join(task) == 0);
    CHECK(llam_join(cold_task) == 0 && cold.passed);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 && info.retained_references == 0U);
    CHECK(llam_runtime_module_unregister(runtime, module) == 0);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EINVAL);
    CHECK(llam_managed_catalog_test_register(runtime, &record, &duplicate) == 0);
    CHECK(duplicate != module);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EINVAL);
    CHECK(llam_runtime_module_unregister(runtime, duplicate) == 0);
    unsigned char heap_blob[128] = {0};
    llam_module_opts_t module_opts = {.metadata_address = (uint64_t)(uintptr_t)heap_blob,
                                      .metadata_bytes = sizeof(heap_blob)};
    CHECK(llam_runtime_module_register_ex(runtime, &module_opts, sizeof(module_opts), &module) == -1);
    CHECK(module == NULL);
    module_opts.flags = 1U;
    CHECK(llam_runtime_module_register_ex(runtime, &module_opts, sizeof(module_opts), &module) == -1 && errno == EINVAL);
    return 0;
}

/* Optional authentic compiler-produced DSO. Its long(long) entry intentionally
 * fails task signature admission while loader registration and pinning succeed. */
static int check_native_fixture(llam_runtime_t *runtime, llam_runtime_t *peer, const char *path) {
    typedef long (*fixture_entry_fn)(long);
#if LLAM_PLATFORM_WINDOWS
    HMODULE native = LoadLibraryA(path);
    CHECK(native != NULL);
    fixture_entry_fn entry = (fixture_entry_fn)(uintptr_t)GetProcAddress(native, "entry");
#else
    void *native = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (native == NULL) fprintf(stderr, "fixture dlopen: %s\n", dlerror());
    CHECK(native != NULL);
    fixture_entry_fn entry = (fixture_entry_fn)dlsym(native, "entry");
#endif
    CHECK(entry != NULL);
    llam_managed_image_t image;
    CHECK(llam_managed_image_open((const void *)(uintptr_t)entry, true, &image) == 0);
    llam_module_opts_t opts = {.metadata_address = image.metadata,
                               .metadata_bytes = image.metadata_size};
    llam_managed_image_close(&image);
    llam_module_t *module = NULL;
    CHECK(llam_runtime_module_register_ex(runtime, &opts, sizeof(opts), &module) == 0);
    CHECK(llam_runtime_module_unregister(peer, module) == -1 && errno == EXDEV);
#if LLAM_PLATFORM_WINDOWS
    CHECK(FreeLibrary(native) != 0);
#else
    CHECK(dlclose(native) == 0);
#endif
    /* Only LLAM's independent loader pin remains when this native entry runs. */
    CHECK(entry(8L) == 59L);
    llam_managed_admission_t admission;
    CHECK(llam_managed_task_admit(runtime, (llam_task_fn)(uintptr_t)entry, 0U, &admission) == -1 && errno == ENOTSUP);
    llam_runtime_opts_t runtime_opts;
    llam_preempt_opts_t preempt_opts;
    llam_runtime_t *discovered = NULL;
    llam_preempt_info_t info;
    CHECK(llam_runtime_opts_init(&runtime_opts, sizeof(runtime_opts)) == 0);
    CHECK(llam_preempt_opts_init(&preempt_opts, sizeof(preempt_opts)) == 0);
    runtime_opts.deterministic = 1U;
    preempt_opts.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&runtime_opts, sizeof(runtime_opts), &preempt_opts,
                                       sizeof(preempt_opts), &discovered) == 0);
    CHECK(llam_runtime_preempt_info_ex(discovered, &info, sizeof(info)) == 0 && info.registered_modules >= 1U);
    CHECK(llam_runtime_module_unregister(runtime, module) == 0);
    CHECK(llam_runtime_module_unregister(runtime, module) == -1 && errno == EINVAL);
    /* The discovered peer's own pin survives explicit owner's unregister. */
    CHECK(entry(8L) == 59L);
    llam_runtime_destroy(discovered);
    puts("test_managed_catalog_internal: native fixture registration, discovery, independent pins and typed entry checks passed");
    return 0;
}

int main(int argc, char **argv) {
    llam_runtime_opts_t runtime_opts;
    llam_preempt_opts_t preempt_opts;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *peer = NULL;
    if (check_options() != 0 || llam_runtime_opts_init(&runtime_opts, sizeof(runtime_opts)) != 0 ||
        llam_preempt_opts_init(&preempt_opts, sizeof(preempt_opts)) != 0) return 1;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    runtime_opts.deterministic = 1U;
    preempt_opts.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    /* Only the complete policy field is supplied; omitted flags/budget default. */
    if (llam_runtime_create_managed(&runtime_opts, sizeof(runtime_opts), &preempt_opts, sizeof(uint32_t), &runtime) != 0 ||
        llam_runtime_create_managed(&runtime_opts, sizeof(runtime_opts), &preempt_opts, sizeof(preempt_opts), &peer) != 0) {
        perror("managed create");
        llam_runtime_destroy(runtime);
        return 1;
    }
    int result = check_catalog(runtime, peer);
    if (result == 0) result = check_admission_rollback_and_flags(&runtime_opts);
    if (result == 0 && argc > 1) result = check_native_fixture(runtime, peer, argv[1]);
    llam_runtime_destroy(runtime);
    llam_runtime_destroy(peer);
    if (result != 0) return 1;
    puts("test_managed_catalog_internal: policy, admission, immutable index and module handle lifetime passed");
    return 0;
}
