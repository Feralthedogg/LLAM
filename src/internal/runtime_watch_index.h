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
 * See LICENSE.
 */

#ifndef LLAM_RUNTIME_WATCH_INDEX_H
#define LLAM_RUNTIME_WATCH_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private auxiliary index. The node watch_lock protects every field. */
typedef struct llam_watch_index_entry {
    struct llam_watch_index_entry *next;
    void *watch;
    uint64_t hash;
} llam_watch_index_entry_t;

typedef struct llam_watch_index {
    llam_watch_index_entry_t **buckets;
    size_t capacity;
    size_t count;
    bool disabled;
} llam_watch_index_t;

llam_watch_index_entry_t *llam_watch_index_first(const llam_watch_index_t *index, int fd);
void llam_watch_index_insert(llam_watch_index_t *index, llam_watch_index_entry_t *entry,
                             int fd, void *watch);
void llam_watch_index_remove(llam_watch_index_t *index, llam_watch_index_entry_t *entry);
void llam_watch_index_reset(llam_watch_index_t *index);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_watch_index_test_fail_next_allocation(void);
#endif
#endif
