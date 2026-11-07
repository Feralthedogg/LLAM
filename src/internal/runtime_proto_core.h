/**
 * @file src/internal/runtime_proto_core.h
 * @brief Internal prototypes for core runtime lifecycle, task, timer, wake, and debug operations.
 *
 * @details
 * This header intentionally collects cross-cutting core helpers that are shared
 * across scheduler, I/O, allocator, and lifecycle translation units. Keep new
 * declarations grouped by ownership so call sites can identify which subsystem
 * owns a helper before using it.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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

#ifndef LLAM_RUNTIME_PROTO_CORE_H
#define LLAM_RUNTIME_PROTO_CORE_H

#include "runtime_state.h"

/*
 * Task list, task allocation, and stack lifetime.
 *
 * The runtime owns all task handles through shard-local task lists for
 * diagnostics, shutdown cleanup, and rehome operations. Stack allocation is
 * intentionally separate from task object allocation so stacks can be cached
 * and reclaimed independently.
 */
void llam_add_task_to_list(llam_runtime_t *rt, llam_task_t *task);
void llam_add_task_to_list_locked(llam_shard_t *shard, llam_task_t *task);
bool llam_task_list_eager_enabled(const llam_runtime_t *rt);
void llam_task_ensure_listed(llam_task_t *task);
int llam_alloc_task_stack(llam_task_t *task, llam_stack_class_t stack_class);
void llam_free_task(llam_task_t *task);
void llam_task_release_resources(llam_task_t *task);
void llam_task_release_stack(llam_task_t *task);
void llam_task_mark_reclaim_ready(llam_task_t *task);
void llam_task_group_note_retirement(llam_task_t *task);
void llam_local_executor_task_retired(llam_task_t *task);
void llam_local_executor_task_started(llam_task_t *task, llam_worker_t *worker);
bool llam_local_executor_worker_closing(llam_worker_t *worker);
bool llam_local_executor_worker_pending(const llam_worker_t *worker);
bool llam_local_executor_worker_idle(const llam_worker_t *worker);
void llam_local_executor_worker_joined(llam_worker_t *worker);
void llam_local_executor_defer_shutdown(llam_runtime_t *runtime);
llam_local_executor_t *llam_local_executor_retire_runtime(llam_runtime_t *runtime);
void llam_local_executor_collect_retired(llam_local_executor_t *head);
bool llam_task_owns_native_thread(const llam_task_t *task);

typedef struct llam_cleanup_wait_state {
    llam_task_t *task;
    unsigned cleanup_depth;
    unsigned drain_depth;
} llam_cleanup_wait_state_t;
int llam_borrowed_cleanup_enter(llam_cleanup_wait_state_t *mask);
void llam_borrowed_cleanup_leave(const llam_cleanup_wait_state_t *mask);
int llam_borrowed_wait_event(llam_runtime_t *owner, llam_channel_t *event);

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
void llam_yield_internal(void);
#else
#define llam_yield_internal llam_yield
#endif
void llam_task_local_clear(llam_task_t *task);
void llam_task_local_cleanup_owned(llam_task_t *task);
void llam_scoped_task_entry_returned(llam_task_t *task);
void llam_scoped_task_prepare_retire(llam_task_t *task);
void llam_scoped_task_retire(llam_task_t *task);
llam_future_t *llam_scoped_task_take_retired(llam_runtime_t *runtime);
void llam_scoped_task_collect_retired(llam_future_t *head);
void llam_call_native_affine_cleanup(llam_blocking_fn fn, void *arg);
bool llam_task_scan_ref_try_acquire(llam_runtime_t *rt, llam_task_t *task);
bool llam_task_scan_ref_release(llam_runtime_t *rt, llam_task_t *task);
int llam_task_wait_scan_refs_quiescent(llam_runtime_t *rt, llam_task_t *task);
void llam_reclaim_claimed_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_detached_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_joined_task(llam_runtime_t *rt, llam_task_t *task);
llam_task_t *llam_task_alloc(llam_shard_t *shard);
void llam_task_allocator_free(llam_task_t *task);
void llam_runtime_prewarm_task_allocators(llam_runtime_t *rt);
void llam_task_register_public_slab(llam_task_t *items, unsigned count);
int llam_task_unregister_public_slab(llam_task_t *items, unsigned count);
bool llam_task_try_unregister_public_slab(llam_task_t *items, unsigned count);
llam_task_t *llam_task_resolve_public_handle(const llam_task_t *handle);
void llam_task_end_public_op(llam_task_t *task);
int llam_task_wait_public_ops_quiescent(llam_task_t *task);
llam_task_t *llam_runtime_spawn_group_owned_ex(llam_runtime_t *runtime,
                                               llam_task_group_t *owning_group,
                                               llam_task_fn fn,
                                               void *arg,
                                               const llam_spawn_opts_t *opts,
                                               size_t opts_size);
