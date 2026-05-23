/**
 * @file src/core/runtime_timer_heap.c
 * @brief 4-ary timer heap primitives.
 *
 * @details
 * Timer heap mechanics are isolated from wait/cancel dispatch so timer policy
 * changes do not grow the deadline-expiration state machine.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_internal.h"

#define LLAM_TIMER_HEAP_ARITY 4U

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
    shard->timers = shard->timer_heap_len > 0U ? shard->timer_heap[0] : NULL;
}

static void llam_timer_heap_swap(llam_shard_t *shard, size_t lhs, size_t rhs) {
    llam_timer_node_t *tmp = shard->timer_heap[lhs];

    shard->timer_heap[lhs] = shard->timer_heap[rhs];
    shard->timer_heap[rhs] = tmp;
    shard->timer_heap[lhs]->heap_index = lhs;
    shard->timer_heap[rhs]->heap_index = rhs;
}

static bool llam_timer_heap_reserve(llam_shard_t *shard, size_t needed) {
    llam_timer_node_t **items;
    size_t max_items = SIZE_MAX / sizeof(*items);
    size_t new_cap;

    if (needed > max_items || shard->timer_heap_cap > max_items) {
        errno = ENOMEM;
        return false;
    }
    if (needed <= shard->timer_heap_cap) {
        return true;
    }
    if (shard->timer_heap_cap == 0U) {
        new_cap = 64U;
    } else if (shard->timer_heap_cap > max_items / 2U) {
        new_cap = needed;
    } else {
        new_cap = shard->timer_heap_cap * 2U;
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
    items = realloc(shard->timer_heap, new_cap * sizeof(*items));
    if (items == NULL) {
        errno = ENOMEM;
        return false;
    }
    shard->timer_heap = items;
    shard->timer_heap_cap = new_cap;
    return true;
}

static void llam_timer_heap_sift_up(llam_shard_t *shard, size_t index) {
    while (index > 0U) {
        size_t parent = llam_timer_heap_parent(index);

        if (!llam_timer_heap_less(shard->timer_heap[index], shard->timer_heap[parent])) {
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
             child < shard->timer_heap_len && child < first_child + LLAM_TIMER_HEAP_ARITY;
             ++child) {
            if (llam_timer_heap_less(shard->timer_heap[child], shard->timer_heap[best])) {
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

bool llam_timer_heap_push_locked(llam_shard_t *shard, llam_timer_node_t *node) {
    size_t index;
    size_t max_items = SIZE_MAX / sizeof(*shard->timer_heap);

    if (shard->timer_heap_len >= max_items) {
        errno = ENOMEM;
        return false;
    }
    if (!llam_timer_heap_reserve(shard, shard->timer_heap_len + 1U)) {
        return false;
    }
    index = shard->timer_heap_len++;
    shard->timer_heap[index] = node;
    node->heap_index = index;
    llam_timer_heap_sift_up(shard, index);
    llam_timer_heap_refresh_root(shard);
    atomic_fetch_add_explicit(&shard->timer_count, 1U, memory_order_release);
    return true;
}

llam_timer_node_t *llam_timer_heap_remove_at_locked(llam_shard_t *shard, size_t index) {
    llam_timer_node_t *node;
    llam_timer_node_t *last;

    if (index >= shard->timer_heap_len) {
        return NULL;
    }

    node = shard->timer_heap[index];
    shard->timer_heap_len -= 1U;
    if (index != shard->timer_heap_len) {
        last = shard->timer_heap[shard->timer_heap_len];
        shard->timer_heap[index] = last;
        last->heap_index = index;
        if (index > 0U &&
            llam_timer_heap_less(last, shard->timer_heap[llam_timer_heap_parent(index)])) {
            llam_timer_heap_sift_up(shard, index);
        } else {
            llam_timer_heap_sift_down(shard, index);
        }
    }
    shard->timer_heap[shard->timer_heap_len] = NULL;
    node->heap_index = 0U;
    llam_timer_heap_refresh_root(shard);
    atomic_fetch_sub_explicit(&shard->timer_count, 1U, memory_order_release);
    return node;
}

llam_timer_node_t *llam_timer_heap_pop_min_locked(llam_shard_t *shard) {
    return llam_timer_heap_remove_at_locked(shard, 0U);
}
