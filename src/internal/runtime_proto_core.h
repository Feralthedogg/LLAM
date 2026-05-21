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
void llam_reclaim_claimed_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_detached_task(llam_runtime_t *rt, llam_task_t *task);
void llam_try_reclaim_joined_task(llam_runtime_t *rt, llam_task_t *task);
llam_task_t *llam_task_alloc(llam_shard_t *shard);
void llam_task_allocator_free(llam_task_t *task);
void llam_runtime_prewarm_task_allocators(llam_runtime_t *rt);

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

/**
 * @brief Inline task-to-task switch for validated hot handoff paths.
 *
 * @details
 * Direct channel, wake, and join handoffs have already validated both task
 * pointers. Keeping the errno save/restore sequence inline removes one C
 * wrapper call from every fiber-to-fiber handoff while preserving task-local
 * errno semantics.
 */
static inline void llam_switch_task_to_task_hot(llam_task_t *from, llam_task_t *to) {
    from->saved_errno = errno;
    errno = to->saved_errno;
    llam_ctx_switch(&from->ctx, &to->ctx);
    errno = from->saved_errno;
}

/*
 * Slab allocators and per-shard cache quiescence.
 *
 * Allocator helpers are shard-local unless a function explicitly accepts the
 * runtime. Remote frees are drained only at quiescent/safepoint-friendly points.
 */
void llam_allocator_destroy(llam_allocator_t *allocator);
int llam_allocator_init(llam_allocator_t *allocator);
void llam_allocator_quiescent(llam_shard_t *shard);
int llam_allocator_grow_io_buffer_slab(llam_shard_t *shard);
int llam_allocator_grow_io_req_slab(llam_shard_t *shard);
int llam_allocator_grow_task_slab(llam_shard_t *shard);
int llam_allocator_grow_timer_slab(llam_shard_t *shard);
int llam_allocator_grow_wait_slab(llam_shard_t *shard);
void llam_allocator_lock(llam_allocator_t *allocator);
void llam_allocator_unlock(llam_allocator_t *allocator);
llam_io_buffer_t *llam_io_buffer_alloc(llam_shard_t *shard, size_t min_capacity);
void llam_io_buffer_allocator_free(llam_io_buffer_t *buffer);
llam_timer_node_t *llam_timer_node_alloc(llam_shard_t *shard);
void llam_shard_drain_stack_cache(llam_shard_t *shard);
void llam_runtime_drain_stack_cache(llam_runtime_t *rt);
void llam_runtime_prewarm_stack_cache(llam_runtime_t *rt);

/*
 * Small utility and environment helpers.
 */
size_t llam_align_up(size_t value, size_t alignment);
void llam_atomic_update_peak(atomic_uint *peak, unsigned value);
const char *llam_env_get(const char *name);
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
void llam_tune_ctrl_thread(void);
void llam_tune_io_worker_thread(llam_node_t *node);
void llam_tune_scheduler_thread(llam_shard_t *shard, bool opaque_helper);

/*
 * Context/FPU and process signal integration.
 */
void llam_clear_xsave_globals(void);
void llam_ctx_destroy_fp_state(llam_ctx_t *ctx);
int llam_ctx_init_fp_state(llam_ctx_t *ctx);
int llam_detect_xsave_support(llam_runtime_t *rt);
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
int llam_wake_handle_wait(int fd, int timeout_ms);
int llam_wake_handle_wait_ns(int fd, int timeout_ms, uint64_t timeout_ns);

/*
 * Blocking-worker jobs, safepoints, tracing, and fatal-stop state.
 */
llam_block_job_t *llam_block_job_alloc(llam_runtime_t *rt);
void llam_block_job_release(llam_runtime_t *rt, llam_block_job_t *job);
int llam_consume_task_wake_error(llam_task_t *task);
int llam_runtime_check_handle(const llam_runtime_t *runtime);
int llam_runtime_collect_stats_ex_rt(llam_runtime_t *rt, llam_runtime_stats_t *stats, size_t stats_size);
llam_runtime_t *llam_runtime_current_owner(void);
llam_runtime_t *llam_runtime_owner_for_new_object(void);
int llam_runtime_check_object_owner(const llam_runtime_t *owner_runtime);
int llam_runtime_require_object_owner(const llam_runtime_t *owner_runtime);
void llam_runtime_lifecycle_lock(void);
int llam_runtime_lifecycle_trylock(void);
void llam_runtime_lifecycle_unlock(void);
int llam_runtime_request_stop_rt(llam_runtime_t *rt);
int llam_runtime_run_rt(llam_runtime_t *rt);
void llam_runtime_shutdown_rt(llam_runtime_t *rt);
int llam_runtime_write_stats_json_rt(llam_runtime_t *rt, int fd);
void llam_record_fatal(llam_runtime_t *rt, int err);
void llam_request_stop(llam_runtime_t *rt);
bool llam_runtime_has_live_tasks(llam_runtime_t *rt);
unsigned llam_runtime_live_tasks(llam_runtime_t *rt);
void llam_runtime_note_task_live(llam_runtime_t *rt, llam_shard_t *shard);
bool llam_runtime_note_task_dead(llam_runtime_t *rt, llam_task_t *task);
void llam_task_safepoint(void);
void llam_task_sample_live_stack(llam_task_t *task);
void llam_task_sample_stack_rsp(llam_task_t *task, uintptr_t rsp);
const char *llam_trace_kind_name(llam_trace_kind_t kind);
void llam_trace_shard(llam_shard_t *shard,
                    llam_task_t *task,
                    llam_trace_kind_t kind,
                    llam_task_state_id_t from,
                    llam_task_state_id_t to,
                    llam_wait_reason_t reason);
void llam_uninstall_thread_signal_stack(llam_shard_t *shard);

#endif