int llam_task_claim_join_public_handle(const llam_task_t *handle,
                                       llam_task_t *self,
                                       llam_task_t **out_task,
                                       llam_runtime_t **out_rt,
                                       bool *out_task_pinned);
int llam_task_claim_group_join_public_handle(const llam_task_t *handle,
                                             llam_task_group_t *group,
                                             llam_task_t *self,
                                             llam_task_t **out_task,
                                             llam_runtime_t **out_rt,
                                             bool *out_task_pinned);
int llam_task_claim_detach_public_handle(const llam_task_t *handle,
                                         llam_task_t **out_task,
                                         llam_runtime_t **out_rt,
                                         bool *out_reclaim_after_unlock,
                                         bool *out_task_pinned);

/*
 * Task-local errno and fiber context-switch boundaries.
 *
 * These helpers are the only scheduler/task switch entry points used outside
 * architecture context implementations.  They preserve POSIX errno as logical
 * task state even when tasks move between worker threads.
 */
void llam_task_save_errno(llam_task_t *task);
void llam_task_restore_errno(const llam_task_t *task);
void llam_switch_task_to_scheduler(llam_task_t *task, llam_ctx_t *scheduler_ctx);
void llam_switch_scheduler_to_task(llam_ctx_t *scheduler_ctx, llam_task_t *task);
void llam_switch_task_to_task(llam_task_t *from, llam_task_t *to);

#ifndef LLAM_NO_SANITIZE_THREAD
#ifndef __has_feature
#define __has_feature(x) 0
#define LLAM_UNDEF_HAS_FEATURE 1
#endif
#if defined(__SANITIZE_THREAD__)
#define LLAM_TSAN_BUILD 1
#elif __has_feature(thread_sanitizer)
#define LLAM_TSAN_BUILD 1
#endif
#if defined(LLAM_UNDEF_HAS_FEATURE)
#undef __has_feature
#undef LLAM_UNDEF_HAS_FEATURE
#endif
#if defined(LLAM_TSAN_BUILD)
#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define LLAM_NO_SANITIZE_THREAD __attribute__((no_sanitize("thread")))
#elif __has_attribute(no_sanitize_thread)
#define LLAM_NO_SANITIZE_THREAD __attribute__((no_sanitize_thread))
#endif
#endif
#endif
#ifndef LLAM_NO_SANITIZE_THREAD
#define LLAM_NO_SANITIZE_THREAD
#endif
/* Native signal adapters also use this build qualification marker. */
#endif

/*
 * TSan does not model LLAM's stackful fiber switches.  Keep the real shared
 * task state instrumented, but hide POSIX errno TLS loads/stores at switch
 * boundaries so sanitizer runs do not report races on thread-local storage.
 */
static inline LLAM_NO_SANITIZE_THREAD int llam_thread_errno_load(void) {
    return errno;
}

/* A resumed G can be on another native M. Keep the store out of line so the
 * compiler cannot reuse a const libc errno accessor from before ctx_switch. */
LLAM_NO_SANITIZE_THREAD void llam_thread_errno_store(int value);

/**
 * @brief Inline task-to-task switch for validated hot handoff paths.
 *
 * @details
 * Direct channel, wake, and join handoffs have already validated both task
 * pointers. Keeping the errno save/restore sequence inline removes one C
 * wrapper call from every fiber-to-fiber handoff while preserving task-local
 * errno semantics. The caller supplies its pre-bookkeeping errno snapshot,
 * avoiding a redundant native TLS store followed immediately by a load.
 */
