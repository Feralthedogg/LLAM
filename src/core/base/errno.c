/**
 * @file src/core/base/errno.c
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
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "windows_async_lifetime.h"
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) && LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
#include "windows_async_probe.h"
#endif

/* noinline is also required under LTO: the native TLS address must be resolved
 * in this new activation, after any migration of the caller's C frame. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
LLAM_NO_SANITIZE_THREAD void llam_thread_errno_store(int value) {
    errno = value;
}

/* Resolve errno in a fresh activation after an opaque LLAM call may migrate
 * its caller. libc's const errno-address accessor must not be hoisted across it. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
LLAM_NO_SANITIZE_THREAD int llam_thread_errno_load_current(void) {
    return errno;
}

/**
 * @brief Snapshot the current thread-local @c errno into a managed task.
 *
 * @param task Task whose logical errno state should be updated.
 */
void llam_task_save_errno(llam_task_t *task) {
    if (task != NULL) {
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (task->async_preempt_task != NULL) {
            task->windows_async_saved_last_error = GetLastError();
        }
#endif
        task->saved_errno = llam_thread_errno_load();
    }
}

/**
 * @brief Restore a managed task's logical @c errno into the current worker.
 *
 * @param task Task whose logical errno state should become thread-local state.
 */
void llam_task_restore_errno(const llam_task_t *task) {
    llam_thread_errno_store(task != NULL ? task->saved_errno : 0);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (task != NULL && task->async_preempt_task != NULL) {
        SetLastError(task->windows_async_saved_last_error);
    }
#endif
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
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    bool windows_async_enabled;
    DWORD scheduler_last_error = 0U;
#endif

    if (scheduler_ctx == NULL || task == NULL) {
        abort();
    }

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_scheduler_error(task, true);
#endif
    scheduler_errno = llam_thread_errno_load();
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    windows_async_enabled =
        atomic_load_explicit(&task->owner_runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U;
    if (windows_async_enabled) {
        scheduler_last_error = GetLastError();
    }
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) && LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    llam_windows_async_probe_record_scheduler_errno(task, scheduler_errno);
#endif
#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    llam_async_preempt_save_scheduler_state();
#endif
#if !LLAM_ASYNC_CONTEXT_SHARED
    llam_task_restore_errno(task);
#endif
#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (!llam_async_preempt_resume_task(scheduler_ctx, task)) {
        llam_ctx_switch(scheduler_ctx, &task->ctx);
    }
#elif LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (!llam_windows_async_lifetime_resume_task(scheduler_ctx, task)) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (!llam_windows_async_probe_resume(scheduler_ctx, task)) {
            llam_ctx_switch(scheduler_ctx, &task->ctx);
        }
#else
        llam_ctx_switch(scheduler_ctx, &task->ctx);
#endif
    }
    /* The first return seam must consume G-local state before scheduler restore. */
    (void)llam_windows_async_lifetime_arrival(task);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_probe_arrival(task);
#endif
#elif LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_switch_scheduler_to_task(scheduler_ctx, task, scheduler_errno);
#elif defined(LLAM_ENABLE_TEST_HOOKS) && LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    if (!llam_windows_async_probe_resume(scheduler_ctx, task)) {
        llam_ctx_switch(scheduler_ctx, &task->ctx);
    }
    /* The remote bridge leaves target-local errno/LastError intact until here. */
    llam_windows_async_probe_arrival(task);
#else
    llam_ctx_switch(scheduler_ctx, &task->ctx);
#endif
    llam_thread_errno_store(scheduler_errno);
#if LLAM_ASYNC_CONTEXT_SHARED && defined(LLAM_ENABLE_TEST_HOOKS)
    llam_async_shared_test_scheduler_error(task, scheduler_errno);
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (windows_async_enabled) {
        SetLastError(scheduler_last_error);
    }
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_scheduler_error(task, false);
#endif
}

/**
 * @brief Switch directly from one running task fiber to another task fiber.
 *
 * @param from Currently running task whose context is saved.
 * @param to   Runnable task whose context is restored.
 */
void llam_switch_task_to_task(llam_task_t *from, llam_task_t *to) {
    if (from == NULL || to == NULL) {
        abort();
    }

    llam_task_save_errno(from);
    llam_task_restore_errno(to);
    llam_ctx_switch(&from->ctx, &to->ctx);
    llam_task_restore_errno(from);
}
