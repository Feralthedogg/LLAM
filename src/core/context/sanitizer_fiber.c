/**
 * @file src/core/context/sanitizer_fiber.c
 * @brief AddressSanitizer and ThreadSanitizer fiber-switch integration.
 *
 * @details
 * LLAM changes stacks without calling the operating system. Sanitizers must be
 * told which logical fiber and stack becomes current at every scheduler/task
 * and task/task handoff. Normal builds compile the same lifecycle helpers to
 * no-ops, while sanitizer builds bind directly to compiler-rt's fiber APIs.
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

#if LLAM_ASAN_FIBER_ENABLED
extern void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                           const void *bottom,
                                           size_t size);
extern void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                            const void **bottom_old,
                                            size_t *size_old);

enum {
    LLAM_ASAN_SWITCH_NONE = 0U,
    LLAM_ASAN_SWITCH_FROM_SCHEDULER = 1U,
    LLAM_ASAN_SWITCH_FROM_TASK = 2U,
};

static _Thread_local void *g_llam_asan_scheduler_fake_stack;
static _Thread_local const void *g_llam_asan_scheduler_stack_bottom;
static _Thread_local size_t g_llam_asan_scheduler_stack_size;
static _Thread_local unsigned g_llam_asan_switch_origin;
#endif

#if LLAM_TSAN_FIBER_ENABLED
extern void *__tsan_get_current_fiber(void);
extern void *__tsan_create_fiber(unsigned flags);
extern void __tsan_destroy_fiber(void *fiber);
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags);

/*
 * A context handoff transfers exclusive execution on one operating-system
 * thread. Let TSan model that same-thread ordering; using the no-sync flag
 * would incorrectly treat the physical thread's TLS as concurrently shared
 * between logical fibers. Cross-worker handoff remains ordered by LLAM's
 * synchronized runnable queues.
 */
#define LLAM_TSAN_SWITCH_TO_FIBER_FLAGS 0U

static _Thread_local void *g_llam_tsan_scheduler_fiber;
#endif

int llam_sanitizer_task_fiber_init(llam_task_t *task) {
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_ASAN_FIBER_ENABLED
    task->asan_fake_stack = NULL;
#endif
#if LLAM_TSAN_FIBER_ENABLED
    task->tsan_fiber = __tsan_create_fiber(0U);
    if (task->tsan_fiber == NULL) {
        errno = ENOMEM;
        return -1;
    }
#endif
    return 0;
}

void llam_sanitizer_task_fiber_destroy(llam_task_t *task) {
#if LLAM_TSAN_FIBER_ENABLED
    void *fiber;

    if (task == NULL) {
        return;
    }
    fiber = task->tsan_fiber;
    task->tsan_fiber = NULL;
    if (fiber != NULL) {
        __tsan_destroy_fiber(fiber);
    }
#else
    (void)task;
#endif
}

LLAM_SANITIZER_SWITCH_BOUNDARY void llam_sanitizer_before_scheduler_to_task(
    llam_task_t *task) {
    if (task == NULL || task->stack_base == NULL || task->stack_size == 0U) {
        abort();
    }
#if LLAM_ASAN_FIBER_ENABLED
    g_llam_asan_switch_origin = LLAM_ASAN_SWITCH_FROM_SCHEDULER;
    __sanitizer_start_switch_fiber(&g_llam_asan_scheduler_fake_stack,
                                   task->stack_base,
                                   task->stack_size);
#endif
#if LLAM_TSAN_FIBER_ENABLED
    if (task->tsan_fiber == NULL) {
        abort();
    }
    g_llam_tsan_scheduler_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(task->tsan_fiber,
                           LLAM_TSAN_SWITCH_TO_FIBER_FLAGS);
#endif
}

LLAM_SANITIZER_SWITCH_BOUNDARY void llam_sanitizer_finish_scheduler_switch(void) {
#if LLAM_ASAN_FIBER_ENABLED
    void *fake_stack = g_llam_asan_scheduler_fake_stack;

    g_llam_asan_scheduler_fake_stack = NULL;
    __sanitizer_finish_switch_fiber(fake_stack, NULL, NULL);
    g_llam_asan_switch_origin = LLAM_ASAN_SWITCH_NONE;
#endif
}

LLAM_SANITIZER_SWITCH_BOUNDARY void llam_sanitizer_before_task_to_scheduler(
    llam_task_t *task,
    bool terminal) {
    if (task == NULL) {
        abort();
    }
#if LLAM_ASAN_FIBER_ENABLED
    void **fake_stack_save;

    if (g_llam_asan_scheduler_stack_bottom == NULL ||
        g_llam_asan_scheduler_stack_size == 0U) {
        abort();
    }
    fake_stack_save = terminal ? NULL : &task->asan_fake_stack;
    if (terminal) {
        task->asan_fake_stack = NULL;
    }
    g_llam_asan_switch_origin = LLAM_ASAN_SWITCH_FROM_TASK;
    __sanitizer_start_switch_fiber(fake_stack_save,
                                   g_llam_asan_scheduler_stack_bottom,
                                   g_llam_asan_scheduler_stack_size);
#else
    (void)terminal;
#endif
#if LLAM_TSAN_FIBER_ENABLED
    if (g_llam_tsan_scheduler_fiber == NULL) {
        abort();
    }
    task->tsan_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(g_llam_tsan_scheduler_fiber,
                           LLAM_TSAN_SWITCH_TO_FIBER_FLAGS);
#endif
}

LLAM_SANITIZER_SWITCH_BOUNDARY void llam_sanitizer_before_task_to_task(
    llam_task_t *from,
    llam_task_t *to) {
    if (from == NULL || to == NULL ||
        to->stack_base == NULL || to->stack_size == 0U) {
        abort();
    }
#if LLAM_ASAN_FIBER_ENABLED
    g_llam_asan_switch_origin = LLAM_ASAN_SWITCH_FROM_TASK;
    __sanitizer_start_switch_fiber(&from->asan_fake_stack,
                                   to->stack_base,
                                   to->stack_size);
#endif
#if LLAM_TSAN_FIBER_ENABLED
    if (to->tsan_fiber == NULL) {
        abort();
    }
    from->tsan_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(to->tsan_fiber,
                           LLAM_TSAN_SWITCH_TO_FIBER_FLAGS);
#endif
}

LLAM_SANITIZER_SWITCH_BOUNDARY void llam_sanitizer_finish_task_switch(
    llam_task_t *task) {
    if (task == NULL) {
        abort();
    }
#if LLAM_ASAN_FIBER_ENABLED
    const void *old_stack_bottom = NULL;
    size_t old_stack_size = 0U;
    void *fake_stack = task->asan_fake_stack;

    task->asan_fake_stack = NULL;
    __sanitizer_finish_switch_fiber(fake_stack,
                                    &old_stack_bottom,
                                    &old_stack_size);
    if (g_llam_asan_switch_origin == LLAM_ASAN_SWITCH_FROM_SCHEDULER) {
        if (old_stack_bottom == NULL || old_stack_size == 0U) {
            abort();
        }
        g_llam_asan_scheduler_stack_bottom = old_stack_bottom;
        g_llam_asan_scheduler_stack_size = old_stack_size;
    }
    g_llam_asan_switch_origin = LLAM_ASAN_SWITCH_NONE;
#endif
}