static inline void llam_switch_task_to_task_hot(llam_task_t *from, llam_task_t *to,
                                                int saved_errno) {
    from->saved_errno = saved_errno;
    llam_thread_errno_store(to->saved_errno);
    llam_ctx_switch(&from->ctx, &to->ctx);
    llam_thread_errno_store(from->saved_errno);
}

/*
 * Slab allocators and per-shard cache quiescence.
 *
 * Allocator helpers are shard-local unless a function explicitly accepts the
 * runtime. Remote frees are drained only at quiescent/safepoint-friendly points.
 */
void llam_allocator_destroy(llam_allocator_t *allocator);
int llam_allocator_init(llam_allocator_t *allocator);
int llam_allocator_record_chunk(llam_allocator_t *allocator,
                                void *storage,
                                size_t bytes,
                                bool mmapped,
                                unsigned item_kind,
                                unsigned item_count);
void llam_allocator_quiescent(llam_shard_t *shard);
int llam_allocator_grow_io_buffer_slab(llam_shard_t *shard);
int llam_allocator_grow_io_req_slab(llam_shard_t *shard);
int llam_allocator_grow_task_slab(llam_shard_t *shard);
int llam_allocator_grow_timer_slab(llam_shard_t *shard);
int llam_allocator_grow_wait_slab(llam_shard_t *shard);
void llam_allocator_lock(llam_allocator_t *allocator);
void llam_allocator_unlock(llam_allocator_t *allocator);
void llam_allocator_link_chunk_locked(llam_allocator_t *allocator, llam_alloc_chunk_t *chunk);
llam_task_t *llam_task_span_alloc_external(llam_shard_t *shard);
void llam_task_span_release_external(llam_shard_t *owner, llam_task_t *task);
void llam_task_span_quiescent(llam_shard_t *owner);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_task_span_test_fail_next(unsigned stage);
#endif

/*
 * Public task-handle tagging.
 *
 * Task objects are recycled aggressively by the shard allocator. Public task
 * handles are slot+family-tagged-generation values so consumed or wrong-family
 * handles cannot alias a newly spawned task that reused the same task object.
 */
#if UINTPTR_MAX <= UINT32_MAX
#error "LLAM task public handles require uintptr_t wider than 32 bits"
#define LLAM_TASK_PUBLIC_HANDLE_SHIFT 0U
#else
#define LLAM_TASK_PUBLIC_HANDLE_SHIFT 32U
#endif

static inline llam_task_t *llam_task_public_handle(llam_task_t *task) {
    unsigned generation;

    if (task == NULL) {
        return NULL;
    }
    generation = atomic_load_explicit(&task->public_handle_generation, memory_order_acquire);
    return (llam_task_t *)llam_public_slot_encode_handle(task->public_handle_slot,
                                                         generation,
                                                         LLAM_TASK_PUBLIC_HANDLE_SHIFT);
}

int llam_task_activate_public_handle(llam_task_t *task);
void llam_task_invalidate_public_handle(llam_task_t *task);

llam_io_buffer_t *llam_io_buffer_allocator_alloc(llam_shard_t *shard, size_t min_capacity);
void llam_io_buffer_allocator_free(llam_io_buffer_t *buffer);
llam_timer_node_t *llam_timer_node_alloc(llam_shard_t *shard);
void llam_processor_drain_stack_cache(llam_processor_t *processor);
void llam_runtime_drain_stack_cache(llam_runtime_t *rt);
void llam_runtime_prewarm_stack_cache(llam_runtime_t *rt);

/*
 * Small utility and environment helpers.
 */
int llam_align_up_checked(size_t value, size_t alignment, size_t *out_value);
size_t llam_align_up(size_t value, size_t alignment);
void llam_atomic_update_peak(atomic_uint *peak, unsigned value);
const char *llam_env_get(const char *name);
bool llam_ascii_is_space(int ch);
unsigned llam_env_flag(const char *name, unsigned default_value);
unsigned llam_env_flag_value(const char *value, unsigned default_value);
unsigned llam_max_unsigned(unsigned a, unsigned b);
long llam_page_size(void);
void llam_pause_cpu(void);
uint64_t llam_slice_ns(llam_task_class_t task_class);
const char *llam_stack_profile_hint(const llam_task_t *task);

