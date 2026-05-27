/**
 * @file src/core/runtime_channel_select.c
 * @brief Public channel multiplexing API.
 *
 * @details
 * Channel select parks the current task on every requested channel queue and
 * lets the first matching channel operation complete the shared select state.
 * Stale wait nodes are skipped by normal channel send/receive paths, so one
 * selected operation wakes the task exactly once while the resumed task removes
 * the remaining nodes from their queues.
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

static unsigned llam_channel_select_completed_state(llam_channel_select_state_t *state) {
    unsigned completed;

    do {
        completed = atomic_load_explicit(&state->completed, memory_order_acquire);
    } while (completed == LLAM_SELECT_COMPLETING);

    return completed;
}

static void llam_channel_select_sort_channels(llam_channel_t **channels, size_t count) {
    size_t i;

    for (i = 1U; i < count; ++i) {
        llam_channel_t *channel = channels[i];
        size_t j = i;

        while (j > 0U && (uintptr_t)channels[j - 1U] > (uintptr_t)channel) {
            channels[j] = channels[j - 1U];
            --j;
        }
        channels[j] = channel;
    }
}

static void llam_channel_select_release_channels(llam_channel_t **channels, size_t count) {
    while (count > 0U) {
        --count;
        llam_channel_end_public_op(channels[count]);
    }
}

static int llam_channel_select_collect_channels(llam_select_op_t *ops,
                                                size_t op_count,
                                                llam_channel_t **channels,
                                                llam_channel_t **op_channels,
                                                size_t *out_count) {
    size_t count = 0U;
    size_t i;

    for (i = 0U; i < op_count; ++i) {
        llam_channel_t *channel = llam_channel_resolve_public_handle(ops[i].channel);
        size_t j;
        bool found = false;

        if (channel == NULL) {
            llam_channel_select_release_channels(channels, count);
            errno = EINVAL;
            return -1;
        }
        if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
            llam_channel_end_public_op(channel);
            llam_channel_select_release_channels(channels, count);
            return -1;
        }
        for (j = 0U; j < count; ++j) {
            if (channels[j] == channel) {
                found = true;
                break;
            }
        }
        if (!found) {
            channels[count++] = channel;
            op_channels[i] = channel;
        } else {
            op_channels[i] = channels[j];
            llam_channel_end_public_op(channel);
        }
    }
    llam_channel_select_sort_channels(channels, count);
    *out_count = count;
    return 0;
}

static void llam_channel_select_lock_channels(llam_channel_t **channels, size_t count) {
    size_t i;

    /*
     * All participating channels are locked in address order.  Select may park
     * one task on multiple channel queues, so stable lock ordering is required
     * to avoid deadlocks when another select touches the same channel set.
     */
    for (i = 0U; i < count; ++i) {
        pthread_mutex_lock(&channels[i]->lock);
    }
}

static void llam_channel_select_unlock_channels(llam_channel_t **channels, size_t count) {
    while (count > 0U) {
        --count;
        pthread_mutex_unlock(&channels[count]->lock);
    }
}

static bool llam_channel_select_op_may_run_locked(const llam_channel_select_state_t *state, size_t op_index) {
    const llam_select_op_t *op;
    llam_channel_t *channel;

    if (state == NULL || state->op_channels == NULL || op_index >= state->op_count) {
        return false;
    }
    op = &state->ops[op_index];
    channel = state->op_channels[op_index];

    if (op->kind == LLAM_SELECT_OP_RECV) {
        return channel->count > 0U || channel->send_waiters.head != NULL || channel->closed;
    }
    if (op->kind == LLAM_SELECT_OP_SEND) {
        return channel->closed || channel->recv_waiters.head != NULL || channel->count < channel->capacity;
    }
    return false;
}

static bool llam_channel_select_any_ready_locked(const llam_channel_select_state_t *state) {
    size_t i;

    for (i = 0U; i < state->op_count; ++i) {
        if (llam_channel_select_op_may_run_locked(state, i)) {
            return true;
        }
    }
    return false;
}

