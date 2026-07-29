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
void llam_task_release_stack(llam_task_t *task);
void llam_task_mark_reclaim_ready(llam_task_t *task);
void llam_task_local_clear(llam_task_t *task);
bool llam_task_scan_ref_try_acquire(llam_runtime_t *rt, llam_task_t *task);
bool llam_task_scan_ref_release(llam_runtime_t *rt, llam_task_t *task);
int llam_task_wait_scan_refs_quiescent(llam_runtime_t *rt, llam_task_t *task);
void llam_reclaim_claimed_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_detached_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_joined_task(llam_runtime_t *rt, llam_task_t *task);
llam_task_t *llam_task_alloc(llam_shard_t *shard);
void llam_task_allocator_free(llam_task_t *task);
uint64_t llam_runtime_prewarm_share(uint64_t total, unsigned count, unsigned index);
int llam_runtime_prewarm_task_allocators(llam_runtime_t *rt,
                                         uint64_t total,
                                         bool exact,
                                         uint64_t *achieved);
void llam_task_register_public_slab(llam_task_t *items, unsigned count);
int llam_task_unregister_public_slab(llam_task_t *items, unsigned count);
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
int llam_sanitizer_task_fiber_init(llam_task_t *task);
void llam_sanitizer_task_fiber_destroy(llam_task_t *task);
void llam_sanitizer_before_scheduler_to_task(llam_task_t *task);
void llam_sanitizer_finish_scheduler_switch(void);
void llam_sanitizer_before_task_to_scheduler(llam_task_t *task, bool terminal);
void llam_sanitizer_before_task_to_task(llam_task_t *from, llam_task_t *to);
void llam_sanitizer_finish_task_switch(llam_task_t *task);

/*
 * TSan does not model LLAM's stackful fiber switches.  Keep the real shared
 * task state instrumented, but hide POSIX errno TLS loads/stores at switch
 * boundaries so sanitizer runs do not report races on thread-local storage.
 */
static inline LLAM_NO_SANITIZE_THREAD int llam_thread_errno_load(void) {
    return errno;
}

static inline LLAM_NO_SANITIZE_THREAD void llam_thread_errno_store(int value) {
    errno = value;
}

/**
 * @brief Inline task-to-task switch for validated hot handoff paths.
 *
 * @details
 * Direct channel, wake, and join handoffs have already validated both task
 * pointers. Keeping the errno save/restore sequence inline removes one C
 * wrapper call from every fiber-to-fiber handoff while preserving task-local
 * errno semantics.
 */
static inline LLAM_SANITIZER_SWITCH_BOUNDARY void llam_switch_task_to_task_hot(
    llam_task_t *from,
    llam_task_t *to) {
    from->saved_errno = llam_thread_errno_load();
    llam_thread_errno_store(to->saved_errno);
#if LLAM_SANITIZER_FIBER_ENABLED
    llam_sanitizer_before_task_to_task(from, to);
#endif
    llam_ctx_switch(&from->ctx, &to->ctx);
#if LLAM_SANITIZER_FIBER_ENABLED
    llam_sanitizer_finish_task_switch(from);
#endif
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
void llam_shard_drain_stack_cache(llam_shard_t *shard);
void llam_runtime_drain_stack_cache(llam_runtime_t *rt);
int llam_runtime_prewarm_stack_cache(llam_runtime_t *rt,
                                     uint64_t total,
                                     bool exact,
                                     uint64_t *achieved);
int llam_runtime_prewarm_timer_heaps(llam_runtime_t *rt,
                                     uint64_t total,
                                     bool exact,
                                     uint64_t *achieved);

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
int llam_stack_vm_map(size_t stack_size,
                      void **mapping_out,
                      size_t *mapping_size_out,
                      void **stack_base_out);
int llam_stack_vm_release(void *mapping, size_t mapping_size);
int llam_stack_vm_discard(void *stack_base, size_t stack_size);
int llam_stack_vm_reactivate(void *stack_base, size_t stack_size);
int llam_stack_vm_secure_zero(void *stack_base, size_t stack_size);
int llam_stack_vm_sample_resident(void *stack_base,
                                  size_t stack_size,
                                  uint64_t *resident_bytes_out,
                                  bool *valid_out);
void llam_stack_cache_account_remove(llam_runtime_t *rt,
                                     size_t mapping_size,
                                     uint64_t committed_bytes);
void llam_stack_mapping_release(llam_runtime_t *rt,
                                void *mapping,
                                size_t mapping_size);
bool llam_stack_cache_pop_mapping(llam_runtime_t *rt,
                                  llam_shard_t *preferred_shard,
                                  size_t stack_size,
                                  void **mapping_out,
                                  size_t *mapping_size_out,
                                  void **stack_base_out);
bool llam_stack_cache_publish_mapping(llam_runtime_t *rt,
                                      llam_shard_t *preferred_shard,
                                      void *mapping,
                                      size_t mapping_size,
                                      void *stack_base,
                                      size_t stack_size,
                                      uint64_t committed_bytes,
                                      uint64_t last_return_ns,
                                      uint32_t stack_class,
                                      uint32_t state);
bool llam_stack_cache_return_mapping(llam_runtime_t *rt,
                                     llam_shard_t *preferred_shard,
                                     void *mapping,
                                     size_t mapping_size,
                                     void *stack_base,
                                     size_t stack_size);
void llam_pause_cpu(void);
uint64_t llam_slice_ns(llam_task_class_t task_class);
const char *llam_stack_profile_hint(const llam_task_t *task);

/*
 * CPU/NUMA discovery and thread/platform tuning.
 */
unsigned llam_count_allowed_cpus(unsigned **out_cpus);
unsigned llam_detect_cpu_node(unsigned cpu_id);
unsigned llam_find_or_add_node_id(unsigned *node_ids,
                                unsigned *node_count,
                                unsigned limit,
                                unsigned kernel_node_id);
bool llam_runtime_affinity_supported(void);
int llam_runtime_capture_driver_affinity(llam_runtime_t *rt);
int llam_runtime_apply_worker_affinity(llam_runtime_t *rt, unsigned cpu_id);
int llam_runtime_restore_driver_affinity(llam_runtime_t *rt);
bool llam_runtime_native_thread_enter(llam_runtime_t *rt, atomic_uint *counter);
void llam_runtime_native_thread_exit(llam_runtime_t *rt, atomic_uint *counter);
#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef enum llam_test_stack_vm_operation {
    LLAM_TEST_STACK_VM_DISCARD = 0,
    LLAM_TEST_STACK_VM_REACTIVATE = 1,
    LLAM_TEST_STACK_VM_SCRUB = 2,
    LLAM_TEST_STACK_VM_RESIDENT_SAMPLE = 3,
    LLAM_TEST_STACK_VM_OPERATION_COUNT = 4,
} llam_test_stack_vm_operation_t;
void llam_runtime_test_reset_stack_vm_hooks(void);
void llam_runtime_test_set_stack_vm_error(
    llam_test_stack_vm_operation_t operation,
    int error_code);