/*
 * CPU/NUMA discovery and thread/platform tuning.
 */
void llam_bind_current_thread_to_cpu(unsigned cpu_id);
unsigned llam_count_allowed_cpus(unsigned **out_cpus);
unsigned llam_detect_cpu_node(unsigned cpu_id);
unsigned llam_find_or_add_node_id(unsigned *node_ids,
                                unsigned *node_count,
                                unsigned limit,
                                unsigned kernel_node_id);
void llam_restore_init_thread_affinity(llam_runtime_t *rt);
void llam_tune_block_worker_thread(void);
void llam_tune_ctrl_thread(bool native_progress);
void llam_tune_io_worker_thread(llam_node_t *node);
void llam_tune_scheduler_thread(llam_shard_t *shard, bool opaque_helper);

/*
 * Context/FPU and process signal integration.
 */
void llam_clear_xsave_globals(void);
void llam_ctx_destroy_fp_state(llam_ctx_t *ctx);
int llam_ctx_init_fp_state(llam_ctx_t *ctx, llam_runtime_t *rt);
int llam_detect_xsave_support(llam_runtime_t *rt);
void llam_release_xsave_globals(llam_runtime_t *rt);
void llam_fault_signal_handler(int signo, siginfo_t *info, void *ucontext);
int llam_install_process_signal_handlers(llam_runtime_t *rt);
int llam_install_thread_signal_stack(llam_worker_t *worker);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_worker_test_force_sigaltstack_error(llam_worker_role_t role, int error_code);
#endif
void llam_preempt_signal_handler(int signo);
void llam_restore_process_signal_handlers(llam_runtime_t *rt);
void llam_preempt_capabilities_init(llam_runtime_t *runtime);
void llam_preempt_capabilities_note_ranges(llam_runtime_t *runtime, size_t range_count);
void llam_preempt_capabilities_note_async_result(
    llam_runtime_t *runtime,
    bool enabled,
    llam_preempt_failure_reason_t failure_reason);
llam_preempt_effective_policy_t llam_preempt_effective_policy_for(
    unsigned preempt_mode,
    uint32_t compiled_backends,
    bool async_enabled,
    size_t registered_safe_ranges);
const char *llam_preempt_effective_policy_name(llam_preempt_effective_policy_t policy);
const char *llam_preempt_async_state_name(llam_preempt_async_state_t state);
const char *llam_preempt_failure_reason_name(llam_preempt_failure_reason_t reason);
int llam_preempt_module_register(llam_runtime_t *runtime,
                                 uintptr_t image_base,
                                 size_t image_size,
                                 const void *metadata,
                                 size_t metadata_size,
                                 bool required_mode);
#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard);
bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard);
unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task);
bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task);
llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback);
void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task);
int llam_async_preempt_run_begin(llam_runtime_t *runtime);
void llam_async_preempt_run_abort(llam_runtime_t *runtime, int error,
                                  llam_preempt_failure_reason_t reason);