static void llam_channel_select_cleanup_nodes(llam_channel_select_state_t *state) {
    size_t i;

    if (state == NULL || state->nodes == NULL || state->ops == NULL) {
        return;
    }
    if (state->channels != NULL && state->channel_count > 0U) {
        llam_channel_select_lock_channels(state->channels, state->channel_count);
        for (i = 0U; i < state->op_count; ++i) {
            llam_wait_node_t *node = state->nodes[i];
            llam_channel_t *channel = state->op_channels != NULL ? state->op_channels[i] : NULL;

            if (node == NULL || channel == NULL) {
                continue;
            }
            if (state->ops[i].kind == LLAM_SELECT_OP_RECV) {
                (void)llam_wait_queue_remove(&channel->recv_waiters, node);
            } else if (state->ops[i].kind == LLAM_SELECT_OP_SEND) {
                (void)llam_wait_queue_remove(&channel->send_waiters, node);
            }
        }
        llam_channel_select_unlock_channels(state->channels, state->channel_count);
        return;
    }
    for (i = 0U; i < state->op_count; ++i) {
        llam_wait_node_t *node = state->nodes[i];
        llam_channel_t *channel = state->op_channels != NULL ? state->op_channels[i] : NULL;

        if (node == NULL || channel == NULL) {
            continue;
        }
        pthread_mutex_lock(&channel->lock);
        if (state->ops[i].kind == LLAM_SELECT_OP_RECV) {
            (void)llam_wait_queue_remove(&channel->recv_waiters, node);
        } else if (state->ops[i].kind == LLAM_SELECT_OP_SEND) {
            (void)llam_wait_queue_remove(&channel->send_waiters, node);
        }
        pthread_mutex_unlock(&channel->lock);
    }
}

static void llam_channel_select_release_nodes(llam_shard_t *shard,
                                              llam_task_t *task,
                                              llam_wait_node_t **nodes,
                                              size_t op_count) {
    size_t i;

    if (nodes == NULL) {
        return;
    }
    for (i = 0U; i < op_count; ++i) {
        size_t embedded_index;

        if (nodes[i] == NULL) {
            continue;
        }
        if (task != NULL && nodes[i] == &task->embedded_wait_node) {
            llam_wait_node_reset(nodes[i], task->owner_runtime, UINT_MAX);
            continue;
        }
        if (task != NULL) {
            for (embedded_index = 0U; embedded_index < LLAM_TASK_EMBEDDED_SELECT_NODES; ++embedded_index) {
                if (nodes[i] == &task->embedded_select_nodes[embedded_index]) {
                    llam_wait_node_reset(nodes[i], task->owner_runtime, UINT_MAX);
                    break;
                }
            }
            if (embedded_index < LLAM_TASK_EMBEDDED_SELECT_NODES) {
                continue;
            }
        }
        llam_wait_node_free(shard, nodes[i]);
    }
}

static int llam_channel_select_alloc_nodes(llam_shard_t *shard,
                                           llam_task_t *task,
                                           llam_select_op_t *ops,
                                           size_t op_count,
                                           llam_channel_select_state_t *state) {
    size_t i;

    for (i = 0U; i < op_count; ++i) {
        llam_wait_node_t *node;

        if (task != NULL &&
            task->active_wait_node == NULL &&
            task->active_select_state == NULL &&
            i < LLAM_TASK_EMBEDDED_SELECT_NODES) {
            node = &task->embedded_select_nodes[i];
            llam_wait_node_reset(node, task->owner_runtime, UINT_MAX);
        } else {
            node = llam_wait_node_alloc(shard);
        }

        if (node == NULL) {
            llam_channel_select_release_nodes(shard, task, state->nodes, i);
            return -1;
        }
        node->task = task;
        node->owner_shard = shard->id;
        node->select_state = state;
        node->select_kind = ops[i].kind;
        node->scalar_value = (intptr_t)i;
        node->value = ops[i].send_value;
        node->error_code = 0;
        state->nodes[i] = node;
    }
    return 0;
}

static void llam_channel_select_enqueue_nodes_locked(llam_channel_select_state_t *state) {
    size_t i;

    for (i = 0U; i < state->op_count; ++i) {
        llam_wait_node_t *node = state->nodes[i];
        llam_channel_t *channel = state->op_channels[i];

        if (state->ops[i].kind == LLAM_SELECT_OP_RECV) {
            llam_wait_queue_push_tail(&channel->recv_waiters, node);
        } else {
            llam_wait_queue_push_tail(&channel->send_waiters, node);
        }
    }
}

static void llam_channel_select_set_task_tracking(llam_task_t *task,
                                                  llam_channel_select_state_t *state,
                                                  unsigned parked_shard,
                                                  llam_wait_reason_t reason) {
    /*
     * Select waits are not I/O waits.  Clear any stale active I/O owner through
     * the centralized exchange helper so dynamic rehome and completion threads
     * cannot race on task->active_io_req.
     */
    llam_task_clear_wait_tracking(task);
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = state;
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = reason;
}

