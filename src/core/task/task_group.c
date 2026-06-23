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
#include "task_group_internal.h"

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

llam_task_group_t *llam_task_group_create(void) {
    llam_task_group_t *group = calloc(1, sizeof(*group));
    int rc;

    if (group == NULL) {
        return NULL;
    }
    rc = pthread_mutex_init(&group->lock, NULL);
    if (rc != 0) {
        free(group);
        // Preserve pthread's direct error code for FFI callers.
        errno = rc;
        return NULL;
    }
    group->lock_initialized = true;
    group->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&group->active_ops);
    group->cancel_token = llam_cancel_token_create();
    if (group->cancel_token == NULL) {
        pthread_mutex_destroy(&group->lock);
        free(group);
        return NULL;
    }
    if (llam_task_group_register_live(group) != 0) {
        int saved_errno = errno;

        (void)llam_cancel_token_destroy(group->cancel_token);
        pthread_mutex_destroy(&group->lock);
        free(group);
        errno = saved_errno;
        return NULL;
    }
    return llam_task_group_public_handle(group);
}

llam_task_t *llam_task_group_spawn_ex(llam_task_group_t *group,
                                      llam_task_fn fn,
                                      void *arg,
                                      const llam_spawn_opts_t *opts,
                                      size_t opts_size) {
    llam_spawn_opts_t raw_opts;
    llam_spawn_opts_t effective_opts;
    size_t copy_size;
    llam_task_t *task;
    llam_runtime_t *owner_runtime;

    if (group == NULL || fn == NULL) {
        errno = EINVAL;
        return NULL;
    }
    memset(&effective_opts, 0, sizeof(effective_opts));
    effective_opts.task_class = (uint32_t)LLAM_TASK_CLASS_DEFAULT;
    effective_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_DEFAULT;
    if (opts != NULL) {
        if (opts_size == 0U) {
            errno = EINVAL;
            return NULL;
        }
        memset(&raw_opts, 0, sizeof(raw_opts));
        copy_size = opts_size < sizeof(raw_opts) ? opts_size : sizeof(raw_opts);
        memcpy(&raw_opts, opts, copy_size);
        if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, task_class)) {
            effective_opts.task_class = raw_opts.task_class;
        }
        if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, stack_class)) {
            effective_opts.stack_class = raw_opts.stack_class;
        }
        if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, flags)) {
            effective_opts.flags = raw_opts.flags;
        }
        if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, deadline_ns)) {
            effective_opts.deadline_ns = raw_opts.deadline_ns;
        }
        if (LLAM_TASK_GROUP_SPAWN_OPTS_PREFIX_HAS_FIELD(opts_size, cancel_token)) {
            effective_opts.cancel_token = raw_opts.cancel_token;
        }
    }
    if (llam_task_group_lock_live(group, &group) != 0) {
        return NULL;
    }
    if (group->joining) {
        pthread_mutex_unlock(&group->lock);
        errno = EBUSY;
        return NULL;
    }
    if (llam_public_active_op_is_saturated(llam_public_active_op_count(&group->active_ops))) {
        /*
         * Do not treat ordinary active ops as a spawn exclusion gate: group
         * cancel and spawn are allowed to race by design.  Only the saturated
         * sentinel is rejected so corrupted/exhausted public-op state fails
         * closed instead of creating a new child while teardown is unsafe.
         */
        pthread_mutex_unlock(&group->lock);
        errno = EBUSY;
        return NULL;
    }
    if (group->count >= SIZE_MAX / sizeof(*group->tasks)) {
        /*
         * Keep corrupted or future-imported group state from wrapping
         * group->count + 1 to zero and then writing a spawned child through a
         * bogus tasks[SIZE_MAX] slot.
         */
        pthread_mutex_unlock(&group->lock);
        errno = ENOMEM;
        return NULL;
    }
    if (group->active_spawns >= (SIZE_MAX / 2U)) {
        /*
         * active_spawns is a destruction gate while spawn runs outside
         * group->lock.  Values this large are not reachable through normal API
         * use; treat them as corrupted/exhausted state rather than allowing
         * active_spawns + 1 to wrap to zero and open a destroy race.
         */
        pthread_mutex_unlock(&group->lock);
        errno = ENOMEM;
        return NULL;
    }
    if (llam_task_group_reserve_locked(group, group->count + 1U) != 0) {
        pthread_mutex_unlock(&group->lock);
        return NULL;
    }
    group->active_spawns += 1U;
    owner_runtime = group->owner_runtime;
    if (effective_opts.cancel_token == NULL) {
        effective_opts.cancel_token = group->cancel_token;
    }
    pthread_mutex_unlock(&group->lock);

    /*
     * Children belong to the group's owner runtime, not whichever runtime or
     * unmanaged host thread happens to call the group API. Ownership is attached
     * inside spawn before runnable publication; marking the returned borrowed
     * handle afterward leaves a race where a child can consume it first.
     */
    task = llam_runtime_spawn_group_owned_ex(owner_runtime, group, fn, arg, &effective_opts, sizeof(effective_opts));

    pthread_mutex_lock(&group->lock);
    group->active_spawns -= 1U;
    if (task != NULL) {
        group->tasks[group->count++] = task;
    }
    pthread_mutex_unlock(&group->lock);
    return task;
}

llam_task_t *llam_task_group_spawn(llam_task_group_t *group,
                                   llam_task_fn fn,
                                   void *arg,
                                   const llam_spawn_opts_t *opts) {
    return llam_task_group_spawn_ex(group, fn, arg, opts, opts != NULL ? sizeof(*opts) : 0U);
}

int llam_task_group_cancel(llam_task_group_t *group) {
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

static int llam_task_group_join_impl(llam_task_group_t *group, bool has_deadline, uint64_t deadline_ns) {
    llam_task_t **tasks;
    size_t count;
    size_t i;

    if (llam_task_group_lock_live(group, &group) != 0) {
        return -1;
    }

    if (group->active_spawns != 0U ||
        llam_public_active_op_count(&group->active_ops) != 0U ||
        group->joining) {
        pthread_mutex_unlock(&group->lock);
        errno = EBUSY;
        return -1;
    }
    group->joining = true;
    tasks = group->tasks;
    count = group->count;
    group->count = 0U;
    pthread_mutex_unlock(&group->lock);

    for (i = 0U; i < count; ++i) {
        int join_rc = llam_task_group_join_child_handle(tasks[i], group, has_deadline, deadline_ns);

        if (join_rc != 0) {
            int saved_errno = errno;

            pthread_mutex_lock(&group->lock);
            while (i < count) {
                group->tasks[group->count++] = tasks[i++];
            }
            group->joining = false;
            pthread_mutex_unlock(&group->lock);
            errno = saved_errno;
            return -1;
        }
    }
    pthread_mutex_lock(&group->lock);
    group->joining = false;
    pthread_mutex_unlock(&group->lock);
    return 0;
}

int llam_task_group_join(llam_task_group_t *group) {
    return llam_task_group_join_impl(group, false, 0U);
}

int llam_task_group_join_until(llam_task_group_t *group, uint64_t deadline_ns) {
    return llam_task_group_join_impl(group, true, deadline_ns);
}