unsigned llam_runtime_test_stack_vm_calls(
    llam_test_stack_vm_operation_t operation);

typedef enum llam_test_affinity_operation {
    LLAM_TEST_AFFINITY_CAPTURE = 0,
    LLAM_TEST_AFFINITY_APPLY = 1,
    LLAM_TEST_AFFINITY_RESTORE = 2,
    LLAM_TEST_AFFINITY_OPERATION_COUNT = 3,
} llam_test_affinity_operation_t;
void llam_runtime_test_reset_affinity_hooks(void);
void llam_runtime_test_set_affinity_supported(int supported);
void llam_runtime_test_set_affinity_error(llam_test_affinity_operation_t operation,
                                          int error_code);
unsigned llam_runtime_test_affinity_calls(
    llam_test_affinity_operation_t operation);
void llam_runtime_test_fail_shard_create_on(unsigned call_index);
void llam_runtime_test_reset_shard_create_hook(void);
unsigned llam_runtime_test_shard_create_calls(void);
#endif
void llam_tune_block_worker_thread(void);
void llam_tune_ctrl_thread(void);
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
int llam_install_thread_signal_stack(llam_shard_t *shard);
void llam_preempt_signal_handler(int signo);
void llam_restore_process_signal_handlers(llam_runtime_t *rt);

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
typedef enum llam_test_prewarm_kind {
    LLAM_TEST_PREWARM_TASK = 0,
    LLAM_TEST_PREWARM_STACK = 1,
    LLAM_TEST_PREWARM_TIMER = 2,
    LLAM_TEST_PREWARM_KIND_COUNT = 3
} llam_test_prewarm_kind_t;
void llam_runtime_test_set_prewarm_allocation_limit(llam_test_prewarm_kind_t kind,
                                                    uint64_t successful_objects);
void llam_runtime_test_reset_prewarm_allocation_limits(void);
bool llam_runtime_test_prewarm_allocation_permitted(llam_test_prewarm_kind_t kind,
                                                    uint64_t objects);
#endif
int llam_runtime_init_rt(llam_runtime_t *rt,
                         const llam_runtime_opts_t *opts,
                         size_t opts_size,
                         bool heap_allocated);
int llam_runtime_register_handle(llam_runtime_t *rt, bool heap_allocated);
int llam_runtime_claim_destroy_handle(llam_runtime_t *rt, bool *out_heap_allocated);
void llam_runtime_unregister_handle(llam_runtime_t *rt);
void llam_runtime_retire_heap_handle(llam_runtime_t *rt);
int llam_runtime_collect_stats_ex_rt(llam_runtime_t *rt, llam_runtime_stats_t *stats, size_t stats_size);
llam_runtime_t *llam_runtime_current_owner(void);
llam_runtime_t *llam_runtime_owner_for_new_object(void);
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
llam_timer_node_t *llam_timer_heap_remove_at_locked(llam_shard_t *shard, size_t index);
llam_timer_node_t *llam_timer_heap_pop_min_locked(llam_shard_t *shard);
const char *llam_trace_kind_name(llam_trace_kind_t kind);
void llam_trace_shard(llam_shard_t *shard,
                    llam_task_t *task,
                    llam_trace_kind_t kind,
                    llam_task_state_id_t from,
                    llam_task_state_id_t to,
                    llam_wait_reason_t reason);
void llam_uninstall_thread_signal_stack(llam_shard_t *shard);

#endif