static int llam_channel_select_finish(llam_channel_select_state_t *state, size_t *selected_index) {
    if (state->selected_index == SIZE_MAX) {
        errno = state->error_code != 0 ? state->error_code : ECANCELED;
        return -1;
    }

    *selected_index = state->selected_index;
    state->ops[state->selected_index].result_errno = state->error_code;
    if (state->error_code == 0 &&
        state->ops[state->selected_index].kind == LLAM_SELECT_OP_RECV &&
        state->ops[state->selected_index].recv_out != NULL) {
        *state->ops[state->selected_index].recv_out = state->selected_value;
    }
    return 0;
}

bool llam_channel_select_complete_node(llam_wait_node_t *node, void *value, int error_code) {
    llam_channel_select_state_t *state;
    unsigned expected = LLAM_SELECT_PENDING;
    size_t selected;
    bool should_queue;

    if (node == NULL || node->select_state == NULL) {
        return false;
    }

    state = node->select_state;
    if (!atomic_compare_exchange_strong_explicit(&state->completed,
                                                 &expected,
                                                 LLAM_SELECT_COMPLETING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }

    selected = (size_t)node->scalar_value;
    state->selected_index = selected;
    state->selected_value = value;
    state->error_code = error_code;
    node->value = value;
    node->error_code = error_code;
    should_queue = atomic_load_explicit(&state->wake_armed, memory_order_acquire) != 0U &&
                   node->task != NULL &&
                   node->task->state == LLAM_TASK_STATE_PARKED;
    if (should_queue) {
        atomic_store_explicit(&state->wake_queued, 1U, memory_order_release);
    }
    /*
     * Select can be completed after queue insertion but before the task commits
     * to a context switch.  Inline completion means the waiter must not be
     * queued; queued completion means the waiter must park once to consume the
     * already-published wake.
     */
    atomic_store_explicit(&state->completed,
                          should_queue ? LLAM_SELECT_COMPLETED_QUEUED : LLAM_SELECT_COMPLETED_INLINE,
                          memory_order_release);
    return true;
}

bool llam_channel_select_node_should_wake(llam_wait_node_t *node) {
    llam_channel_select_state_t *state;

    if (node == NULL || node->task == NULL || node->select_state == NULL) {
        return false;
    }
    state = node->select_state;
    if (llam_channel_select_completed_state(state) != LLAM_SELECT_COMPLETED_QUEUED) {
        return false;
    }
    return atomic_load_explicit(&state->wake_queued, memory_order_acquire) != 0U &&
           node->task->state == LLAM_TASK_STATE_PARKED;
}

bool llam_channel_select_abort_task_wait(llam_task_t *task, int error_code, llam_wait_reason_t reason) {
    llam_channel_select_state_t *state;
    unsigned expected = LLAM_SELECT_PENDING;
    bool should_queue;

    if (task == NULL || task->active_select_state == NULL) {
        return false;
    }
    state = task->active_select_state;
    if (!atomic_compare_exchange_strong_explicit(&state->completed,
                                                 &expected,
                                                 LLAM_SELECT_COMPLETING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }

    state->selected_index = SIZE_MAX;
    state->selected_value = NULL;
    state->error_code = error_code;
    llam_channel_select_cleanup_nodes(state);
    task->wake_error_code = error_code;
    should_queue = atomic_load_explicit(&state->wake_armed, memory_order_acquire) != 0U &&
                   task->state == LLAM_TASK_STATE_PARKED;
    if (should_queue) {
        atomic_store_explicit(&state->wake_queued, 1U, memory_order_release);
    }
    atomic_store_explicit(&state->completed,
                          should_queue ? LLAM_SELECT_COMPLETED_QUEUED : LLAM_SELECT_COMPLETED_INLINE,
                          memory_order_release);
    if (should_queue) {
        llam_runtime_t *rt = task->owner_runtime;

        if (rt != NULL) {
            llam_reinject_task_on_shard(rt,
                                      task,
                                      task->parked_shard,
                                      true,
                                      LLAM_TRACE_WAKE,
                                      reason);
        }
    }
    return true;
}

int llam_channel_select(llam_select_op_t *ops,
                        size_t op_count,
                        uint64_t deadline_ns,
                        size_t *selected_index) {
    llam_channel_select_state_t state;
    llam_channel_t **channels;
    llam_wait_node_t **nodes;
    llam_channel_t *inline_channels[LLAM_CHANNEL_SELECT_INLINE_OPS];
    llam_channel_t *inline_op_channels[LLAM_CHANNEL_SELECT_INLINE_OPS];
    llam_wait_node_t *inline_nodes[LLAM_CHANNEL_SELECT_INLINE_OPS];
    llam_channel_t **op_channels;
    llam_task_t *task;
    llam_shard_t *shard;
    size_t channel_count;
    size_t start = 0U;
    size_t i;
    bool heap_arrays;
    int rc;

    if (ops == NULL || op_count == 0U || op_count > (size_t)LLAM_CHANNEL_SELECT_MAX_OPS || selected_index == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* Clear stale per-op status before any bounded-array failure path. */
    for (i = 0U; i < op_count; ++i) {
        ops[i].result_errno = 0;
    }
    if (llam_require_task_context() != 0) {
        return -1;
    }
    for (i = 0U; i < op_count; ++i) {
        if (llam_channel_select_validate_op(&ops[i]) != 0) {
            return -1;
        }
    }

    task = g_llam_tls_task;
    shard = g_llam_tls_shard;

    for (;;) {
        if (op_count <= LLAM_CHANNEL_SELECT_INLINE_OPS) {
            int selected = llam_channel_select_try_ready_batch(ops, op_count, start, selected_index);

            if (selected < 0) {
                return -1;
            }
            if (selected == LLAM_SELECT_TRY_SELECTED) {
                return 0;
            }
            if (selected == LLAM_SELECT_TRY_NOT_READY) {
                goto select_not_ready;
            }
            /*
             * Fallback means a queued peer waiter is involved.  Preserve the
             * existing public-operation path for that uncommon case.
             */
        } else {
            int selected = llam_channel_select_try_ready_large(ops, op_count, start, selected_index);

            if (selected < 0) {
                return -1;
            }
            if (selected == LLAM_SELECT_TRY_SELECTED) {
                return 0;
            }
            if (selected == LLAM_SELECT_TRY_NOT_READY) {
                goto select_not_ready;
            }
        }
        if (start == 0U) {
            for (i = 0U; i < op_count; ++i) {
                int selected = llam_channel_select_try_one_fast(&ops[i]);

                if (selected < 0) {
                    return -1;
                }
                if (selected == LLAM_SELECT_TRY_FALLBACK) {
                    selected = llam_channel_select_try_one(&ops[i]);
                    if (selected < 0) {
                        return -1;
                    }
                }
                if (selected > 0) {
                    *selected_index = i;
                    return 0;
                }
            }
        } else {
            for (i = 0U; i < op_count; ++i) {
                size_t index = start + i;
                int selected;

                if (index >= op_count) {
                    index -= op_count;
                }
                selected = llam_channel_select_try_one_fast(&ops[index]);
                if (selected < 0) {
                    return -1;
                }
                if (selected == LLAM_SELECT_TRY_FALLBACK) {
                    selected = llam_channel_select_try_one(&ops[index]);
                    if (selected < 0) {
                        return -1;
                    }
                }
                if (selected > 0) {
                    *selected_index = index;
                    return 0;
                }
            }
        }
select_not_ready:
        if (deadline_ns == 0U || llam_deadline_passed(deadline_ns)) {
            errno = ETIMEDOUT;
            return -1;
        }

        heap_arrays = op_count > LLAM_CHANNEL_SELECT_INLINE_OPS;
        if (heap_arrays) {
            nodes = calloc(op_count, sizeof(*nodes));
            channels = calloc(op_count, sizeof(*channels));
            op_channels = calloc(op_count, sizeof(*op_channels));
            if (nodes == NULL || channels == NULL || op_channels == NULL) {
                free(nodes);
                free(channels);
                free(op_channels);
                errno = ENOMEM;
                return -1;
            }
        } else {
            memset(inline_nodes, 0, sizeof(inline_nodes));
            nodes = inline_nodes;
            channels = inline_channels;
            op_channels = inline_op_channels;
        }

        memset(&state, 0, sizeof(state));
        state.owner_runtime = task->owner_runtime;
        state.ops = ops;
        state.nodes = nodes;
        state.op_channels = op_channels;
        state.op_count = op_count;
        state.selected_index = SIZE_MAX;
        atomic_init(&state.completed, LLAM_SELECT_PENDING);
        atomic_init(&state.wake_armed, 0U);
        atomic_init(&state.wake_queued, 0U);

        if (llam_channel_select_collect_channels(ops, op_count, channels, op_channels, &channel_count) != 0) {
            if (heap_arrays) {
                free(nodes);
                free(channels);
                free(op_channels);
            }
            return -1;
        }
        state.channels = channels;
        state.channel_count = channel_count;
        if (llam_channel_select_alloc_nodes(shard, task, ops, op_count, &state) != 0) {
            llam_channel_select_release_channels(channels, channel_count);
            if (heap_arrays) {
                free(nodes);
                free(channels);
                free(op_channels);
            }
            return -1;
        }

        llam_channel_select_lock_channels(channels, channel_count);
        if (llam_channel_select_any_ready_locked(&state)) {
            llam_channel_select_unlock_channels(channels, channel_count);
            llam_channel_select_release_nodes(shard, task, nodes, op_count);
            llam_channel_select_release_channels(channels, channel_count);
            if (heap_arrays) {
                free(nodes);
                free(channels);
                free(op_channels);
            }
            start = (start + 1U) % op_count;
            continue;
        }
        llam_channel_select_enqueue_nodes_locked(&state);
        llam_channel_select_unlock_channels(channels, channel_count);

        llam_task_ensure_listed(task);
        llam_channel_select_set_task_tracking(task,
                                             &state,
                                             shard->id,
                                             ops[0].kind == LLAM_SELECT_OP_SEND ? LLAM_WAIT_CHANNEL_SEND : LLAM_WAIT_CHANNEL_RECV);
        if (deadline_ns != UINT64_MAX && llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
            if (llam_channel_select_completed_state(&state) != LLAM_SELECT_PENDING) {
                goto select_ready;
            }
            llam_channel_select_cleanup_nodes(&state);
            if (llam_channel_select_completed_state(&state) != LLAM_SELECT_PENDING) {
                /*
                 * A peer can pop and complete a select node after the first
                 * pending check but before cleanup holds every channel lock.
                 * Completion has already transferred value/error ownership, so
                 * consume it instead of reporting the setup failure.
                 */
                goto select_ready;
            }
            task->state = LLAM_TASK_STATE_RUNNING;
            task->wait_reason = LLAM_WAIT_NONE;
            llam_task_clear_wait_tracking(task);
            llam_channel_select_release_nodes(shard, task, nodes, op_count);
            llam_channel_select_release_channels(channels, channel_count);
            if (heap_arrays) {
                free(nodes);
                free(channels);
                free(op_channels);
            }
            return -1;
        }
        if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
            if (llam_channel_select_completed_state(&state) != LLAM_SELECT_PENDING) {
                goto select_ready;
            }
            llam_disarm_task_wait_deadline(task);
            llam_channel_select_cleanup_nodes(&state);
            if (llam_channel_select_completed_state(&state) != LLAM_SELECT_PENDING) {
                /*
                 * Cancellation setup lost a race with a peer operation that had
                 * already popped one of our nodes.  The selected operation wins
                 * over the cancellation setup error.
                 */
                goto select_ready;
            }
            task->state = LLAM_TASK_STATE_RUNNING;
            task->wait_reason = LLAM_WAIT_NONE;
            llam_task_clear_wait_tracking(task);
            llam_channel_select_release_nodes(shard, task, nodes, op_count);
            llam_channel_select_release_channels(channels, channel_count);
            if (heap_arrays) {
                free(nodes);
                free(channels);
                free(op_channels);
            }
            errno = ECANCELED;
            return -1;
        }

select_ready:
        {
            unsigned completed = llam_channel_select_completed_state(&state);

            if (completed == LLAM_SELECT_PENDING) {
                atomic_store_explicit(&state.wake_armed, 1U, memory_order_release);
                completed = llam_channel_select_completed_state(&state);
            }
            if (completed == LLAM_SELECT_PENDING || completed == LLAM_SELECT_COMPLETED_QUEUED) {
                llam_park_current_task(task->wait_reason, LLAM_TRACE_STATE);
            } else {
                task->state = LLAM_TASK_STATE_RUNNING;
                task->wait_reason = LLAM_WAIT_NONE;
            }
        }
        if (deadline_ns != UINT64_MAX) {
            // Select completion may happen before the waiter fully commits to
            // parking; remove any deadline not observed by the wake path.
            llam_disarm_task_wait_deadline(task);
        }
        llam_cancel_token_unregister_task(task);
        llam_task_clear_wait_tracking(task);
        llam_channel_select_cleanup_nodes(&state);
        rc = llam_channel_select_finish(&state, selected_index);
        if (state.selected_index != SIZE_MAX && state.selected_index < op_count) {
            llam_channel_waiter_consumed(state.op_channels[state.selected_index]);
        }
        llam_channel_select_release_nodes(shard, task, nodes, op_count);
        llam_channel_select_release_channels(channels, channel_count);
        if (heap_arrays) {
            free(nodes);
            free(channels);
            free(op_channels);
        }
        return rc;
    }
}