void llam_async_preempt_run_end(llam_runtime_t *runtime);
int llam_async_preempt_block_reserve(llam_runtime_t *runtime, bool *reserved);
void llam_async_preempt_block_release(llam_runtime_t *runtime);
bool llam_async_preempt_drain_service_needed(const llam_runtime_t *runtime);
void llam_async_preempt_drain_probe(llam_shard_t *shard, uint64_t now_ns);
bool llam_async_preempt_dynamic_allowed(const llam_runtime_t *runtime);
bool llam_async_preempt_idle_domain(const llam_shard_t *shard);
#else
#if LLAM_ASYNC_CONTEXT_SHARED || (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT))
unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard);
bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard);
unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task);
bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task);
llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback);
void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task);
#else
static inline unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard) {
    (void)shard;
    return 0U;
}
static inline bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard) {
    (void)shard;
    return false;
}
static inline unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task) {
    (void)task;
    return UINT_MAX;
}
static inline bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task) {
    (void)shard;
    (void)task;
    return false;
}
static inline llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback) {
    (void)shard;
    (void)fallback;
    return NULL;
}
static inline void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task) {
    (void)worker;
    (void)task;
}
#endif
static inline int llam_async_preempt_block_reserve(llam_runtime_t *runtime, bool *reserved) {
    (void)runtime;
    *reserved = false;
    return 0;
}
static inline void llam_async_preempt_block_release(llam_runtime_t *runtime) {
    (void)runtime;
}
static inline bool llam_async_preempt_drain_service_needed(const llam_runtime_t *runtime) {
    (void)runtime;
    return false;
}
static inline void llam_async_preempt_drain_probe(llam_shard_t *shard, uint64_t now_ns) {
    (void)shard;
    (void)now_ns;
}
static inline bool llam_async_preempt_dynamic_allowed(const llam_runtime_t *runtime) {
    (void)runtime;
    return true;
}
static inline bool llam_async_preempt_idle_domain(const llam_shard_t *shard) {
    (void)shard;
    return true;
}
#endif
#if defined(__linux__) && defined(__x86_64__)
int llam_async_preempt_register_safe_range(llam_runtime_t *runtime,
                                           uintptr_t begin,
                                           uintptr_t end);
int llam_async_preempt_enable_runtime(llam_runtime_t *runtime);
void llam_async_preempt_shutdown_runtime(llam_runtime_t *runtime);
int llam_async_preempt_worker_ready(llam_worker_t *worker);
bool llam_async_preempt_prepare_request(
    llam_worker_t *worker,
    llam_task_t *task,
    const llam_processor_owner_ticket_t *ticket,
    uint64_t worker_run_generation,
    uint64_t task_run_generation,
    uint64_t *out_request_generation);
void llam_async_preempt_cancel_request(llam_worker_t *worker,
                                      uint64_t request_generation);
bool llam_async_preempt_sample_task_stack(llam_task_t *task);
void llam_async_preempt_trampoline_c(void *opaque_state);
#if defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int llam_async_preempt_task_init(llam_task_t *task);
void llam_async_preempt_task_destroy(llam_task_t *task);
void llam_async_preempt_save_scheduler_state(void);
bool llam_async_preempt_resume_task(llam_ctx_t *scheduler_ctx, llam_task_t *task);
void llam_async_preempt_scheduler_return(llam_shard_t *shard, llam_task_t *task);
void llam_async_preempt_worker_stopped(llam_worker_t *worker);
#endif
#else
static inline bool llam_async_preempt_prepare_request(
    llam_worker_t *worker,
    llam_task_t *task,
    const llam_processor_owner_ticket_t *ticket,
    uint64_t worker_run_generation,
    uint64_t task_run_generation,
    uint64_t *out_request_generation) {
    (void)worker;
    (void)task;
    (void)ticket;
    (void)worker_run_generation;
    (void)task_run_generation;
    (void)out_request_generation;
    return false;
}
static inline void llam_async_preempt_cancel_request(llam_worker_t *worker,
                                                     uint64_t request_generation) {
    (void)worker;
    (void)request_generation;
}
#endif

/*
 * Wake handles and low-level Linux futex/eventfd wrappers.
 */
void llam_drain_node_wake(llam_node_t *node);
void llam_drain_shard_wake(llam_shard_t *shard);
unsigned llam_eventfd_try_claim(atomic_uint *pending);
void llam_kick_node(llam_node_t *node);
void llam_kick_shard(llam_shard_t *shard);
long llam_linux_futex_wait_private(atomic_uint *addr, unsigned expected);
long llam_linux_futex_wait_private_timeout(atomic_uint *addr, unsigned expected, const struct timespec *timeout);
long llam_linux_futex_wake_private(atomic_uint *addr, unsigned count);
void llam_opaque_wake_destroy(llam_shard_t *shard);
int llam_opaque_wake_init(llam_shard_t *shard);
void llam_opaque_wake_signal(llam_shard_t *shard);
void llam_opaque_wake_wait(llam_shard_t *shard);
void llam_wake_all_shards(llam_runtime_t *rt);
int llam_wake_handle_create(void);
void llam_wake_handle_close(int fd);
void llam_wake_handle_drain(int fd);
int llam_wake_handle_signal(int fd);
int llam_wake_handle_wait(int fd, int timeout_ms);
int llam_wake_handle_wait_ns(int fd, int timeout_ms, uint64_t timeout_ns);

