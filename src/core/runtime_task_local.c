/**
 * @file src/core/runtime_task_local.c
 * @brief Task-local storage key management and per-task value access.
 *
 * @details
 * Task-local values are stored directly on the current task and are cleared
 * when the task object returns to the allocator. Keys are process-global and
 * intentionally small fixed-width integers for FFI friendliness.
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

/** Maximum process-wide task-local keys exposed through the fixed-width ABI. */
#define LLAM_TASK_LOCAL_MAX_KEYS 65535U

static pthread_mutex_t g_llam_task_local_key_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_llam_task_local_key_active;
static size_t *g_llam_task_local_key_refcount;
static size_t g_llam_task_local_key_capacity;
static uint32_t g_llam_task_local_next_key = 1U;

static int llam_task_local_key_ensure_capacity(uint32_t key) {
    unsigned char *active_items;
    size_t *refcount_items;
    size_t new_capacity;

    if ((size_t)key < g_llam_task_local_key_capacity) {
        return 0;
    }
    new_capacity = g_llam_task_local_key_capacity != 0U ? g_llam_task_local_key_capacity : 64U;
    while ((size_t)key >= new_capacity) {
        if (new_capacity > SIZE_MAX / 2U) {
            errno = ENOMEM;
            return -1;
        }
        new_capacity *= 2U;
    }
    active_items = calloc(new_capacity, sizeof(*active_items));
    refcount_items = calloc(new_capacity, sizeof(*refcount_items));
    if (active_items == NULL || refcount_items == NULL) {
        free(active_items);
        free(refcount_items);
        errno = ENOMEM;
        return -1;
    }
    if (g_llam_task_local_key_capacity != 0U) {
        memcpy(active_items,
               g_llam_task_local_key_active,
               g_llam_task_local_key_capacity * sizeof(*active_items));
        memcpy(refcount_items,
               g_llam_task_local_key_refcount,
               g_llam_task_local_key_capacity * sizeof(*refcount_items));
    }
    free(g_llam_task_local_key_active);
    free(g_llam_task_local_key_refcount);
    g_llam_task_local_key_active = active_items;
    g_llam_task_local_key_refcount = refcount_items;
    g_llam_task_local_key_capacity = new_capacity;
    return 0;
}

static bool llam_task_local_key_is_active_locked(llam_task_local_key_t key) {
    return key != 0U &&
           key != LLAM_TASK_LOCAL_INVALID_KEY &&
           (size_t)key < g_llam_task_local_key_capacity &&
           g_llam_task_local_key_active[key] != 0U;
}

static bool llam_task_local_key_is_active(llam_task_local_key_t key) {
    bool active;

    pthread_mutex_lock(&g_llam_task_local_key_lock);
    active = llam_task_local_key_is_active_locked(key);
    pthread_mutex_unlock(&g_llam_task_local_key_lock);
    return active;
}

static bool llam_task_local_find_reusable_key_locked(uint32_t *out_key) {
    size_t key;
    size_t limit;

    if (out_key == NULL || g_llam_task_local_key_active == NULL) {
        return false;
    }
    limit = g_llam_task_local_key_capacity < (size_t)g_llam_task_local_next_key
        ? g_llam_task_local_key_capacity
        : (size_t)g_llam_task_local_next_key;
    if (limit > (size_t)LLAM_TASK_LOCAL_MAX_KEYS + 1U) {
        limit = (size_t)LLAM_TASK_LOCAL_MAX_KEYS + 1U;
    }
    for (key = 1U; key < limit; ++key) {
        /*
         * Deleted keys can remain present in live tasks until those tasks clear
         * the value or exit.  Reuse only when no task-local entry still carries
         * this id, otherwise a new key could observe an old value.
         */
        if (g_llam_task_local_key_active[key] == 0U &&
            g_llam_task_local_key_refcount[key] == 0U) {
            *out_key = (uint32_t)key;
            return true;
        }
    }
    return false;
}

static int llam_task_local_key_ref_acquire(llam_task_local_key_t key) {
    int rc = 0;

    pthread_mutex_lock(&g_llam_task_local_key_lock);
    if (!llam_task_local_key_is_active_locked(key)) {
        errno = EINVAL;
        rc = -1;
    } else {
        g_llam_task_local_key_refcount[key] += 1U;
    }
    pthread_mutex_unlock(&g_llam_task_local_key_lock);
    return rc;
}

static void llam_task_local_key_ref_release(llam_task_local_key_t key) {
    pthread_mutex_lock(&g_llam_task_local_key_lock);
    if ((size_t)key < g_llam_task_local_key_capacity &&
        g_llam_task_local_key_refcount[key] != 0U) {
        g_llam_task_local_key_refcount[key] -= 1U;
    }
    pthread_mutex_unlock(&g_llam_task_local_key_lock);
}

