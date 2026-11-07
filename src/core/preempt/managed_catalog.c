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
#include "core/task/local_executor_internal.h"
#include "managed_image.h"
#include "managed_async_adapter.h"
#include "capsule_pool.h"
#include "managed_scheduler.h"

#define LLAM_MANAGED_DEFAULT_BUDGET (UINT64_C(64) * 1024U * 1024U)
#define LLAM_MANAGED_DEFAULT_AFFINE_CAPACITY 4U
#define LLAM_MANAGED_MAX_AFFINE_CAPACITY 1024U
#define LLAM_MANAGED_MAX_MODULES 1024U
#define LLAM_MANAGED_MAX_ENTRIES 65536U
#define LLAM_MANAGED_MAX_RANGES 1048576U
#define LLAM_MANAGED_PREFIX_HAS(type, size, field) \
    ((size) >= offsetof(type, field) + sizeof(((type *)0)->field))

struct llam_module {
    llam_runtime_t *owner_runtime;
    struct llam_module *next;
    size_t slot;
    uint32_t generation;
    llam_managed_image_t image;
    llam_managed_image_records_t records;
};

typedef struct llam_managed_entry_ref {
    const llam_llpm2_entry_t *entry;
    uint64_t task_type_id;
} llam_managed_entry_ref_t;

typedef struct llam_managed_extent {
    uintptr_t begin;
    uintptr_t end;
} llam_managed_extent_t;

typedef struct llam_managed_index {
    uint64_t required_isa;
    size_t entry_count;
    size_t extent_count;
    size_t safe_count;
    llam_managed_entry_ref_t *entries;
    llam_managed_extent_t *extents;
    const llam_llpm2_range_t **safe_ranges;
} llam_managed_index_t;

struct llam_managed_catalog {
    llam_runtime_t *runtime;
    llam_preempt_opts_t opts;
    atomic_uint effective_policy;
    uint32_t reason;
    bool running;
    atomic_bool committed;
    _Atomic size_t legacy_references;
    _Atomic size_t excluded_references;
    _Atomic size_t affine_references;
    size_t module_count;
    _Atomic size_t references;
    llam_module_t *modules;
    llam_managed_index_t index;
};

/* Mutation and handle resolution share one cold lock. Native loader calls never
 * occur under this lock. Every image pin belongs to its runtime binding. */
static pthread_mutex_t g_managed_catalog_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_public_slot_table_t g_managed_module_slots;

static int llam_managed_catalog_operation_lock(void) {
    /* A managed M must not block behind another runtime's cold index rebuild.
     * Host control operations may wait; managed callers retry after EAGAIN. */
    int error = g_llam_tls_task != NULL ? pthread_mutex_trylock(&g_managed_catalog_lock)
                                       : pthread_mutex_lock(&g_managed_catalog_lock);
    if (error == 0) return 0;
    errno = error == EBUSY ? EAGAIN : error;
    return -1;
}

static void llam_managed_index_destroy(llam_managed_index_t *index) {
    free(index->entries);
    free(index->extents);
    free(index->safe_ranges);
    memset(index, 0, sizeof(*index));
}

uint64_t llam_managed_task_type_id(uint16_t architecture, uint16_t native_abi) {
    /* Matches managed_llvm_contract.h's C ABI, void (ptr), no ABI attributes. */
    char text[] = "void (ptr):0:0:0";
    uint64_t hash = UINT64_C(14695981039346656037);
    if (architecture < 1U || architecture > 2U || native_abi < 1U || native_abi > 3U) return 0U;
    text[13] = (char)('0' + architecture);
    text[15] = (char)('0' + native_abi);
    for (size_t i = 0U; i < sizeof(text) - 1U; ++i) {
        hash = (hash ^ (unsigned char)text[i]) * UINT64_C(1099511628211);
    }
    return hash != 0U ? hash : 1U;
}

static int llam_managed_entry_compare(const void *left, const void *right) {
    const llam_managed_entry_ref_t *a = left;
    const llam_managed_entry_ref_t *b = right;
    return (a->entry->native_entry > b->entry->native_entry) -
           (a->entry->native_entry < b->entry->native_entry);
}

static int llam_managed_extent_compare(const void *left, const void *right) {
    const llam_managed_extent_t *a = left;
    const llam_managed_extent_t *b = right;
    return (a->begin > b->begin) - (a->begin < b->begin);
}

static int llam_managed_range_compare(const void *left, const void *right) {
    const llam_llpm2_range_t *a = *(const llam_llpm2_range_t *const *)left;
    const llam_llpm2_range_t *b = *(const llam_llpm2_range_t *const *)right;
    return (a->begin > b->begin) - (a->begin < b->begin);
}