/*
 * Blocking-worker jobs, safepoints, tracing, and fatal-stop state.
 */
llam_block_job_t *llam_block_job_alloc(llam_runtime_t *rt);
void llam_block_job_release(llam_runtime_t *rt, llam_block_job_t *job);
int llam_consume_task_wake_error(llam_task_t *task);
llam_runtime_t *llam_runtime_default_storage(void);
typedef void (*llam_runtime_live_iter_fn)(llam_runtime_t *rt, void *arg);
int llam_runtime_check_handle(const llam_runtime_t *runtime);
int llam_runtime_begin_public_op(llam_runtime_t *runtime, llam_runtime_t **out_runtime);
void llam_runtime_end_public_op(llam_runtime_t *runtime);
int llam_runtime_for_each_live(llam_runtime_live_iter_fn fn, void *arg);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_runtime_test_force_live_iter_snapshot_alloc_failure(bool enabled);
#endif
int llam_runtime_init_rt(llam_runtime_t *rt,
                         const llam_runtime_opts_t *opts,
                         size_t opts_size,
                         bool heap_allocated);
bool llam_runtime_init_cleanup_here(const llam_runtime_t *runtime);
/* Storage/lease bootstrap only; it does not qualify managed code or native contexts. */
int llam_runtime_init_registered_rt(llam_runtime_t *rt,
                                    const llam_runtime_opts_t *opts,
                                    size_t opts_size,
                                    bool heap_allocated,
                                    size_t managed_worker_count);
struct llam_preempt_opts;
int llam_runtime_init_managed_rt(llam_runtime_t *runtime,
                                 const llam_runtime_opts_t *opts,
                                 size_t opts_size, bool heap_allocated,
                                 const struct llam_preempt_opts *managed_opts);
int llam_managed_catalog_prepare(llam_runtime_t *runtime, const struct llam_preempt_opts *opts);

int llam_runtime_register_handle(llam_runtime_t *rt, bool heap_allocated);
int llam_runtime_claim_destroy_handle(llam_runtime_t *rt, bool *out_heap_allocated);
void llam_runtime_unregister_handle(llam_runtime_t *rt);
void llam_runtime_retire_heap_handle(llam_runtime_t *rt);
int llam_runtime_collect_stats_ex_rt(llam_runtime_t *rt, llam_runtime_stats_t *stats, size_t stats_size);
llam_runtime_t *llam_runtime_current_owner(void);
llam_runtime_t *llam_runtime_owner_for_new_object(void);
int llam_runtime_check_creation_owner(llam_runtime_t *owner);
int llam_runtime_begin_object_creation(llam_runtime_t *runtime, llam_runtime_t **out);
/* Caller retains the owner runtime through handle publication. */
llam_cancel_token_t *llam_cancel_token_create_owned(llam_runtime_t *owner_runtime);
static inline llam_cancel_token_t *llam_task_cancel_source(const llam_task_t *task, unsigned index) {
    return index == 0U ? task->cancel_token
         : index == 1U ? task->group_cancel_token : task->operation_cancel_token;
}
static inline bool llam_task_has_cancel_tokens(const llam_task_t *task) {
    return task != NULL && atomic_load_explicit(&task->cleanup_wait_depth, memory_order_relaxed) == 0U &&
                           (task->cancel_token != NULL || task->group_cancel_token != NULL ||
                            task->operation_cancel_token != NULL);
}
static inline uint64_t llam_task_request_deadline(const llam_task_t *task) {
    if (task == NULL || atomic_load_explicit(&task->cleanup_wait_depth, memory_order_relaxed) != 0U) return 0U;
    uint64_t a = task->request_deadline_ns, b = task->operation_deadline_ns;
    return a == 0U ? b : (b == 0U || a < b ? a : b);
}
static inline void llam_task_merge_wait_deadline(const llam_task_t *task,
                                                 bool *has_deadline, uint64_t *deadline_ns) {
    uint64_t budget = llam_task_request_deadline(task);
    if (budget != 0U && (!*has_deadline || budget < *deadline_ns)) {
        *has_deadline = true;
        *deadline_ns = budget;
    }
}
int llam_thread_errno_load_current(void);
int llam_cancel_token_lock_task(llam_task_t *task, llam_cancel_lock_set_t *set);
void llam_cancel_token_unlock_task(const llam_cancel_lock_set_t *set);
void llam_cancel_token_register_task_locked(llam_task_t *task);
void llam_cancel_token_unregister_task_locked(llam_task_t *task);
int llam_runtime_check_object_owner(const llam_runtime_t *owner_runtime);
int llam_runtime_check_object_owner_for_cleanup(const llam_runtime_t *owner_runtime);
bool llam_runtime_object_owner_is_live(const llam_runtime_t *owner_runtime);
int llam_runtime_begin_live_object_owner_op(const llam_runtime_t *owner_runtime,
                                            llam_runtime_t **out_runtime,
                                            int dead_errno);
