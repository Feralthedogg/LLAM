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

#ifndef NM_RUNTIME_PROTO_CORE_H
#define NM_RUNTIME_PROTO_CORE_H

#include "runtime_state.h"

/*
 * Task list, task allocation, and stack lifetime.
 *
 * The runtime owns all task handles through a global task list for diagnostics
 * and rehome operations. Stack allocation is intentionally separate from task
 * object allocation so stacks can be cached and reclaimed independently.
 */
void nm_add_task_to_list(nm_runtime_t *rt, nm_task_t *task);
int nm_alloc_task_stack(nm_task_t *task, nm_stack_class_t stack_class);
void nm_free_task(nm_task_t *task);
void nm_task_release_stack(nm_task_t *task);
void nm_task_mark_reclaim_ready(nm_task_t *task);
void nm_try_reclaim_joined_task(nm_runtime_t *rt, nm_task_t *task);
nm_task_t *nm_task_alloc(nm_shard_t *shard);
void nm_task_allocator_free(nm_task_t *task);
void nm_runtime_prewarm_task_allocators(nm_runtime_t *rt);

/*
 * Slab allocators and per-shard cache quiescence.
 *
 * Allocator helpers are shard-local unless a function explicitly accepts the
 * runtime. Remote frees are drained only at quiescent/safepoint-friendly points.
 */
void nm_allocator_destroy(nm_allocator_t *allocator);
void nm_allocator_init(nm_allocator_t *allocator);
void nm_allocator_quiescent(nm_shard_t *shard);
int nm_allocator_grow_io_buffer_slab(nm_shard_t *shard);
int nm_allocator_grow_io_req_slab(nm_shard_t *shard);
int nm_allocator_grow_task_slab(nm_shard_t *shard);
int nm_allocator_grow_timer_slab(nm_shard_t *shard);
int nm_allocator_grow_wait_slab(nm_shard_t *shard);
void nm_allocator_lock(nm_allocator_t *allocator);
void nm_allocator_unlock(nm_allocator_t *allocator);
nm_io_buffer_t *nm_io_buffer_alloc(nm_shard_t *shard, size_t min_capacity);
void nm_io_buffer_allocator_free(nm_io_buffer_t *buffer);
nm_timer_node_t *nm_timer_node_alloc(nm_shard_t *shard);
void nm_shard_drain_stack_cache(nm_shard_t *shard);
void nm_runtime_drain_stack_cache(nm_runtime_t *rt);
void nm_runtime_prewarm_stack_cache(nm_runtime_t *rt);

/*
 * Small utility and environment helpers.
 */
size_t nm_align_up(size_t value, size_t alignment);
void nm_atomic_update_peak(atomic_uint *peak, unsigned value);
const char *nm_env_get(const char *name);
unsigned nm_max_unsigned(unsigned a, unsigned b);
long nm_page_size(void);
void nm_pause_cpu(void);
uint64_t nm_slice_ns(nm_task_class_t task_class);
const char *nm_stack_profile_hint(const nm_task_t *task);

/*
 * CPU/NUMA discovery and thread/platform tuning.
 */
void nm_bind_current_thread_to_cpu(unsigned cpu_id);
unsigned nm_count_allowed_cpus(unsigned **out_cpus);
unsigned nm_detect_cpu_node(unsigned cpu_id);
unsigned nm_find_or_add_node_id(unsigned *node_ids,
                                unsigned *node_count,
                                unsigned limit,
                                unsigned kernel_node_id);
void nm_restore_init_thread_affinity(nm_runtime_t *rt);
void nm_tune_block_worker_thread(void);
void nm_tune_ctrl_thread(void);
void nm_tune_io_worker_thread(nm_node_t *node);
void nm_tune_scheduler_thread(nm_shard_t *shard, bool opaque_helper);

/*
 * Context/FPU and process signal integration.
 */
void nm_clear_xsave_globals(void);
void nm_ctx_destroy_fp_state(nm_ctx_t *ctx);
int nm_ctx_init_fp_state(nm_ctx_t *ctx);
int nm_detect_xsave_support(nm_runtime_t *rt);
void nm_fault_signal_handler(int signo, siginfo_t *info, void *ucontext);
int nm_install_process_signal_handlers(nm_runtime_t *rt);
int nm_install_thread_signal_stack(nm_shard_t *shard);
void nm_preempt_signal_handler(int signo);
void nm_restore_process_signal_handlers(nm_runtime_t *rt);

/*
 * Wake handles and low-level Linux futex/eventfd wrappers.
 */
void nm_drain_node_wake(nm_node_t *node);
void nm_drain_shard_wake(nm_shard_t *shard);
unsigned nm_eventfd_try_claim(atomic_uint *pending);
void nm_kick_node(nm_node_t *node);
void nm_kick_shard(nm_shard_t *shard);
long nm_linux_futex_wait_private(atomic_uint *addr, unsigned expected);
long nm_linux_futex_wait_private_timeout(atomic_uint *addr, unsigned expected, const struct timespec *timeout);
long nm_linux_futex_wake_private(atomic_uint *addr, unsigned count);
void nm_opaque_wake_destroy(nm_shard_t *shard);
int nm_opaque_wake_init(nm_shard_t *shard);
void nm_opaque_wake_signal(nm_shard_t *shard);
void nm_opaque_wake_wait(nm_shard_t *shard);
void nm_wake_all_shards(nm_runtime_t *rt);
int nm_wake_handle_create(void);
void nm_wake_handle_close(int fd);
int nm_wake_handle_wait(int fd, int timeout_ms);
int nm_wake_handle_wait_ns(int fd, int timeout_ms, uint64_t timeout_ns);

/*
 * Blocking-worker jobs, safepoints, tracing, and fatal-stop state.
 */
nm_block_job_t *nm_block_job_alloc(nm_runtime_t *rt);
void nm_block_job_release(nm_runtime_t *rt, nm_block_job_t *job);
int nm_consume_task_wake_error(nm_task_t *task);
void nm_record_fatal(nm_runtime_t *rt, int err);
void nm_request_stop(nm_runtime_t *rt);
void nm_task_safepoint(void);
void nm_task_sample_live_stack(nm_task_t *task);
void nm_task_sample_stack_rsp(nm_task_t *task, uintptr_t rsp);
const char *nm_trace_kind_name(nm_trace_kind_t kind);
void nm_trace_shard(nm_shard_t *shard,
                    nm_task_t *task,
                    nm_trace_kind_t kind,
                    nm_task_state_id_t from,
                    nm_task_state_id_t to,
                    nm_wait_reason_t reason);
void nm_uninstall_thread_signal_stack(nm_shard_t *shard);

#endif
