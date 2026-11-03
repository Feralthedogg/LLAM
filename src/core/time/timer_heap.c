/**
 * @file src/core/time/timer_heap.c
 * @brief 4-ary timer heap primitives.
 *
 * @details
 * Timer heap mechanics are isolated from wait/cancel dispatch so timer policy
 * changes do not grow the deadline-expiration state machine.
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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#define LLAM_TIMER_HEAP_ARITY 4U

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_timer_test_fail_rehome_reserve;

void llam_timer_test_fail_next_rehome_reserve(void) {
    atomic_store_explicit(&g_llam_timer_test_fail_rehome_reserve,
                          true,
                          memory_order_release);
}
#endif

static size_t llam_timer_heap_parent(size_t index) {
    return (index - 1U) / LLAM_TIMER_HEAP_ARITY;
}

static bool llam_timer_heap_less(const llam_timer_node_t *lhs, const llam_timer_node_t *rhs) {
    if (lhs->deadline_ns != rhs->deadline_ns) {
        return lhs->deadline_ns < rhs->deadline_ns;
    }
    if (lhs->task == NULL || rhs->task == NULL) {
        return lhs < rhs;
    }
    return lhs->task->id < rhs->task->id;
}

static void llam_timer_heap_refresh_root(llam_shard_t *shard) {
    shard->processor.timers = shard->processor.timer_heap_len > 0U ? shard->processor.timer_heap[0] : NULL;
}

static void llam_timer_heap_swap(llam_shard_t *shard, size_t lhs, size_t rhs) {
    llam_timer_node_t *tmp = shard->processor.timer_heap[lhs];

    shard->processor.timer_heap[lhs] = shard->processor.timer_heap[rhs];
    shard->processor.timer_heap[rhs] = tmp;
    shard->processor.timer_heap[lhs]->heap_index = lhs;
    shard->processor.timer_heap[rhs]->heap_index = rhs;
}

static bool llam_timer_heap_reserve(llam_shard_t *shard, size_t needed) {
    llam_timer_node_t **items;
    size_t max_items = SIZE_MAX / sizeof(llam_timer_node_t *);
    size_t new_cap;

    if (needed > max_items || shard->processor.timer_heap_cap > max_items) {
        errno = ENOMEM;
        return false;
    }
    if (needed <= shard->processor.timer_heap_cap) {
        return true;
    }
    if (shard->processor.timer_heap_cap == 0U) {
        new_cap = 64U;
    } else if (shard->processor.timer_heap_cap > max_items / 2U) {
        new_cap = needed;
    } else {
        new_cap = shard->processor.timer_heap_cap * 2U;
    }
    while (new_cap < needed) {
        if (new_cap > max_items / 2U) {
            new_cap = needed;
            break;
        }
        new_cap *= 2U;
    }
    if (new_cap > max_items) {
        errno = ENOMEM;
        return false;
    }
    items = realloc(shard->processor.timer_heap, new_cap * sizeof(*items));
    if (items == NULL) {
        errno = ENOMEM;
        return false;
    }
    shard->processor.timer_heap = items;
    shard->processor.timer_heap_cap = new_cap;
    return true;
}

static bool llam_timer_count_increment(llam_shard_t *shard) {
    unsigned count;

    count = atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire);
    for (;;) {
        if (LLAM_UNLIKELY(count == UINT_MAX)) {
            /*
             * timer_count feeds scheduler pressure and diagnostics.  Refuse
             * new timers instead of wrapping to zero and hiding pending work.
             */
            llam_record_fatal(shard->runtime, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&shard->processor.timer_count,
                                                  &count,
                                                  count + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

static void llam_timer_count_decrement(llam_shard_t *shard) {
    unsigned count;

    count = atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire);
    while (count > 0U) {
        if (atomic_compare_exchange_weak_explicit(&shard->processor.timer_count,
                                                  &count,
                                                  count - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }

    /*
     * Removing a heap entry without a matching accounting credit means a stale
     * timer or corrupted heap state reached the scheduler.  Keep the counter at
     * zero so shutdown/pressure checks cannot see UINT_MAX.
     */
    llam_record_fatal(shard->runtime, EINVAL);
    errno = EINVAL;
}

static void llam_timer_heap_sift_up(llam_shard_t *shard, size_t index) {
    while (index > 0U) {
        size_t parent = llam_timer_heap_parent(index);

        if (!llam_timer_heap_less(shard->processor.timer_heap[index], shard->processor.timer_heap[parent])) {
            break;
        }
        llam_timer_heap_swap(shard, index, parent);
        index = parent;
    }
}

static void llam_timer_heap_sift_down(llam_shard_t *shard, size_t index) {
    for (;;) {
        size_t first_child = index * LLAM_TIMER_HEAP_ARITY + 1U;
        size_t best = index;
        size_t child;

        for (child = first_child;
             child < shard->processor.timer_heap_len && child < first_child + LLAM_TIMER_HEAP_ARITY;
             ++child) {
            if (llam_timer_heap_less(shard->processor.timer_heap[child], shard->processor.timer_heap[best])) {
                best = child;
            }
        }
        if (best == index) {
            break;
        }
        llam_timer_heap_swap(shard, index, best);
        index = best;
    }
}

static bool llam_timer_rehome_entry_valid(llam_shard_t *source,
                                          llam_shard_t *target,
                                          llam_timer_node_t *node,
                                          size_t index) {
    llam_task_t *task;
    unsigned parked_shard;
    unsigned state;
    unsigned wait_reason;
    uint64_t generation;

    if (source == NULL || target == NULL || node == NULL ||
        node->owner_runtime != source->runtime || node->task == NULL ||
        node->owner_shard >= source->runtime->active_shards ||
        node->next != NULL || node->heap_index != index) {
        return false;
    }
    task = node->task;
    state = atomic_load_explicit(&task->state, memory_order_acquire);
    wait_reason = atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    generation = atomic_load_explicit(&task->wait_generation, memory_order_acquire);
    parked_shard = atomic_load_explicit(&task->parked_shard, memory_order_acquire);
    return task->owner_runtime == source->runtime &&
           task->active_timer == node &&
           atomic_load_explicit(&task->active_timer_heap_shard,
                                memory_order_acquire) == source->id &&
           state == LLAM_TASK_STATE_PARKED &&
           wait_reason != LLAM_WAIT_NONE &&
           generation != 0U && generation != UINT64_MAX &&
           generation == node->wait_generation &&
           task->deadline_ns == node->deadline_ns &&
           (parked_shard == source->id || parked_shard == target->id) &&
           (task->flags & LLAM_TASK_FLAG_PINNED) == 0U &&
           atomic_load_explicit(&task->active_select_state,
                                memory_order_acquire) == node->select_state &&
           atomic_load_explicit(&task->active_wait_lifetime_ops,
                                memory_order_acquire) == node->wait_lifetime_ops;
}

static bool llam_timer_target_heap_valid(llam_shard_t *source,
                                         llam_shard_t *target) {
    size_t i;

    if ((target->processor.timer_heap_len > 0U &&
         target->processor.timer_heap == NULL) ||
        target->processor.timer_heap_len > target->processor.timer_heap_cap ||
        target->processor.timers !=
            (target->processor.timer_heap_len > 0U
                 ? target->processor.timer_heap[0]
                 : NULL) ||
        atomic_load_explicit(&target->processor.timer_count,
                             memory_order_acquire) !=
            target->processor.timer_heap_len) {
        return false;
    }
    for (i = 0U; i < target->processor.timer_heap_len; ++i) {
        llam_timer_node_t *node = target->processor.timer_heap[i];
        llam_task_t *task;

        if (node == NULL || node->owner_runtime != source->runtime ||
            node->task == NULL || node->next != NULL || node->heap_index != i ||
            atomic_load_explicit(&node->task->active_timer_heap_shard,
                                 memory_order_acquire) != target->id) {
            return false;
        }
        task = node->task;
        if (task->owner_runtime != source->runtime || task->active_timer != node) {
            return false;
        }
    }
    return true;
}

static void llam_timer_rehome_release_pins(llam_timer_node_t *pins) {
    while (pins != NULL) {
        llam_timer_node_t *next = pins->next;
        llam_task_t *task = pins->task;

        pins->next = NULL;
        llam_task_wait_resolver_end(task);
        pins = next;
    }
}

/**
 * @brief Transactionally move every active source timer into the target heap.
 *
 * @details
 * Caller holds both shard opaque locks and shard locks in ascending shard-id
 * order. Resolver references pin each task's wait-generation and recyclable
 * wait owner through preflight and commit. Capacity is reserved before any
 * ownership field or heap entry changes; the existing node allocation owner is
 * deliberately left untouched.
 */
bool llam_processor_rehome_timers_locked(llam_shard_t *source,
                                         llam_shard_t *target,
                                         unsigned *migrated_out) {
    llam_timer_node_t *pins = NULL;
    size_t source_len;
    size_t target_len;
    size_t merged_len;
    unsigned source_count;
    unsigned target_count;
    size_t i;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source == NULL || target == NULL || source == target ||
        source->runtime == NULL || source->runtime != target->runtime ||
        source->runtime->active_shards == 0U ||
        source->id >= source->runtime->active_shards ||
        target->id >= source->runtime->active_shards ||
        !llam_processor_online(&source->processor) ||
        !llam_processor_online(&target->processor) ||
        atomic_load_explicit(&source->processor.timer_callbacks_active,
                             memory_order_acquire) != 0U) {
        return false;
    }

    source_len = source->processor.timer_heap_len;
    target_len = target->processor.timer_heap_len;
    source_count = atomic_load_explicit(&source->processor.timer_count,
                                        memory_order_acquire);
    target_count = atomic_load_explicit(&target->processor.timer_count,
                                        memory_order_acquire);
    if ((source_len > 0U && source->processor.timer_heap == NULL) ||
        source_len > source->processor.timer_heap_cap ||
        source->processor.timers !=
            (source_len > 0U ? source->processor.timer_heap[0] : NULL) ||
        source_count != source_len ||
        source_count > UINT_MAX - target_count ||
        target_count != target_len ||
        target_len > SIZE_MAX - source_len ||
        !llam_timer_target_heap_valid(source, target)) {
        return false;
    }
    merged_len = target_len + source_len;
    if (merged_len > SIZE_MAX / sizeof(*target->processor.timer_heap)) {
        errno = ENOMEM;
        return false;
    }

    for (i = 0U; i < source_len; ++i) {
        llam_timer_node_t *node = source->processor.timer_heap[i];

        if (node == NULL || node->task == NULL ||
            !llam_task_wait_resolver_try_begin(node->task)) {
            llam_timer_rehome_release_pins(pins);
            return false;
        }
        if (!llam_timer_rehome_entry_valid(source, target, node, i)) {
            llam_task_wait_resolver_end(node->task);
            llam_timer_rehome_release_pins(pins);
            return false;
        }
        node->next = pins;
        pins = node;
    }

#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (atomic_exchange_explicit(&g_llam_timer_test_fail_rehome_reserve,
                                 false,
                                 memory_order_acq_rel)) {
        errno = ENOMEM;
        llam_timer_rehome_release_pins(pins);
        return false;
    }
