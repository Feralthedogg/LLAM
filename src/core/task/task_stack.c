/**
 * @file src/core/task/task_stack.c
 * @brief Fiber stack attachment, context setup, and task-side release.
 *
 * @details
 * Mapping policy and cache-list mechanics live behind the stack-cache
 * boundary. This unit only attaches an owned mapping to a task, constructs
 * its execution context, and detaches the mapping during task reclamation.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

/**
 * @brief Allocate and initialize a fiber stack for a task.
 *
 * @param task        Task receiving stack/context state.
 * @param stack_class Requested stack size class.
 * @return 0 on success, -1 on allocation or context setup failure.
 */
int llam_alloc_task_stack(llam_task_t *task, llam_stack_class_t stack_class) {
    size_t stack_size = llam_stack_bytes(stack_class);
    size_t mapping_size = 0U;
    void *mapping;
    llam_shard_t *cache_shard = NULL;
    llam_runtime_t *rt;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = task->owner_runtime;
    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (g_llam_tls_shard != NULL &&
        g_llam_tls_shard->runtime == rt &&
        g_llam_tls_shard->id < rt->active_shards) {
        cache_shard = g_llam_tls_shard;
    } else if (task->home_shard < rt->active_shards) {
        cache_shard = &rt->shards[task->home_shard];
    }

    // Lookup order is local shard cache, then runtime fallback cache, then a
    // fresh guarded platform mapping. This preserves locality without failing
    // if the owner shard has no warm stack available.
    if (!llam_stack_cache_pop_mapping(rt,
                                      cache_shard,
                                      stack_size,
                                      &mapping,
                                      &mapping_size,
                                      &task->stack_base)) {
        if (llam_stack_vm_map(stack_size,
                              &mapping,
                              &mapping_size,
                              &task->stack_base) != 0) {
            return -1;
        }
    }
    task->stack_mapping = mapping;
    task->mapping_size = mapping_size;
    task->stack_size = stack_size;
    if (llam_ctx_init_fp_state(&task->ctx, task->owner_runtime) != 0) {
        int saved_errno = errno;

        llam_stack_mapping_release(rt,
                                   task->stack_mapping,
                                   task->mapping_size);
        task->stack_mapping = NULL;
        task->mapping_size = 0U;
        task->stack_base = NULL;
        task->stack_size = 0U;
        errno = saved_errno;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    if (rt->windows_unsafe_skip_task_simd != 0U) {
        /*
         * Opt-in ceiling mode for benchmark/runtime profiles that guarantee
         * managed tasks do not depend on ABI-preserved XMM6-XMM15 state across
         * cooperative switches.
         */
        task->ctx.simd_flags =
            LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
    }
#endif
#if ((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) && LLAM_ARCH_X86_64) || \
    (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64)
    {
        uintptr_t stack_top = (uintptr_t)task->stack_base + task->stack_size;
        uint64_t *sp;

        stack_top &= ~(uintptr_t)0xFUL;
        sp = (uint64_t *)stack_top;
        // Hand-written x86-64 bootstrap: the assembly switch restores callee
        // saved registers, then returns into llam_fiber_bootstrap with r12=task.
        *--sp = (uint64_t)(uintptr_t)llam_task_exit_internal;
        *--sp = (uint64_t)(uintptr_t)llam_fiber_bootstrap;

        task->ctx.rsp = (uint64_t)(uintptr_t)sp;
        task->ctx.rbx = 0;
        task->ctx.rbp = 0;
#if LLAM_PLATFORM_WINDOWS
        task->ctx.rsi = 0;
        task->ctx.rdi = 0;
#endif
        task->ctx.r12 = (uint64_t)(uintptr_t)task;
        task->ctx.r13 = 0;
        task->ctx.r14 = 0;
        task->ctx.r15 = 0;
    }
#else
    // Non-x86 ports delegate initial context construction to the platform
    // context implementation.
    if (llam_ctx_make_task(&task->ctx,
                           task->stack_base,
                           task->stack_size,
                           task) != 0) {
        int saved_errno = errno;

        llam_ctx_destroy_fp_state(&task->ctx);
        llam_stack_mapping_release(rt,
                                   task->stack_mapping,
                                   task->mapping_size);
        task->stack_mapping = NULL;
        task->mapping_size = 0U;
        task->stack_base = NULL;
        task->stack_size = 0U;
        errno = saved_errno;
        return -1;
    }
#endif
    if (llam_sanitizer_task_fiber_init(task) != 0) {
        int saved_errno = errno;

        llam_ctx_destroy_fp_state(&task->ctx);
        llam_stack_mapping_release(rt,
                                   task->stack_mapping,
                                   task->mapping_size);
        task->stack_mapping = NULL;
        task->mapping_size = 0U;
        task->stack_base = NULL;
        task->stack_size = 0U;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

/**
 * @brief Release a task's stack, preferably into a stack cache.
 *
 * @param task Task whose stack mapping should be detached and recycled.
 */
void llam_task_release_stack(llam_task_t *task) {
    void *mapping;
    size_t mapping_size;
    void *stack_base;
    size_t stack_size;
    llam_shard_t *cache_shard = NULL;
    pthread_mutex_t *diagnostic_lock = NULL;
    llam_runtime_t *rt;

    if (task == NULL) {
        return;
    }

    llam_sanitizer_task_fiber_destroy(task);
    rt = task->owner_runtime;
    if (rt != NULL && task->alloc_owner_shard < rt->active_shards) {
        /*
         * Runtime dumps walk alloc-owner diagnostic lists under this lock. Keep
         * stack pointer/size detachment serialized with those snapshots; the
         * cache insertion itself happens after unlock to avoid nesting cache
         * locks under the diagnostic list lock.
         */
        diagnostic_lock = &rt->shards[task->alloc_owner_shard].lock;
        pthread_mutex_lock(diagnostic_lock);
    }
    if (task->stack_mapping == NULL ||
        task->mapping_size == 0U ||
        task->stack_base == NULL ||
        task->stack_size == 0U) {
        if (diagnostic_lock != NULL) {
            pthread_mutex_unlock(diagnostic_lock);
        }
        return;
    }
    mapping = task->stack_mapping;
    mapping_size = task->mapping_size;
    stack_base = task->stack_base;
    stack_size = task->stack_size;
    task->stack_mapping = NULL;
    task->mapping_size = 0U;
    task->stack_base = NULL;
    task->stack_size = 0U;
    if (diagnostic_lock != NULL) {
        pthread_mutex_unlock(diagnostic_lock);
    }
    if (rt != NULL && task->home_shard < rt->active_shards) {
        cache_shard = &rt->shards[task->home_shard];
    } else if (g_llam_tls_shard != NULL &&
               g_llam_tls_shard->runtime == rt &&
               g_llam_tls_shard->id < rt->active_shards) {
        cache_shard = g_llam_tls_shard;
    }
    // Preserve home-shard locality when possible. The centralized return path
    // enforces the runtime byte budget before either cache publishes an entry.
    (void)llam_stack_cache_return_mapping(rt,
                                          cache_shard,
                                          mapping,
                                          mapping_size,
                                          stack_base,
                                          stack_size);
}

/**
 * @brief Choose an active shard for a spawn operation.
 *
 * @param rt Runtime whose shards should be considered.
 * @return Shard id selected for the new task.
 */
unsigned llam_pick_spawn_shard(llam_runtime_t *rt) {
    unsigned start_id;
    unsigned shard_id;
    unsigned ticket;
    unsigned i;

    if (g_llam_tls_shard != NULL &&
        g_llam_tls_shard->runtime == rt &&
        g_llam_tls_shard->id < rt->active_shards) {
        return g_llam_tls_shard->id;
    }

    /*
     * Unmanaged host threads may call spawn entry points concurrently. The
     * ticket is only a placement hint, but it still must be atomic to avoid
     * corrupting runtime-local placement state under multi-threaded embedders.
     */
    ticket = atomic_fetch_add_explicit(&rt->next_spawn_shard,
                                       1U,
                                       memory_order_relaxed);
    start_id = ticket % rt->active_shards;
    shard_id = start_id;
    // Round-robin across shards that currently accept work; this avoids pushing
    // new tasks into shards that are draining or marked offline.
    for (i = 0; i < rt->active_shards; ++i) {
        unsigned candidate = (start_id + i) % rt->active_shards;

        if (llam_shard_accepts_new_work(&rt->shards[candidate])) {
            return candidate;
        }
    }

    return shard_id;
}
