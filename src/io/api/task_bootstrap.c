/**
 * @file src/io/api/task_bootstrap.c
 * @brief Task fiber bootstrap and terminal task transitions.
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

#include "io/runtime_io_api_internal.h"

/**
 * @brief Finish the current task and switch back to the scheduler context.
 *
 * This is the terminal path for task fibers.  It marks the task dead, wakes
 * join waiters, decrements shard-local live-task accounting, and requests
 * runtime stop when the last task exits.
 *
 * @note This function does not return.  It assumes g_llam_tls_task and
 *       g_llam_tls_shard identify the running task and owner shard.
 */
void llam_task_exit_internal(void) {
    llam_runtime_t *rt;
    llam_task_t *task = g_llam_tls_task;

    if (LLAM_UNLIKELY(task == NULL ||
                      g_llam_tls_shard == NULL ||
                      task->owner_runtime == NULL ||
                      g_llam_tls_shard->runtime != task->owner_runtime)) {
        abort();
    }
    rt = task->owner_runtime;

    pthread_mutex_lock(&task->lock);
    task->state = LLAM_TASK_STATE_DEAD;
    task->wait_reason = LLAM_WAIT_NONE;
    atomic_store_explicit(&task->completed, 1U, memory_order_release);
    pthread_mutex_unlock(&task->lock);
    llam_trace_shard(g_llam_tls_shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_DEAD, LLAM_WAIT_NONE);
    llam_reinject_join_waiters(rt, task);

    if (llam_runtime_note_task_dead(rt, task)) {
        llam_request_stop(rt);
    }

    llam_task_sample_live_stack(task);
    llam_switch_task_to_scheduler(task,
                                g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
    abort();
}

/**
 * @brief Enter a task fiber, run its user callback, and finalize task exit.
 *
 * @param task Task object whose entry/arg fields are already initialized.
 *
 * @note Called from the architecture-specific fiber bootstrap path.
 */
LLAM_SANITIZER_SWITCH_BOUNDARY void llam_task_bootstrap(llam_task_t *task) {
    llam_runtime_t *rt;

    /*
     * This entry is deliberately free of sanitizer instrumentation: the new
     * physical stack is active, but ASan has not finished the logical fiber
     * switch yet. Validate obvious corruption without invoking the sanitizer
     * API so direct fail-closed probes still terminate as LLAM invariants.
     */
    if (LLAM_UNLIKELY(task == NULL ||
                      task->entry == NULL ||
                      g_llam_tls_shard == NULL ||
                      task->owner_runtime == NULL ||
                      g_llam_tls_shard->runtime != task->owner_runtime)) {
        abort();
    }
#if LLAM_SANITIZER_FIBER_ENABLED
    llam_sanitizer_finish_task_switch(task);
#endif
    g_llam_tls_task = task;
    rt = task->owner_runtime;
    llam_task_restore_errno(task);
    if (rt->run_timing_enabled != 0U || rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE) {
        atomic_store_explicit(&g_llam_tls_shard->last_safepoint_ns, llam_now_ns(), memory_order_relaxed);
    }
    task->entry(task->arg);
    llam_task_exit_internal();
}

/**
 * @brief Abort after detecting an invalid fiber stack alignment.
 *
 * @param rsp Stack pointer value observed by the assembly bootstrap.
 *
 * @note This is a hard-fail diagnostic for ABI violations in context setup.
 */
void llam_fiber_alignment_violation(uint64_t rsp) {
    dprintf(STDERR_FILENO,
            "llam: fiber bootstrap stack misaligned rsp=0x%llx\n",
            (unsigned long long)rsp);
    abort();
}
