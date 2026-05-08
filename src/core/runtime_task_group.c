/**
 * @file src/core/runtime_task_group.c
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

static int llam_task_group_reserve_locked(llam_task_group_t *group, size_t needed) {
    llam_task_t **items;
    size_t new_capacity;

    if (needed <= group->capacity) {
        return 0;
    }
    new_capacity = group->capacity != 0U ? group->capacity * 2U : 16U;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            errno = ENOMEM;
            return -1;
        }
        new_capacity *= 2U;
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

    if (group == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&group->lock, NULL) != 0) {
        free(group);
        errno = ENOMEM;
        return NULL;
    }
    group->lock_initialized = true;
    group->cancel_token = llam_cancel_token_create();
    if (group->cancel_token == NULL) {
        pthread_mutex_destroy(&group->lock);
        free(group);
        return NULL;
    }
    return group;
}

int llam_task_group_destroy(llam_task_group_t *group) {
    if (group == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&group->lock);
    if (group->count != 0U) {
        pthread_mutex_unlock(&group->lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&group->lock);

    if (group->cancel_token != NULL && llam_cancel_token_destroy(group->cancel_token) != 0) {
        return -1;
    }
    if (group->lock_initialized) {
        pthread_mutex_destroy(&group->lock);
    }
    free(group->tasks);
    free(group);
    return 0;
}

llam_task_t *llam_task_group_spawn_ex(llam_task_group_t *group,
                                      llam_task_fn fn,
                                      void *arg,
                                      const llam_spawn_opts_t *opts,
                                      size_t opts_size) {
    llam_spawn_opts_t effective_opts;
    size_t copy_size;
    llam_task_t *task;

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
        copy_size = opts_size < sizeof(effective_opts) ? opts_size : sizeof(effective_opts);
        memcpy(&effective_opts, opts, copy_size);
    }
    if (effective_opts.cancel_token == NULL) {
        effective_opts.cancel_token = group->cancel_token;
    }

    pthread_mutex_lock(&group->lock);
    if (llam_task_group_reserve_locked(group, group->count + 1U) != 0) {
        pthread_mutex_unlock(&group->lock);
        return NULL;
    }
    pthread_mutex_unlock(&group->lock);

    task = llam_spawn_ex(fn, arg, &effective_opts, sizeof(effective_opts));
    if (task == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&group->lock);
    group->tasks[group->count++] = task;
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
    if (group == NULL || group->cancel_token == NULL) {
        errno = EINVAL;
        return -1;
    }
    return llam_cancel_token_cancel(group->cancel_token);
}

int llam_task_group_join(llam_task_group_t *group) {
    llam_task_t **tasks;
    size_t count;
    size_t i;

    if (group == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&group->lock);
    tasks = group->tasks;
    count = group->count;
    group->count = 0U;
    pthread_mutex_unlock(&group->lock);

    for (i = 0U; i < count; ++i) {
        if (llam_join(tasks[i]) != 0) {
            int saved_errno = errno;

            pthread_mutex_lock(&group->lock);
            while (i < count) {
                group->tasks[group->count++] = tasks[i++];
            }
            pthread_mutex_unlock(&group->lock);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}