#endif
    if (!llam_timer_heap_reserve(target, merged_len)) {
        llam_timer_rehome_release_pins(pins);
        return false;
    }

    /* No operation below this point allocates or can reject a preflighted node. */
    for (i = 0U; i < source_len; ++i) {
        llam_timer_node_t *node = source->processor.timer_heap[i];
        llam_task_t *task = node->task;
        size_t target_index = target_len + i;

        target->processor.timer_heap[target_index] = node;
        node->heap_index = target_index;
        atomic_store_explicit(&task->active_timer_heap_shard,
                              target->id,
                              memory_order_release);
        atomic_store_explicit(&task->parked_shard,
                              target->id,
                              memory_order_release);
        if (atomic_load_explicit(&task->home_shard,
                                 memory_order_acquire) == source->id) {
            atomic_store_explicit(&task->home_shard,
                                  target->id,
                                  memory_order_release);
        }
        source->processor.timer_heap[i] = NULL;
    }
    target->processor.timer_heap_len = merged_len;
    source->processor.timer_heap_len = 0U;
    source->processor.timers = NULL;
    for (i = merged_len / LLAM_TIMER_HEAP_ARITY; i-- > 0U;) {
        llam_timer_heap_sift_down(target, i);
    }
    llam_timer_heap_refresh_root(target);
    /*
     * Publish the filled target heap before clearing the source count. Readers
     * use the counters only as skip hints; each actual heap access takes its
     * shard lock. This ordering can over-count briefly but cannot hide a timer.
     */
    atomic_store_explicit(&target->processor.timer_count,
                          target_count + source_count,
                          memory_order_release);
    atomic_store_explicit(&source->processor.timer_count,
                          0U,
                          memory_order_release);
    llam_timer_rehome_release_pins(pins);
    if (migrated_out != NULL) {
        *migrated_out = (unsigned)source_len;
    }
    return true;
}