int llam_task_local_key_create(llam_task_local_key_t *out_key) {
    uint32_t key = 0U;
    bool new_key = false;

    if (out_key == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_key = LLAM_TASK_LOCAL_INVALID_KEY;

    pthread_mutex_lock(&g_llam_task_local_key_lock);
    if (!llam_task_local_find_reusable_key_locked(&key)) {
        if (g_llam_task_local_next_key > LLAM_TASK_LOCAL_MAX_KEYS) {
            pthread_mutex_unlock(&g_llam_task_local_key_lock);
            errno = ENOSPC;
            return -1;
        }
        key = g_llam_task_local_next_key;
        new_key = true;
    }
    if (llam_task_local_key_ensure_capacity(key) != 0) {
        pthread_mutex_unlock(&g_llam_task_local_key_lock);
        return -1;
    }
    if (new_key) {
        // A transient allocation failure must not consume a public key id.
        g_llam_task_local_next_key = key + 1U;
    }
    g_llam_task_local_key_active[key] = 1U;
    pthread_mutex_unlock(&g_llam_task_local_key_lock);

    *out_key = key;
    return 0;
}

int llam_task_local_key_delete(llam_task_local_key_t key) {
    pthread_mutex_lock(&g_llam_task_local_key_lock);
    if (!llam_task_local_key_is_active_locked(key)) {
        pthread_mutex_unlock(&g_llam_task_local_key_lock);
        errno = EINVAL;
        return -1;
    }
    g_llam_task_local_key_active[key] = 0U;
    pthread_mutex_unlock(&g_llam_task_local_key_lock);
    return 0;
}

void *llam_task_local_get(llam_task_local_key_t key) {
    llam_task_local_entry_t *entry;
    void *value = NULL;

    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return NULL;
    }

    /*
     * Keep delete/get linearized. If key_delete marks a key inactive, a
     * concurrent get must not scan a stale live-task entry and report success.
     */
    pthread_mutex_lock(&g_llam_task_local_key_lock);
    if (!llam_task_local_key_is_active_locked(key)) {
        pthread_mutex_unlock(&g_llam_task_local_key_lock);
        errno = EINVAL;
        return NULL;
    }

    for (entry = g_llam_tls_task->task_locals; entry != NULL; entry = entry->next) {
        if (entry->key == key) {
            value = entry->value;
            break;
        }
    }
    pthread_mutex_unlock(&g_llam_task_local_key_lock);
    /*
     * NULL is a valid "no value has been set" result for an active key. Clear
     * errno so callers can distinguish that successful miss from ENOTSUP/EINVAL
     * without depending on whatever errno happened to contain on entry.
     */
    errno = 0;
    return value;
}

int llam_task_local_set(llam_task_local_key_t key, void *value) {
    llam_task_local_entry_t *entry;
    llam_task_local_entry_t *prev = NULL;

    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return -1;
    }

    for (entry = g_llam_tls_task->task_locals; entry != NULL; entry = entry->next) {
        if (entry->key == key) {
            if (value != NULL) {
                /*
                 * Updating an existing entry also has to linearize with
                 * key_delete; otherwise a host could delete a key while a task
                 * stores a new value under the now-inactive id.
                 */
                pthread_mutex_lock(&g_llam_task_local_key_lock);
                if (!llam_task_local_key_is_active_locked(key)) {
                    pthread_mutex_unlock(&g_llam_task_local_key_lock);
                    errno = EINVAL;
                    return -1;
                }
                entry->value = value;
                pthread_mutex_unlock(&g_llam_task_local_key_lock);
                return 0;
            }
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                g_llam_tls_task->task_locals = entry->next;
            }
            /*
             * Deleting a key only marks it inactive globally; live tasks may
             * still hold entries.  Allow those entries to be cleared so hosts
             * can discard plugin/task state without waiting for task exit.
             */
            llam_task_local_key_ref_release(entry->key);
            free(entry);
            return 0;
        }
        prev = entry;
    }

    if (!llam_task_local_key_is_active(key)) {
        errno = EINVAL;
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (llam_task_local_key_ref_acquire(key) != 0) {
        free(entry);
        return -1;
    }
    entry->key = key;
    entry->value = value;
    entry->next = g_llam_tls_task->task_locals;
    g_llam_tls_task->task_locals = entry;
    return 0;
}

void llam_task_local_clear(llam_task_t *task) {
    llam_task_local_entry_t *entry;

    if (task == NULL) {
        return;
    }
    entry = task->task_locals;
    task->task_locals = NULL;
    while (entry != NULL) {
        llam_task_local_entry_t *next = entry->next;

        llam_task_local_key_ref_release(entry->key);
        free(entry);
        entry = next;
    }
}
