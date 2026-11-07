/**
 * @file src/core/task/task_group.c
 * @brief Structured task group API.
 *
 * @details
 * Task groups provide a small nursery-style ownership layer on top of spawn and
 * join. The group owns child task handles and can attach a shared cancellation
 * token to children that did not provide one explicitly.
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
#include "task_group_internal.h"

#if defined(LLAM_ENABLE_TEST_HOOKS)
static pthread_mutex_t g_group_spawn_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*g_group_spawn_hook)(unsigned, void *);
static void *g_group_spawn_hook_arg;

void llam_task_group_test_spawn_hook(void (*hook)(unsigned, void *), void *argument) {
    pthread_mutex_lock(&g_group_spawn_hook_lock);
    g_group_spawn_hook = hook;
    g_group_spawn_hook_arg = argument;
    pthread_mutex_unlock(&g_group_spawn_hook_lock);
}

static void llam_group_spawn_test_point(unsigned point) {
    pthread_mutex_lock(&g_group_spawn_hook_lock);
    void (*hook)(unsigned, void *) = g_group_spawn_hook;
    void *argument = g_group_spawn_hook_arg;
    pthread_mutex_unlock(&g_group_spawn_hook_lock);
    if (hook != NULL) hook(point, argument);
}
#else
#define llam_group_spawn_test_point(point) ((void)0)
#endif

/**
 * @brief Return true when an ABI prefix contains a complete spawn option field.
 *
 * Task groups normalize spawn options before adding their shared cancellation
 * token.  Match ::llam_spawn_ex by ignoring partially present fixed-width
 * fields instead of letting byte prefixes overwrite documented defaults.
 */
#define LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(prefix_size, field) \
    ((prefix_size) >= offsetof(llam_spawn_opts_t, field) + sizeof(((llam_spawn_opts_t *)0)->field))

static int llam_task_group_reserve_locked(llam_task_group_t *group, size_t needed) {
    llam_task_t **items;
    size_t max_items = SIZE_MAX / sizeof(llam_task_t *);
    size_t new_capacity;

    if (needed > max_items) {
        errno = ENOMEM;
        return -1;
    }
    if (group->capacity > max_items) {
        errno = ENOMEM;
        return -1;
    }
    if (needed <= group->capacity) {
        return 0;
    }
    if (group->capacity == 0U) {
        new_capacity = 16U;
    } else if (group->capacity > max_items / 2U) {
        new_capacity = needed;
    } else {
        new_capacity = group->capacity * 2U;
    }
    while (new_capacity < needed) {
        if (new_capacity > max_items / 2U) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2U;
    }
    if (new_capacity > max_items) {
        errno = ENOMEM;
        return -1;
    }
    items = realloc(group->tasks, new_capacity * sizeof(*items));
    if (items == NULL) {
        errno = ENOMEM;
        return -1;
    }
    group->tasks = items;
    group->capacity = new_capacity;
    return 0;
}

static int llam_task_group_prepare(llam_task_group_t *group, llam_runtime_t *owner, uint32_t flags) {
    group->owner_runtime = owner;
    group->flags = flags;
    atomic_init(&group->lifetime_refs, 1U);
    llam_public_active_op_init(&group->active_ops);
    int error = pthread_mutex_init(&group->lock, NULL);
    if (error != 0) { errno = error; return -1; }
    group->lock_initialized = true;
    group->cancel_token = llam_cancel_token_create_owned(owner);
    if (group->cancel_token == NULL) return -1;
    group->drain_event = llam_channel_create_owned(1U, owner);
    return group->drain_event != NULL ? 0 : -1;
}

static llam_task_group_t *llam_task_group_create_owned(llam_runtime_t *owner_runtime, uint32_t flags) {
    if (llam_runtime_check_creation_owner(owner_runtime) != 0) return NULL;
    llam_task_group_t *group = calloc(1U, sizeof(*group));
    if (group == NULL) return NULL;
    if (llam_task_group_prepare(group, owner_runtime, flags) != 0 ||
        llam_task_group_register_live(group) != 0) {
        int error = errno;
        llam_task_group_storage_free(group);
        errno = error;
        return NULL;
    }
    return llam_task_group_public_handle(group);
}

llam_task_group_t *llam_task_group_create(void) {
    LLAM_NATIVE_API_GUARD(NULL);
    return llam_task_group_create_owned(llam_runtime_owner_for_new_object(), 0U);
}