static int llam_managed_index_build(llam_module_t *modules, const llam_module_t *omit,
                                     llam_managed_index_t *out) {
    size_t entries = 0U;
    size_t ranges = 0U;
    memset(out, 0, sizeof(*out));
    for (llam_module_t *module = modules; module != NULL; module = module->next) {
        if (module == omit) continue;
        for (size_t i = 0U; i < module->records.count; ++i) {
            const llam_llpm2_t *record = &module->records.records[i];
            if (record->entry_count > LLAM_MANAGED_MAX_ENTRIES - entries ||
                record->range_count > LLAM_MANAGED_MAX_RANGES - ranges) {
                errno = EOVERFLOW;
                return -1;
            }
            entries += record->entry_count;
            ranges += record->range_count;
        }
    }
    if (entries == 0U) return 0;
    out->entries = calloc(entries, sizeof(*out->entries));
    out->extents = calloc(entries * 2U, sizeof(*out->extents));
    if (ranges != 0U) out->safe_ranges = calloc(ranges, sizeof(*out->safe_ranges));
    if (out->entries == NULL || out->extents == NULL ||
        (ranges != 0U && out->safe_ranges == NULL)) {
        llam_managed_index_destroy(out);
        errno = ENOMEM;
        return -1;
    }
    for (llam_module_t *module = modules; module != NULL; module = module->next) {
        if (module == omit) continue;
        for (size_t i = 0U; i < module->records.count; ++i) {
            const llam_llpm2_t *record = &module->records.records[i];
            for (size_t j = 0U; j < record->entry_count; ++j) {
                const llam_llpm2_entry_t *entry = &record->entries[j];
                out->entries[out->entry_count++] = (llam_managed_entry_ref_t){
                    entry, llam_managed_task_type_id(record->header.architecture, record->header.native_abi)};
                /* Compatibility code never runs on an async-capable M. Its
                 * unrelated ISA requirements cannot downgrade managed peers. */
                if (entry->execution_class != LLAM_LLPM2_LEGACY) out->required_isa |= entry->required_isa;
                out->extents[out->extent_count++] = (llam_managed_extent_t){entry->code_begin, entry->code_end};
                if (entry->native_end != 0U) {
                    out->extents[out->extent_count++] = (llam_managed_extent_t){entry->native_entry, entry->native_end};
                }
            }
            for (size_t j = 0U; j < record->range_count; ++j) {
                if (record->ranges[j].kind == LLAM_LLPM2_SAFE) {
                    out->safe_ranges[out->safe_count++] = &record->ranges[j];
                }
            }
        }
    }
    qsort(out->entries, out->entry_count, sizeof(*out->entries), llam_managed_entry_compare);
    qsort(out->extents, out->extent_count, sizeof(*out->extents), llam_managed_extent_compare);
    if (out->safe_count != 0U) {
        qsort(out->safe_ranges, out->safe_count, sizeof(*out->safe_ranges), llam_managed_range_compare);
    }
    for (size_t i = 1U; i < out->entry_count; ++i) {
        if (out->entries[i - 1U].entry->native_entry == out->entries[i].entry->native_entry) goto overlap;
    }
    for (size_t i = 1U; i < out->extent_count; ++i) {
        if (out->extents[i - 1U].end > out->extents[i].begin) goto overlap;
    }
    for (size_t i = 1U; i < out->safe_count; ++i) {
        if (out->safe_ranges[i - 1U]->end > out->safe_ranges[i]->begin) goto overlap;
    }
    return 0;
overlap:
    llam_managed_index_destroy(out);
    errno = EINVAL;
    return -1;
}

static void llam_managed_catalog_invalidate_profile(llam_managed_catalog_t *catalog) {
    catalog->committed = false;
    catalog->effective_policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    if (catalog->runtime->preempt_mode < LLAM_PREEMPT_AUTO) {
        catalog->reason = LLAM_PREEMPT_REASON_REQUEST_DISABLED;
    } else if (catalog->opts.policy == LLAM_PREEMPT_POLICY_COOPERATIVE) {
        catalog->reason = LLAM_PREEMPT_REASON_COOPERATIVE_POLICY;
    } else {
        const llam_managed_scheduler_ops_t *ops = catalog->runtime->managed_scheduler_ops;
        catalog->reason = ops != NULL && ops->native_async ? LLAM_PREEMPT_REASON_PROBE_PENDING
                                                           : LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE;
    }
}

static bool llam_managed_catalog_busy(const llam_managed_catalog_t *catalog) {
    return catalog->running ||
        atomic_load_explicit(&catalog->runtime->exec_started, memory_order_acquire) ||
        atomic_load_explicit(&catalog->references, memory_order_acquire) != 0U;
}

static void llam_managed_module_destroy(llam_module_t *module) {
    if (module == NULL) return;
    llam_managed_image_records_destroy(&module->records);
    llam_managed_image_close(&module->image);
    free(module);
}

