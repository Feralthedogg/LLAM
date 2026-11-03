/*
 * Copyright 2026 Feralthedogg
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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_watch_index_fail_allocation;
void llam_watch_index_test_fail_next_allocation(void) {
    atomic_store_explicit(&g_llam_watch_index_fail_allocation, true, memory_order_relaxed);
}
#endif

static uint64_t llam_watch_index_hash(int fd) {
    return llam_mix64((uint64_t)(unsigned)fd);
}

static bool llam_watch_index_grow(llam_watch_index_t *index) {
    size_t capacity = index->capacity == 0U ? 16U : index->capacity * 2U;
    llam_watch_index_entry_t **buckets;

    if (index->capacity > SIZE_MAX / 2U || capacity > SIZE_MAX / sizeof(*buckets)) {
        return false;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (atomic_exchange_explicit(&g_llam_watch_index_fail_allocation, false, memory_order_relaxed)) {
        return false;
    }
#endif
    buckets = calloc(capacity, sizeof(*buckets));
    if (buckets == NULL) {
        return false;
    }
    for (size_t i = 0U; i < index->capacity; ++i) {
        llam_watch_index_entry_t *entry = index->buckets[i];
        while (entry != NULL) {
            llam_watch_index_entry_t *next = entry->next;
            size_t slot = entry->hash & (capacity - 1U);
            entry->next = buckets[slot];
            buckets[slot] = entry;
            entry = next;
        }
    }
    /* Restore newest-first precedence for retained watches sharing one fd. */
    for (size_t i = 0U; i < capacity; ++i) {
        llam_watch_index_entry_t *entry = buckets[i];
        llam_watch_index_entry_t *head = NULL;
        while (entry != NULL) {
            llam_watch_index_entry_t *next = entry->next;
            entry->next = head;
            head = entry;
            entry = next;
        }
        buckets[i] = head;
    }
    free(index->buckets);
    index->buckets = buckets;
    index->capacity = capacity;
    return true;
}

llam_watch_index_entry_t *llam_watch_index_first(const llam_watch_index_t *index, int fd) {
    return index->capacity == 0U ? NULL :
        index->buckets[llam_watch_index_hash(fd) & (index->capacity - 1U)];
}

void llam_watch_index_insert(llam_watch_index_t *index, llam_watch_index_entry_t *entry,
                             int fd, void *watch) {
    int saved_errno = errno;

    entry->watch = watch;
    entry->hash = llam_watch_index_hash(fd);
    entry->next = NULL;
    if (!index->disabled && index->count >= index->capacity) {
        if (!llam_watch_index_grow(index) && index->capacity == 0U) {
            /* No partial index: first-allocation failure uses the full ownership
             * list until it empties. Resize failure keeps the complete old table. */
            index->disabled = true;
        }
    }
    if (index->capacity != 0U) {
        size_t slot = entry->hash & (index->capacity - 1U);
        entry->next = index->buckets[slot];
        index->buckets[slot] = entry;
    }
    index->count += 1U;
    errno = saved_errno; /* Optional acceleration must not introduce a new API failure. */
}

void llam_watch_index_remove(llam_watch_index_t *index, llam_watch_index_entry_t *entry) {
    if (entry->watch == NULL) {
        return; /* Also supports old test fixtures with manually linked watches. */
    }
    if (index->capacity != 0U) {
        size_t slot = entry->hash & (index->capacity - 1U);
        llam_watch_index_entry_t **cursor = &index->buckets[slot];
        while (*cursor != NULL && *cursor != entry) {
            cursor = &(*cursor)->next;
        }
        assert(*cursor == entry);
        if (*cursor != entry) {
            abort(); /* Never free storage still reachable through an inconsistent index. */
        }
        *cursor = entry->next;
    }
    assert(index->count != 0U);
    index->count -= 1U;
    entry->next = NULL;
    entry->watch = NULL;
    if (index->count == 0U) {
        llam_watch_index_reset(index);
    }
}

void llam_watch_index_reset(llam_watch_index_t *index) {
    free(index->buckets);
    memset(index, 0, sizeof(*index));
}
