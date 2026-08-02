/**
 * @file src/core/task/task_context.c
 * @brief Constant-time caller-owned context access for the current task.
 *
 * @details
 * Each managed task carries one spawn-time context pointer and four inline
 * pointer slots. The runtime treats every pointer as opaque: it never
 * dereferences or frees caller-owned context.
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

void *llam_task_user_context(void) {
    int saved_errno = errno;
    void *value;

    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return NULL;
    }
    value = g_llam_tls_task->user_context;
    errno = saved_errno;
    return value;
}

void *llam_task_context_slot_get(uint32_t slot) {
    int saved_errno = errno;
    void *value;

    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return NULL;
    }
    if (slot >= LLAM_TASK_CONTEXT_SLOT_COUNT) {
        errno = EINVAL;
        return NULL;
    }
    value = g_llam_tls_task->context_slots[slot];
    errno = saved_errno;
    return value;
}

int llam_task_context_slot_set(uint32_t slot, void *value) {
    int saved_errno = errno;

    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (slot >= LLAM_TASK_CONTEXT_SLOT_COUNT) {
        errno = EINVAL;
        return -1;
    }
    g_llam_tls_task->context_slots[slot] = value;
    errno = saved_errno;
    return 0;
}