bool llam_runtime_begin_object_owner_op_if_live(const llam_runtime_t *owner_runtime,
                                                llam_runtime_t **out_runtime);
int llam_runtime_require_object_owner(const llam_runtime_t *owner_runtime);
llam_task_group_t *llam_task_group_resolve_public_handle(llam_task_group_t *handle);
void llam_task_group_end_public_op(llam_task_group_t *group);
void llam_runtime_lifecycle_lock(void);
int llam_runtime_lifecycle_trylock(void);
void llam_runtime_lifecycle_unlock(void);
int llam_runtime_request_stop_rt(llam_runtime_t *rt);
int llam_runtime_run_rt(llam_runtime_t *rt);
void llam_runtime_shutdown_rt(llam_runtime_t *rt);
void llam_runtime_destroy_rt(llam_runtime_t *rt);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_runtime_test_fail_after_stack_cache_prewarm(void);
void llam_runtime_test_fail_ctrl_thread_start(void);
void llam_runtime_test_before_managed_prepare(void (*hook)(llam_runtime_t *, void *), void *arg);
#endif
int llam_task_group_join_child_handle(llam_task_t *task,
                                      llam_task_group_t *group,
                                      bool has_deadline,
                                      uint64_t deadline_ns);
int llam_runtime_write_stats_json_rt(llam_runtime_t *rt, int fd);
void llam_record_fatal(llam_runtime_t *rt, int err);
void llam_record_fatal_deferred(llam_runtime_t *rt, int err);
void llam_request_stop(llam_runtime_t *rt);
bool llam_runtime_has_live_tasks(llam_runtime_t *rt);
unsigned llam_runtime_live_tasks(llam_runtime_t *rt);
void llam_runtime_note_task_live(llam_runtime_t *rt, llam_shard_t *shard);
bool llam_runtime_note_task_dead(llam_runtime_t *rt, llam_task_t *task);
void llam_task_safepoint(void);
void llam_task_sample_live_stack(llam_task_t *task);
void llam_task_sample_stack_rsp(llam_task_t *task, uintptr_t rsp);
bool llam_timer_heap_push_locked(llam_shard_t *shard, llam_timer_node_t *node);
bool llam_processor_rehome_timers_locked(llam_shard_t *source,
                                         llam_shard_t *target,
                                         unsigned *migrated_out);
llam_timer_node_t *llam_timer_heap_remove_at_locked(llam_shard_t *shard, size_t index);
llam_timer_node_t *llam_timer_heap_pop_min_locked(llam_shard_t *shard);
#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef void (*llam_timer_cancel_owner_sample_hook_fn)(void);
void llam_timer_test_fail_next_rehome_reserve(void);
void llam_timer_test_set_cancel_owner_sample_hook(
    llam_timer_cancel_owner_sample_hook_fn hook);
#endif
const char *llam_trace_kind_name(llam_trace_kind_t kind);
void llam_trace_shard(llam_shard_t *shard,
                    llam_task_t *task,
                    llam_trace_kind_t kind,
                    llam_task_state_id_t from,
                    llam_task_state_id_t to,
                    llam_wait_reason_t reason);
void llam_uninstall_thread_signal_stack(llam_worker_t *worker);

#endif