llam_task_group_t *llam_runtime_task_group_create(llam_runtime_t *runtime) {
    LLAM_NATIVE_API_GUARD(NULL);
    llam_runtime_t *pinned = NULL;
    if (llam_runtime_begin_object_creation(runtime, &pinned) != 0) return NULL;
    llam_task_group_t *result = llam_task_group_create_owned(pinned, 0U);
    int saved_errno = errno;
    llam_runtime_end_public_op(pinned);
    errno = saved_errno;
    return result;
}

static int llam_task_group_normalize_flags(const llam_task_group_opts_t *opts, size_t size,
                                           uint32_t *flags) {
    *flags = 0U;
    if (opts == NULL) return 0;
    if (size == 0U) { errno = EINVAL; return -1; }
    llam_task_group_opts_t raw = {0};
    memcpy(&raw, opts, size < sizeof(raw) ? size : sizeof(raw));
    if (size >= sizeof(raw.flags)) *flags = raw.flags;
    if ((*flags & ~LLAM_TASK_GROUP_F_LINK_CANCEL) != 0U ||
        (size >= sizeof(raw) && raw.reserved0 != 0U)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

llam_task_group_t *llam_task_group_create_ex(const llam_task_group_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(NULL);
    uint32_t flags;
    if (llam_task_group_normalize_flags(opts, size, &flags) != 0) return NULL;
    return llam_task_group_create_owned(llam_runtime_owner_for_new_object(), flags);
}

llam_task_group_t *llam_runtime_task_group_create_ex(llam_runtime_t *runtime,
                                                    const llam_task_group_opts_t *opts, size_t size) {
    LLAM_NATIVE_API_GUARD(NULL);
    uint32_t flags;
    if (llam_task_group_normalize_flags(opts, size, &flags) != 0) return NULL;
    llam_runtime_t *pinned = NULL;
    if (llam_runtime_begin_object_creation(runtime, &pinned) != 0) return NULL;
    llam_task_group_t *result = llam_task_group_create_owned(pinned, flags);
    int saved_errno = errno;
    llam_runtime_end_public_op(pinned);
    errno = saved_errno;
    return result;
}

static int llam_group_spawn_options(const llam_spawn_opts_t *input, size_t size,
                                     llam_spawn_opts_t *out) {
    *out = (llam_spawn_opts_t){.task_class = LLAM_TASK_CLASS_DEFAULT, .stack_class = LLAM_STACK_CLASS_DEFAULT};
    if (input == NULL) return 0;
    if (size == 0U) { errno = EINVAL; return -1; }
    llam_spawn_opts_t raw = {0};
    memcpy(&raw, input, size < sizeof(raw) ? size : sizeof(raw));
#define COPY_GROUP_SPAWN_FIELD(field) \
    if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(size, field)) out->field = raw.field
    COPY_GROUP_SPAWN_FIELD(task_class);
    COPY_GROUP_SPAWN_FIELD(stack_class);
    COPY_GROUP_SPAWN_FIELD(flags);
    COPY_GROUP_SPAWN_FIELD(deadline_ns);
    COPY_GROUP_SPAWN_FIELD(cancel_token);
    COPY_GROUP_SPAWN_FIELD(local_executor);
#undef COPY_GROUP_SPAWN_FIELD
    return 0;
}

static int llam_group_reserve_spawn_locked(llam_task_group_t *group, llam_spawn_opts_t *opts) {
    if (group->sealed) { errno = EPIPE; return -1; }
    /* Ordinary cancel operations may race admission; only a saturated pin
     * sentinel or another consumer prevents a new reservation. */
    if (group->joining || llam_public_active_op_is_saturated(llam_public_active_op_count(&group->active_ops))) {
        errno = EBUSY;
        return -1;
    }
    size_t max_items = SIZE_MAX / sizeof(*group->tasks);
    /* Every active spawn owns a promised append slot. Subtraction preserves
     * this bound even for imported/corrupted counters near SIZE_MAX. */
    if (group->count >= max_items || group->active_spawns >= max_items - group->count) {
        errno = ENOMEM;
        return -1;
    }
    size_t needed = group->count + group->active_spawns + 1U;
    if (llam_task_group_reserve_locked(group, needed) != 0) return -1;
    if (group->pending_retirements == SIZE_MAX) { errno = EOVERFLOW; return -1; }
    group->active_spawns += 1U;
    group->pending_retirements += 1U;
    if (opts->cancel_token == NULL) opts->cancel_token = group->cancel_token;
    return 0;
}

static void llam_group_finish_spawn(llam_task_group_t *group, llam_task_t *task) {
    pthread_mutex_lock(&group->lock);
    group->active_spawns -= 1U;
    if (task != NULL) group->tasks[group->count++] = task;
    else group->pending_retirements -= 1U;
    bool signal = llam_task_group_signal_locked(group);
    pthread_mutex_unlock(&group->lock);
    llam_task_group_signal(group, signal);
}

llam_task_t *llam_task_group_spawn_ex(llam_task_group_t *group,
                                      llam_task_fn fn, void *arg,
                                      const llam_spawn_opts_t *opts, size_t opts_size) {
    LLAM_NATIVE_API_GUARD(NULL);
    if (group == NULL || fn == NULL) { errno = EINVAL; return NULL; }
    llam_spawn_opts_t effective;
    if (llam_group_spawn_options(opts, opts_size, &effective) != 0) return NULL;
    llam_group_spawn_test_point(1U);
    if (llam_task_group_lock_live(group, &group) != 0) return NULL;
    int reserved = llam_group_reserve_spawn_locked(group, &effective);
    int error = errno;
    pthread_mutex_unlock(&group->lock);
    if (reserved != 0) { errno = error; return NULL; }
    llam_group_spawn_test_point(2U);
    /* Execution ownership is attached before runnable publication; group
     * admission remains retained until this call appends or rolls back. */
    llam_task_t *task = llam_runtime_spawn_group_owned_ex(group->owner_runtime, group,
                                                          fn, arg, &effective, sizeof(effective));
    error = errno;
    llam_group_finish_spawn(group, task);
    if (task == NULL) errno = error;
    return task;
}

llam_task_t *llam_task_group_spawn(llam_task_group_t *group,
                                   llam_task_fn fn,
                                   void *arg,
                                   const llam_spawn_opts_t *opts) {
    LLAM_NATIVE_API_GUARD(NULL);

    /* New tail fields require the caller-size handshake of the _ex form. */
    size_t size = opts != NULL ? offsetof(llam_spawn_opts_t, local_executor) : 0U;
    return llam_task_group_spawn_ex(group, fn, arg, opts, size);
}

int llam_task_group_cancel(llam_task_group_t *group) {
    LLAM_NATIVE_API_GUARD(-1);

    llam_cancel_token_t *token;

    if (llam_task_group_lock_live(group, &group) != 0) {
        return -1;
    }
    if (group->cancel_token == NULL) {
        pthread_mutex_unlock(&group->lock);
        errno = EINVAL;
        return -1;
    }
    token = group->cancel_token;
    if (llam_public_active_op_try_begin(&group->active_ops) != 0) {
        pthread_mutex_unlock(&group->lock);
        return -1;
    }
    pthread_mutex_unlock(&group->lock);

    if (llam_cancel_token_cancel(token) != 0) {
        int saved_errno = errno;

        llam_public_active_op_end(&group->active_ops);
        errno = saved_errno;
        return -1;
    }

    llam_public_active_op_end(&group->active_ops);
    return 0;
}

int llam_task_group_consume_children(llam_task_group_t *group, bool has_deadline, uint64_t deadline_ns) {
    pthread_mutex_lock(&group->lock);
    if (!group->joining || group->active_spawns != 0U) abort();
    llam_task_t **tasks = group->tasks;
    size_t count = group->count;
    group->count = 0U;
    pthread_mutex_unlock(&group->lock);
    for (size_t i = 0U; i < count; ++i) {
        if (llam_task_group_join_child_handle(tasks[i], group, has_deadline, deadline_ns) != 0) {
            int error = errno;
            pthread_mutex_lock(&group->lock);
            while (i < count) group->tasks[group->count++] = tasks[i++];
            group->joining = false;
            pthread_mutex_unlock(&group->lock);
            errno = error;
            return -1;
        }
    }
    pthread_mutex_lock(&group->lock);
    group->joining = false;
    pthread_mutex_unlock(&group->lock);
    return 0;
}

static int llam_task_group_join_impl(llam_task_group_t *group, bool has_deadline, uint64_t deadline_ns) {
    if (llam_task_group_lock_live(group, &group) != 0) return -1;
    if (group->active_spawns != 0U || llam_public_active_op_count(&group->active_ops) != 0U || group->joining) {
        pthread_mutex_unlock(&group->lock);
        errno = EBUSY;
        return -1;
    }
    group->joining = true;
    pthread_mutex_unlock(&group->lock);
    return llam_task_group_consume_children(group, has_deadline, deadline_ns);
}

int llam_task_group_join(llam_task_group_t *group) {
    LLAM_NATIVE_API_GUARD(-1);

    return llam_task_group_join_impl(group, false, 0U);
}

int llam_task_group_join_until(llam_task_group_t *group, uint64_t deadline_ns) {
    LLAM_NATIVE_API_GUARD(-1);

    return llam_task_group_join_impl(group, true, deadline_ns);
}