bool llam_timer_heap_push_locked(llam_shard_t *shard, llam_timer_node_t *node) {
    size_t index;
    size_t max_items = SIZE_MAX / sizeof(*shard->processor.timer_heap);

    if (shard->processor.timer_heap_len >= max_items) {
        errno = ENOMEM;
        return false;
    }
    if (!llam_timer_heap_reserve(shard, shard->processor.timer_heap_len + 1U)) {
        return false;
    }
    if (!llam_timer_count_increment(shard)) {
        return false;
    }
    index = shard->processor.timer_heap_len++;
    shard->processor.timer_heap[index] = node;
    node->heap_index = index;
    llam_timer_heap_sift_up(shard, index);
    llam_timer_heap_refresh_root(shard);
    return true;
}

llam_timer_node_t *llam_timer_heap_remove_at_locked(llam_shard_t *shard, size_t index) {
    llam_timer_node_t *node;
    llam_timer_node_t *last;

    if (index >= shard->processor.timer_heap_len) {
        return NULL;
    }

    node = shard->processor.timer_heap[index];
    shard->processor.timer_heap_len -= 1U;
    if (index != shard->processor.timer_heap_len) {
        last = shard->processor.timer_heap[shard->processor.timer_heap_len];
        shard->processor.timer_heap[index] = last;
        last->heap_index = index;
        if (index > 0U &&
            llam_timer_heap_less(last, shard->processor.timer_heap[llam_timer_heap_parent(index)])) {
            llam_timer_heap_sift_up(shard, index);
        } else {
            llam_timer_heap_sift_down(shard, index);
        }
    }
    shard->processor.timer_heap[shard->processor.timer_heap_len] = NULL;
    node->heap_index = 0U;
    llam_timer_heap_refresh_root(shard);
    llam_timer_count_decrement(shard);
    return node;
}

llam_timer_node_t *llam_timer_heap_pop_min_locked(llam_shard_t *shard) {
    return llam_timer_heap_remove_at_locked(shard, 0U);
}