static int llam_managed_module_prepare(const void *address, uint64_t bytes,
                                        bool caller_reference, llam_module_t **out) {
    llam_module_t *module = calloc(1U, sizeof(*module));
    uintptr_t guards[2];
    *out = NULL;
    if (module == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (llam_managed_image_open(address, caller_reference, &module->image) != 0) goto fail;
    if (caller_reference && ((uintptr_t)address != module->image.metadata ||
                             bytes != module->image.metadata_size)) {
        errno = EINVAL;
        goto fail;
    }
    llam_native_domain_guard_identities(guards);
    if (llam_managed_image_validate(&module->image, guards, &module->records) != 0) goto fail;
    *out = module;
    return 0;
fail: {
    int saved = errno;
    llam_managed_module_destroy(module);
    errno = saved;
    return -1;
}
}

/* Owns module only on success. Rebuild then publish so any failure leaves the
 * old index and all existing public handles untouched. */
static int llam_managed_module_publish(llam_managed_catalog_t *catalog,
                                        llam_module_t *module, llam_module_t **out) {
    llam_managed_index_t next;
    if (llam_managed_catalog_busy(catalog) ||
        atomic_load_explicit(&catalog->runtime->stop_requested, memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    if (catalog->module_count >= LLAM_MANAGED_MAX_MODULES) {
        errno = EOVERFLOW;
        return -1;
    }
    for (llam_module_t *item = catalog->modules; item != NULL; item = item->next) {
        if (item->image.base == module->image.base) {
            errno = EEXIST;
            return -1;
        }
    }
    module->next = catalog->modules;
    if (llam_managed_index_build(module, NULL, &next) != 0) return -1;
    module->owner_runtime = catalog->runtime;
    if (llam_public_slot_reserve_family_secret(&g_managed_module_slots, module, 32U,
            LLAM_PUBLIC_HANDLE_FAMILY_MODULE, catalog->runtime->public_handle_secret,
            &module->slot, &module->generation) != 0) {
        llam_managed_index_destroy(&next);
        return -1;
    }
    llam_managed_index_destroy(&catalog->index);
    catalog->index = next;
    catalog->modules = module;
    catalog->module_count += 1U;
    llam_managed_catalog_invalidate_profile(catalog);
    if (out != NULL) {
        *out = (llam_module_t *)llam_public_slot_encode_handle(module->slot, module->generation,
                                                              LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
    }
    return 0;
}

static int llam_managed_discover_image(const void *address, void *opaque) {
    llam_managed_catalog_t *catalog = opaque;
    llam_module_t *module = NULL;
    if (llam_managed_module_prepare(address, 0U, false, &module) != 0) {
        /* Images without metadata do not participate; malformed metadata does. */
        return errno == ENOENT ? 0 : -1;
    }
    pthread_mutex_lock(&g_managed_catalog_lock);
    int result = llam_managed_module_publish(catalog, module, NULL);
    int saved = errno;
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (result != 0) llam_managed_module_destroy(module);
    if (result != 0 && saved == EEXIST) return 0;
    errno = saved;
    return result;
}

static int llam_managed_options(const llam_preempt_opts_t *input, size_t size,
                                  llam_preempt_opts_t *out) {
    memset(out, 0, sizeof(*out));
    out->policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    out->capsule_budget_bytes = LLAM_MANAGED_DEFAULT_BUDGET;
    out->native_affine_capacity = LLAM_MANAGED_DEFAULT_AFFINE_CAPACITY;
    if (input == NULL) {
        if (size == 0U) return 0;
        errno = EINVAL;
        return -1;
    }
    if (size == 0U) {
        errno = EINVAL;
        return -1;
    }
    llam_preempt_opts_t raw = {0};
    memcpy(&raw, input, size < sizeof(raw) ? size : sizeof(raw));
#define COPY_PREEMPT_FIELD(field) \
    if (LLAM_MANAGED_PREFIX_HAS(llam_preempt_opts_t, size, field)) out->field = raw.field
    COPY_PREEMPT_FIELD(policy);
    COPY_PREEMPT_FIELD(flags);
    COPY_PREEMPT_FIELD(capsule_budget_bytes);
    COPY_PREEMPT_FIELD(native_affine_capacity);
    COPY_PREEMPT_FIELD(reserved0);
#undef COPY_PREEMPT_FIELD
    if (out->policy > LLAM_PREEMPT_POLICY_COOPERATIVE || out->flags != 0U || out->reserved0 != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (out->native_affine_capacity == 0U) out->native_affine_capacity = LLAM_MANAGED_DEFAULT_AFFINE_CAPACITY;
    if (out->native_affine_capacity > LLAM_MANAGED_MAX_AFFINE_CAPACITY) {
        errno = EINVAL;
        return -1;
    }
    if (out->capsule_budget_bytes == 0U) out->capsule_budget_bytes = LLAM_MANAGED_DEFAULT_BUDGET;
    return 0;
}

int llam_preempt_opts_init(llam_preempt_opts_t *out, size_t out_size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_preempt_opts_t opts;
    if (out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    (void)llam_managed_options(NULL, 0U, &opts);
    memcpy(out, &opts, out_size < sizeof(opts) ? out_size : sizeof(opts));
    return 0;
}

/* The constructor owns registered but unpublished storage. Loader operations
 * run outside the lifecycle gate; failure cleanup owns the attached catalog. */
int llam_managed_catalog_prepare(llam_runtime_t *runtime, const llam_preempt_opts_t *opts) {
    llam_managed_catalog_t *catalog = calloc(1U, sizeof(*catalog));
    if (catalog == NULL) return -1;
    catalog->runtime = runtime;
    catalog->opts = *opts;
    atomic_init(&catalog->effective_policy, LLAM_PREEMPT_POLICY_COOPERATIVE);
    atomic_init(&catalog->committed, false);
    catalog->reason = opts->policy == LLAM_PREEMPT_POLICY_COOPERATIVE
                          ? LLAM_PREEMPT_REASON_COOPERATIVE_POLICY
                          : LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE;
    if (runtime->preempt_mode == LLAM_PREEMPT_OFF || runtime->preempt_mode == LLAM_PREEMPT_COOPERATIVE) {
        catalog->reason = LLAM_PREEMPT_REASON_REQUEST_DISABLED;
    }
    atomic_init(&catalog->references, 0U);
    atomic_init(&catalog->legacy_references, 0U);
    atomic_init(&catalog->excluded_references, 0U);
    atomic_init(&catalog->affine_references, 0U);
    pthread_mutex_lock(&g_managed_catalog_lock);
    runtime->managed_catalog = catalog;
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (opts->policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED &&
        (runtime->preempt_mode == LLAM_PREEMPT_OFF || runtime->preempt_mode == LLAM_PREEMPT_COOPERATIVE)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_managed_image_discover(llam_managed_discover_image, catalog) != 0) return -1;
    int prepared = llam_managed_async_prepare(runtime);
    if (prepared != 0 && (errno != ENOTSUP || opts->policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED)) return -1;
    llam_managed_catalog_invalidate_profile(catalog);
    return 0;
}

int llam_runtime_create_managed(const llam_runtime_opts_t *runtime_opts, size_t runtime_opts_size,
                                 const llam_preempt_opts_t *preempt_opts, size_t preempt_opts_size,
                                 llam_runtime_t **out) {
    if (LLAM_UNLIKELY(llam_native_domain_blocked())) {
        if (out != NULL) *out = NULL;
        errno = ENOTSUP;
        return -1;
    }
    if (out == NULL) { errno = EINVAL; return -1; }
    *out = NULL;
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        errno = EBUSY;
        return -1;
    }
    llam_preempt_opts_t opts;
    if (llam_managed_options(preempt_opts, preempt_opts_size, &opts) != 0) return -1;
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    if (runtime == NULL) return -1;
    if (llam_runtime_init_managed_rt(runtime, runtime_opts, runtime_opts_size, true, &opts) != 0) {
        int error = errno;
        free(runtime);
        errno = error;
        return -1;
    }
    *out = runtime;
    return 0;
}

int llam_runtime_preempt_info_ex(llam_runtime_t *runtime, llam_preempt_info_t *out, size_t out_size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_runtime_t *pinned = NULL;
    llam_preempt_info_t info = {0};
    if (runtime == NULL || out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    if (llam_managed_catalog_operation_lock() != 0) {
        llam_runtime_end_public_op(pinned);
        return -1;
    }
    const llam_managed_catalog_t *catalog = pinned->managed_catalog;
    info.effective_policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    info.requested_policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    info.reason = LLAM_PREEMPT_REASON_LEGACY_RUNTIME;
    if (catalog != NULL) {
        info.requested_policy = catalog->opts.policy;
        info.effective_policy = catalog->effective_policy;
        info.reason = catalog->reason;
        info.flags = (catalog->running ? LLAM_PREEMPT_INFO_F_RUNNING : 0U) |
                     (catalog->committed ? LLAM_PREEMPT_INFO_F_COMMITTED : 0U);
        info.capsule_budget_bytes = catalog->opts.capsule_budget_bytes;
        info.native_affine_capacity = catalog->opts.native_affine_capacity;
        info.registered_modules = catalog->module_count;
        info.registered_entries = catalog->index.entry_count;
        info.retained_references = atomic_load_explicit(&catalog->references, memory_order_acquire);
        info.excluded_tasks = atomic_load_explicit(&catalog->excluded_references, memory_order_acquire);
        info.legacy_tasks = atomic_load_explicit(&catalog->legacy_references, memory_order_acquire);
        info.native_affine_tasks = atomic_load_explicit(&catalog->affine_references, memory_order_acquire);
        info.native_affine_available = llam_managed_affine_available(pinned);
        info.portable_processors = pinned->portable_processor_count;
        info.compatibility_processors = pinned->compatibility_processor_count;
        if (catalog->opts.policy == LLAM_PREEMPT_POLICY_ASYNC_AUTO &&
            info.effective_policy != LLAM_PREEMPT_POLICY_COOPERATIVE) {
            info.effective_policy = info.legacy_tasks != 0U ? LLAM_PREEMPT_EFFECTIVE_MIXED
                                                            : LLAM_PREEMPT_POLICY_ASYNC_AUTO;
            info.reason = info.legacy_tasks != 0U ? LLAM_PREEMPT_REASON_LEGACY_MIXED
                                                  : LLAM_PREEMPT_REASON_NONE;
        }
    }
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (llam_managed_async_memory_info(pinned, &info.capsule_charge_bytes,
                                       &info.conservative_task_charge_bytes) &&
        info.conservative_task_charge_bytes != 0U) {
        info.maximum_charged_tasks = info.capsule_budget_bytes / info.conservative_task_charge_bytes;
    }
    if (atomic_load_explicit(&pinned->async_preempt_enabled, memory_order_acquire) != 0U &&
        pinned->managed_capsule_pool != NULL) {
        const llam_capsule_pool_t *pool = pinned->managed_capsule_pool;
        info.prepared_capsules = atomic_load_explicit(&pool->prepared, memory_order_acquire);
        info.available_capsules = atomic_load_explicit(&pool->available, memory_order_acquire);
        if (info.available_capsules > info.prepared_capsules)
            info.available_capsules = info.prepared_capsules;
        info.reserved_capsule_bytes = info.prepared_capsules * pool->charge;
    }
    if ((info.flags & LLAM_PREEMPT_INFO_F_COMMITTED) != 0U &&
        info.effective_policy != LLAM_PREEMPT_POLICY_COOPERATIVE) {
        for (unsigned i = 0U; i < 3U; ++i) {
            info.effective_quantum_ns[i] = llam_managed_async_effective_quantum(pinned, (llam_task_class_t)i);
        }
    }
    memcpy(out, &info, out_size < sizeof(info) ? out_size : sizeof(info));
    llam_runtime_end_public_op(pinned);
    return 0;
}

int llam_runtime_module_register_ex(llam_runtime_t *runtime, const llam_module_opts_t *opts,
                                     size_t opts_size, llam_module_t **out) {
    if (LLAM_UNLIKELY(llam_native_domain_blocked())) {
        if (out != NULL) *out = NULL;
        errno = ENOTSUP;
        return -1;
    }
    if (out != NULL) *out = NULL;
    llam_runtime_t *pinned = NULL;
    llam_module_t *module = NULL;
    llam_module_opts_t normalized = {0};
    if (runtime == NULL || opts == NULL || out == NULL ||
        !LLAM_MANAGED_PREFIX_HAS(llam_module_opts_t, opts_size, metadata_bytes)) {
        errno = EINVAL;
        return -1;
    }
    llam_module_opts_t raw = {0};
    memcpy(&raw, opts, opts_size < sizeof(raw) ? opts_size : sizeof(raw));
    normalized.metadata_address = raw.metadata_address;
    normalized.metadata_bytes = raw.metadata_bytes;
    if (LLAM_MANAGED_PREFIX_HAS(llam_module_opts_t, opts_size, flags)) normalized.flags = raw.flags;
    if (LLAM_MANAGED_PREFIX_HAS(llam_module_opts_t, opts_size, reserved0)) normalized.reserved0 = raw.reserved0;
    if (normalized.metadata_address == 0U || normalized.metadata_address > UINTPTR_MAX ||
        normalized.metadata_bytes == 0U || normalized.metadata_bytes > SIZE_MAX ||
        normalized.flags != 0U || normalized.reserved0 != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_task != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    pthread_mutex_lock(&g_managed_catalog_lock);
    llam_managed_catalog_t *catalog = pinned->managed_catalog;
    int result = 0;
    if (catalog == NULL || llam_managed_catalog_busy(catalog)) {
        errno = catalog == NULL ? ENOTSUP : EBUSY;
        result = -1;
    }
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (result == 0) {
        result = llam_managed_module_prepare((const void *)(uintptr_t)normalized.metadata_address,
                                              normalized.metadata_bytes, true, &module);
    }
    if (result == 0) {
        pthread_mutex_lock(&g_managed_catalog_lock);
        result = llam_managed_module_publish(catalog, module, out);
        pthread_mutex_unlock(&g_managed_catalog_lock);
    }
    int saved = errno;
    if (result != 0) llam_managed_module_destroy(module);
    llam_runtime_end_public_op(pinned);
    errno = saved;
    return result;
}

int llam_runtime_module_unregister(llam_runtime_t *runtime, llam_module_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_runtime_t *pinned = NULL;
    llam_module_t *module = NULL;
    llam_managed_index_t next;
    int result = -1;
    if (runtime == NULL || handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_task != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    pthread_mutex_lock(&g_managed_catalog_lock);
    module = llam_public_slot_resolve_encoded(&g_managed_module_slots, (uintptr_t)handle,
                                               LLAM_SYNC_PUBLIC_HANDLE_SHIFT, NULL, NULL);
    if (module == NULL) {
        errno = EINVAL;
        goto unlock;
    }
    if (module->owner_runtime != pinned) {
        errno = EXDEV;
        goto unlock;
    }
    llam_managed_catalog_t *catalog = pinned->managed_catalog;
    if (catalog == NULL || llam_managed_catalog_busy(catalog)) {
        errno = EBUSY;
        goto unlock;
    }
    if (llam_managed_index_build(catalog->modules, module, &next) != 0) goto unlock;
    llam_module_t **link = &catalog->modules;
    while (*link != module) link = &(*link)->next;
    *link = module->next;
    catalog->module_count -= 1U;
    llam_public_slot_release(&g_managed_module_slots, module->slot, module, module->generation);
    llam_managed_index_destroy(&catalog->index);
    catalog->index = next;
    llam_managed_catalog_invalidate_profile(catalog);
    result = 0;
unlock: {
    int saved = errno;
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (result == 0) llam_managed_module_destroy(module);
    llam_runtime_end_public_op(pinned);
    errno = saved;
    return result;
}
}

bool llam_managed_catalog_retain(llam_managed_catalog_t *catalog) {
    if (catalog == NULL) return true;
    size_t count = atomic_load_explicit(&catalog->references, memory_order_relaxed);
    do {
        if (count >= SIZE_MAX / 2U) return false;
    } while (!atomic_compare_exchange_weak_explicit(&catalog->references, &count, count + 1U,
                                                    memory_order_relaxed, memory_order_relaxed));
    return true;
}

void llam_managed_catalog_release(llam_managed_catalog_t *catalog) {
    if (catalog == NULL) return;
    /* Publication is protected by the mutation lock or an existing reference.
     * Release makes final native/G accesses precede the quiescent acquire. */
    if (atomic_fetch_sub_explicit(&catalog->references, 1U, memory_order_release) == 0U) abort();
}

void llam_managed_admission_release(llam_managed_admission_t *admission) {
    if (admission == NULL || admission->catalog == NULL) return;
    if (admission->execution_class == LLAM_MANAGED_EXECUTION_LEGACY &&
        atomic_fetch_sub_explicit(&admission->catalog->legacy_references, 1U, memory_order_release) == 0U) {
        abort();
    }
    if ((admission->spawn_flags & LLAM_SPAWN_F_NO_PREEMPT) != 0U &&
        atomic_fetch_sub_explicit(&admission->catalog->excluded_references, 1U, memory_order_release) == 0U) {
        abort();
    }
    if (admission->execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        atomic_fetch_sub_explicit(&admission->catalog->affine_references, 1U, memory_order_release) == 0U) {
        abort();
    }
    if (admission->local_executor != NULL)
        llam_local_executor_admission_release(admission->local_executor, admission->local_executor_retired);
    else llam_managed_release_execution(admission->catalog->runtime, admission->execution_worker,
                                         admission->execution_class, admission->execution_generation);
    llam_managed_catalog_release(admission->catalog);
    *admission = (llam_managed_admission_t){0};
}

bool llam_managed_catalog_has_legacy_references(const llam_runtime_t *runtime) {
    const llam_managed_catalog_t *catalog = runtime != NULL ? runtime->managed_catalog : NULL;
    /* Unknown catalogs keep the ordinary claim path; zero is only an idle hint. */
    return catalog == NULL ||
        atomic_load_explicit(&catalog->legacy_references, memory_order_acquire) != 0U;
}

void llam_managed_task_release(llam_task_t *task) {
    if (task == NULL || task->managed_catalog == NULL) return;
    llam_managed_admission_t admission = {
        .catalog = task->managed_catalog, .entry = task->entry,
        .execution_class = task->managed_execution_class, .spawn_flags = task->flags,
        .execution_worker = task->managed_execution_worker,
        .execution_generation = task->managed_execution_generation,
        .local_executor = task->local_executor, .local_executor_retired = task->local_executor_retired
    };
    llam_managed_admission_release(&admission);
    task->managed_catalog = NULL;
    task->managed_execution_worker = NULL;
    task->managed_execution_generation = 0U;
    task->local_executor = NULL;
}

static bool llam_managed_index_contains(const llam_managed_index_t *index, uintptr_t pc) {
    size_t low = 0U, high = index->extent_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const llam_managed_extent_t *extent = &index->extents[middle];
        if (pc < extent->begin) high = middle;
        else if (pc >= extent->end) low = middle + 1U;
        else return true;
    }
    return false;
}

static const llam_managed_entry_ref_t *llam_managed_index_find_entry(
    const llam_managed_index_t *index, uintptr_t address) {
    size_t low = 0U, high = index->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const llam_managed_entry_ref_t *candidate = &index->entries[middle];
        if (address < candidate->entry->native_entry) high = middle;
        else if (address > candidate->entry->native_entry) low = middle + 1U;
        else return candidate;
    }
    return NULL;
}

static bool llam_managed_task_descriptor_valid(const llam_managed_entry_ref_t *found, bool required) {
    const llam_llpm2_entry_t *descriptor = found->entry;
    return found->task_type_id != 0U && descriptor->type_id == found->task_type_id &&
        descriptor->execution_class <= LLAM_LLPM2_NATIVE_AFFINE &&
        (descriptor->flags & (LLAM_LLPM2_ENTRY_INTERNAL | LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY |
                              LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE)) == 0U &&
        (!required || (descriptor->execution_class != LLAM_LLPM2_LEGACY &&
                       (descriptor->flags & LLAM_LLPM2_ENTRY_NO_PREEMPT) == 0U));
}

/* The admission transaction holds the catalog lock across classification and
 * resource/reference publication; helpers never drop or reacquire it. */
static int llam_managed_classify_task(llam_managed_catalog_t *catalog,
    llam_task_fn entry, bool local, llam_managed_admission_t *out) {
    bool required = catalog->opts.policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    if (required && (out->spawn_flags & LLAM_SPAWN_F_NO_PREEMPT) != 0U) { errno = EINVAL; return -1; }
    uintptr_t address = (uintptr_t)entry;
    const llam_managed_entry_ref_t *found = llam_managed_index_find_entry(&catalog->index, address);
    if (found != NULL) {
        if (!llam_managed_task_descriptor_valid(found, required)) { errno = ENOTSUP; return -1; }
        const llam_llpm2_entry_t *descriptor = found->entry;
        out->entry = descriptor->execution_class == LLAM_LLPM2_LEGACY
                         ? entry : (llam_task_fn)descriptor->managed_entry;
        out->execution_class = descriptor->execution_class;
        if ((descriptor->flags & LLAM_LLPM2_ENTRY_NO_PREEMPT) != 0U)
            out->spawn_flags |= LLAM_SPAWN_F_NO_PREEMPT;
    } else if (llam_managed_index_contains(&catalog->index, address) || required) {
        /* A body/interior/thunk continuation cannot borrow legacy admission. */
        errno = ENOTSUP;
        return -1;
    }
    if (local || (out->spawn_flags & LLAM_SPAWN_F_NATIVE_AFFINE) != 0U) {
        if (out->execution_class == LLAM_MANAGED_EXECUTION_LEGACY &&
            (!local || catalog->opts.policy != LLAM_PREEMPT_POLICY_COOPERATIVE)) {
            errno = ENOTSUP;
            return -1;
        }
        out->execution_class = LLAM_MANAGED_EXECUTION_NATIVE_AFFINE;
    }
    return 0;
}

static int llam_managed_retain_admission(llam_managed_catalog_t *catalog,
    const llam_local_executor_binding_t *local, llam_managed_admission_t *out) {
    if (local != NULL) {
        out->execution_worker = local->worker;
        out->execution_generation = local->generation;
    } else if (llam_managed_reserve_execution(catalog->runtime, out->execution_class,
            &out->execution_worker, &out->execution_generation) != 0) return -1;
    if (!llam_managed_catalog_retain(catalog)) {
        if (local == NULL) llam_managed_release_execution(catalog->runtime, out->execution_worker,
                                                          out->execution_class, out->execution_generation);
        out->execution_worker = NULL;
        errno = EOVERFLOW;
        return -1;
    }
    if (out->execution_class == LLAM_MANAGED_EXECUTION_LEGACY) {
        /* Publish admission before queueing so an idle compatibility M can
         * resume claims. Queue locks and fresh P grants still govern dispatch. */
        atomic_fetch_add_explicit(&catalog->legacy_references, 1U, memory_order_release);
    }
    if (out->execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE)
        atomic_fetch_add_explicit(&catalog->affine_references, 1U, memory_order_relaxed);
    if ((out->spawn_flags & LLAM_SPAWN_F_NO_PREEMPT) != 0U)
        atomic_fetch_add_explicit(&catalog->excluded_references, 1U, memory_order_relaxed);
    out->catalog = catalog;
    out->local_executor = local != NULL ? local->domain : NULL;
    return 0;
}

static int llam_managed_task_admit_impl(llam_runtime_t *runtime, llam_task_fn entry, uint32_t spawn_flags,
    const llam_local_executor_binding_t *local, llam_managed_admission_t *out) {
    if (runtime == NULL || entry == NULL || out == NULL) { errno = EINVAL; return -1; }
    *out = (llam_managed_admission_t){.entry = entry,
        .execution_class = LLAM_MANAGED_EXECUTION_LEGACY, .spawn_flags = spawn_flags};
    if (runtime->managed_catalog == NULL) {
        if ((spawn_flags & LLAM_SPAWN_F_NATIVE_AFFINE) == 0U) return 0;
        *out = (llam_managed_admission_t){0};
        errno = ENOTSUP;
        return -1;
    }
    if (llam_managed_catalog_operation_lock() != 0) {
        *out = (llam_managed_admission_t){0};
        return -1;
    }
    llam_managed_catalog_t *catalog = runtime->managed_catalog;
    int result = -1;
    if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire)) errno = EBUSY;
    else if (llam_managed_classify_task(catalog, entry, local != NULL, out) == 0)
        result = llam_managed_retain_admission(catalog, local, out);
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (result != 0) *out = (llam_managed_admission_t){0};
    return result;
}

int llam_managed_task_admit(llam_runtime_t *runtime, llam_task_fn entry, uint32_t flags,
                             llam_managed_admission_t *out) {
    return llam_managed_task_admit_impl(runtime, entry, flags, NULL, out);
}

int llam_managed_task_admit_local(llam_runtime_t *runtime, llam_task_fn entry,
    uint32_t flags, llam_local_executor_t *handle, llam_managed_admission_t *out) {
    if (handle == NULL) return llam_managed_task_admit(runtime, entry, flags, out);
    llam_local_executor_binding_t binding = {0};
    if (llam_local_executor_admit(runtime, handle, &binding.domain,
                                  &binding.worker, &binding.generation) != 0) return -1;
    int result = llam_managed_task_admit_impl(runtime, entry, flags, &binding, out);
    if (result != 0) {
        int error = errno;
        llam_local_executor_admission_release(binding.domain, false);
        errno = error;
    }
    return result;
}

const llam_llpm2_range_t *llam_managed_catalog_find_safe(const llam_managed_catalog_t *catalog,
                                                        uintptr_t pc) {
    if (catalog == NULL) return NULL;
    size_t low = 0U, high = catalog->index.safe_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const llam_llpm2_range_t *range = catalog->index.safe_ranges[middle];
        if (pc < range->begin) high = middle;
        else if (pc >= range->end) low = middle + 1U;
        else return range;
    }
    return NULL;
}

int llam_managed_catalog_run_begin(llam_runtime_t *runtime) {
    if (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_LEGACY) return 0;
    pthread_mutex_lock(&g_managed_catalog_lock);
    llam_managed_catalog_t *catalog = runtime->managed_catalog;
    int result = 0;
    if (catalog == NULL) {
        errno = ENOTSUP;
        result = -1;
    } else if (catalog->running) {
        errno = EBUSY;
        result = -1;
    } else {
        llam_managed_catalog_invalidate_profile(catalog);
        catalog->running = true;
    }
    pthread_mutex_unlock(&g_managed_catalog_lock);
    return result;
}

uint32_t llam_managed_catalog_policy(const llam_runtime_t *runtime) {
    return runtime->managed_catalog != NULL ? runtime->managed_catalog->opts.policy
                                             : LLAM_PREEMPT_POLICY_COOPERATIVE;
}

uint64_t llam_managed_catalog_budget(const llam_runtime_t *runtime) {
    return runtime->managed_catalog != NULL ? runtime->managed_catalog->opts.capsule_budget_bytes : 0U;
}

uint64_t llam_managed_catalog_required_isa(const llam_runtime_t *runtime) {
    return runtime->managed_catalog != NULL ? runtime->managed_catalog->index.required_isa : 0U;
}

bool llam_managed_catalog_async_enabled(const llam_runtime_t *runtime) {
    /* Only read after the startup barrier or by its committing driver. */
    return runtime->managed_catalog != NULL && runtime->managed_catalog->committed &&
           runtime->managed_catalog->effective_policy != LLAM_PREEMPT_POLICY_COOPERATIVE;
}

int llam_managed_catalog_run_commit(llam_runtime_t *runtime, bool adapter_ready,
                                     uint64_t available_isa) {
    if (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_LEGACY) return 0;
    pthread_mutex_lock(&g_managed_catalog_lock);
    llam_managed_catalog_t *catalog = runtime->managed_catalog;
    int result = -1;
    if (catalog == NULL || !catalog->running || catalog->committed) {
        errno = EINVAL;
        goto unlock;
    }
    uint32_t reason = LLAM_PREEMPT_REASON_NONE;
    if (runtime->preempt_mode == LLAM_PREEMPT_OFF || runtime->preempt_mode == LLAM_PREEMPT_COOPERATIVE) {
        reason = LLAM_PREEMPT_REASON_REQUEST_DISABLED;
    } else if (catalog->opts.policy == LLAM_PREEMPT_POLICY_COOPERATIVE) {
        reason = LLAM_PREEMPT_REASON_COOPERATIVE_POLICY;
    } else if (!adapter_ready) {
        reason = LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE;
    } else if ((catalog->index.required_isa & ~available_isa) != 0U) {
        reason = LLAM_PREEMPT_REASON_ISA_UNAVAILABLE;
    }
    catalog->reason = reason;
    catalog->effective_policy = reason == LLAM_PREEMPT_REASON_NONE ? catalog->opts.policy
                                                                  : LLAM_PREEMPT_POLICY_COOPERATIVE;
    if (reason != LLAM_PREEMPT_REASON_NONE && catalog->opts.policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED) {
        errno = ENOTSUP;
        goto unlock;
    }
    if (reason == LLAM_PREEMPT_REASON_NONE &&
        atomic_load_explicit(&catalog->legacy_references, memory_order_acquire) != 0U) {
        if (!llam_managed_mixed_available(runtime)) {
            catalog->reason = LLAM_PREEMPT_REASON_LEGACY_MIX_UNQUALIFIED;
            catalog->effective_policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
            errno = ENOTSUP;
            goto unlock;
        }
        catalog->reason = LLAM_PREEMPT_REASON_LEGACY_MIXED;
        catalog->effective_policy = LLAM_PREEMPT_EFFECTIVE_MIXED;
    }
    catalog->committed = true;
    result = 0;
unlock:
    pthread_mutex_unlock(&g_managed_catalog_lock);
    return result;
}

int llam_managed_catalog_async_commit(llam_runtime_t *runtime, uint64_t available_isa) {
    return llam_managed_catalog_run_commit(runtime, true, available_isa);
}

void llam_managed_catalog_run_end(llam_runtime_t *runtime) {
    if (runtime->managed_catalog == NULL) return;
    pthread_mutex_lock(&g_managed_catalog_lock);
    llam_managed_catalog_t *catalog = runtime->managed_catalog;
    if (!catalog->committed && catalog->reason == LLAM_PREEMPT_REASON_PROBE_PENDING) {
        catalog->reason = LLAM_PREEMPT_REASON_STARTUP_FAILED;
    }
    catalog->running = false;
    catalog->committed = false;
    pthread_mutex_unlock(&g_managed_catalog_lock);
}

llam_managed_catalog_t *llam_managed_catalog_detach(llam_runtime_t *runtime) {
    pthread_mutex_lock(&g_managed_catalog_lock);
    llam_managed_catalog_t *catalog = runtime->managed_catalog;
    if (catalog == NULL) {
        pthread_mutex_unlock(&g_managed_catalog_lock);
        errno = 0;
        return NULL;
    }
    if (llam_managed_catalog_busy(catalog)) {
        pthread_mutex_unlock(&g_managed_catalog_lock);
        errno = EBUSY;
        return NULL;
    }
    runtime->managed_catalog = NULL;
    for (llam_module_t *module = catalog->modules; module != NULL; module = module->next) {
        llam_public_slot_release(&g_managed_module_slots, module->slot, module, module->generation);
    }
    pthread_mutex_unlock(&g_managed_catalog_lock);
    return catalog;
}

void llam_managed_catalog_retire(llam_managed_catalog_t *catalog) {
    if (catalog == NULL) return;
    /* The lifecycle gate may have already cleared/freed runtime storage. */
    llam_managed_index_destroy(&catalog->index);
    llam_module_t *modules = catalog->modules;
    while (modules != NULL) {
        llam_module_t *next = modules->next;
        llam_managed_module_destroy(modules);
        modules = next;
    }
    free(catalog);
}

int llam_managed_catalog_destroy(llam_runtime_t *runtime) {
    llam_managed_catalog_t *catalog = llam_managed_catalog_detach(runtime);
    if (catalog == NULL && errno != 0) return -1;
    llam_managed_catalog_retire(catalog);
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
/* Exercises catalog/index lifetime independently of the native loader. This is
 * never reachable through public registration and is not image qualification. */
int llam_managed_catalog_test_register(llam_runtime_t *runtime, const llam_llpm2_t *record,
                                        llam_module_t **out) {
    if (out != NULL) *out = NULL;
    if (runtime == NULL || runtime->managed_catalog == NULL || record == NULL || out == NULL ||
        record->entry_count == 0U || record->entry_count > LLAM_MANAGED_MAX_ENTRIES ||
        record->range_count > LLAM_MANAGED_MAX_RANGES || record->call_count != 0U) {
        errno = EINVAL;
        return -1;
    }
    llam_module_t *module = calloc(1U, sizeof(*module));
    if (module == NULL) { errno = ENOMEM; return -1; }
    module->records.records = calloc(1U, sizeof(*module->records.records));
    if (module->records.records == NULL) goto allocation_failed;
    module->records.count = 1U;
    llam_llpm2_t *copy = module->records.records;
    copy->header = record->header;
    copy->entries = calloc(record->entry_count, sizeof(*copy->entries));
    if (record->range_count != 0U) copy->ranges = calloc(record->range_count, sizeof(*copy->ranges));
    if (copy->entries == NULL || (record->range_count != 0U && copy->ranges == NULL)) goto allocation_failed;
    memcpy(copy->entries, record->entries, record->entry_count * sizeof(*copy->entries));
    if (record->range_count != 0U) memcpy(copy->ranges, record->ranges, record->range_count * sizeof(*copy->ranges));
    copy->entry_count = record->entry_count;
    copy->range_count = record->range_count;
    module->image.base = record->entries[0].native_entry;
    pthread_mutex_lock(&g_managed_catalog_lock);
    int result = llam_managed_module_publish(runtime->managed_catalog, module, out);
    int saved = errno;
    pthread_mutex_unlock(&g_managed_catalog_lock);
    if (result != 0) llam_managed_module_destroy(module);
    errno = saved;
    return result;
allocation_failed:
    llam_managed_module_destroy(module);
    errno = ENOMEM;
    return -1;
}
#endif
