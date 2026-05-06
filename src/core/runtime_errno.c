/**
 * @file src/core/runtime_errno.c
 * @brief Task-local errno preservation around LLAM fiber context switches.
 *
 * @details
 * POSIX @c errno is thread-local, but LLAM tasks are fiber-local logical
 * execution contexts that may resume on different scheduler workers.  The
 * scheduler therefore treats @c errno as task state at context-switch
 * boundaries: task-to-scheduler switches snapshot it, and scheduler-to-task
 * switches restore it before user code resumes.
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
 * @brief Snapshot the current thread-local @c errno into a managed task.
 *
 * @param task Task whose logical errno state should be updated.
 */
void llam_task_save_errno(llam_task_t *task) {
    if (task != NULL) {
        task->saved_errno = errno;
    }
}

/**
 * @brief Restore a managed task's logical @c errno into the current worker.
 *
 * @param task Task whose logical errno state should become thread-local state.
 */
void llam_task_restore_errno(const llam_task_t *task) {
    errno = task != NULL ? task->saved_errno : 0;
}

/**
 * @brief Switch from a running task back to its scheduler context.
 *
 * @param task          Running task being parked, yielded, or exited.
 * @param scheduler_ctx Scheduler context to resume.
 */
void llam_switch_task_to_scheduler(llam_task_t *task, llam_ctx_t *scheduler_ctx) {
    if (task == NULL || scheduler_ctx == NULL) {
        abort();
    }

    llam_task_save_errno(task);
    llam_ctx_switch(&task->ctx, scheduler_ctx);
    llam_task_restore_errno(task);
}

/**
 * @brief Switch from a scheduler worker into a runnable task.
 *
 * @param scheduler_ctx Current scheduler context to save.
 * @param task          Runnable task context to resume.
 */
void llam_switch_scheduler_to_task(llam_ctx_t *scheduler_ctx, llam_task_t *task) {
    int scheduler_errno;

    if (scheduler_ctx == NULL || task == NULL) {
        abort();
    }

    scheduler_errno = errno;
    llam_task_restore_errno(task);
    llam_ctx_switch(scheduler_ctx, &task->ctx);
    errno = scheduler_errno;
}
