/**
 * @file src/core/broker/broker_task.c
 * @brief Broker-owned predefined task command routing.
 *
 * @details
 * The broker task path deliberately does not accept client-provided function
 * pointers. Clients request one of the broker's predefined commands and receive
 * a serialized join capability. This keeps executable authority inside the
 * broker process while still letting untrusted clients drive safe work through
 * the shared submission/completion ring.
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
#include "runtime_broker.h"

#include <string.h>

static uint64_t llam_broker_popcount_u64(uint64_t value) {
    uint64_t count = 0U;

    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static bool llam_broker_task_kind_valid(uint32_t kind) {
    switch ((llam_broker_task_kind_t)kind) {
    case LLAM_BROKER_TASK_KIND_RETURN_U64:
    case LLAM_BROKER_TASK_KIND_INCREMENT_U64:
    case LLAM_BROKER_TASK_KIND_POPCOUNT_U64:
    case LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64:
        return true;
    default:
        return false;
    }
}

static llam_broker_task_slot_t *llam_broker_find_task(llam_broker_t *broker,
                                                      const llam_capability_token_t *token,
                                                      uint64_t required_rights) {
    size_t i;

    if (llam_broker_validate_token_family_unlocked(broker,
                                                   token,
                                                   LLAM_BROKER_CAP_FAMILY_TASK,
                                                   required_rights) != 0) {
        return NULL;
    }
    for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        llam_broker_task_slot_t *slot = &broker->tasks[i];

        if (slot->active &&
            slot->id == token->slot &&
            slot->generation == token->generation) {
            if (LLAM_UNLIKELY((slot->rights & required_rights) != required_rights)) {
                errno = EACCES;
                return NULL;
            }
            return slot;
        }
    }
    errno = EACCES;
    return NULL;
}

static void llam_broker_task_slot_reset(llam_broker_task_slot_t *slot) {
    if (slot == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    atomic_init(&slot->state, LLAM_BROKER_TASK_STATE_EMPTY);
}

static bool llam_broker_has_pending_sleep_task_locked(const llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return false;
    }
    for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        const llam_broker_task_slot_t *slot = &broker->tasks[i];
        uint32_t state;

        if (!slot->active ||
            slot->kind != LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64) {
            continue;
        }
        state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state == LLAM_BROKER_TASK_STATE_SPAWNED ||
            state == LLAM_BROKER_TASK_STATE_DETACHED) {
            return true;
        }
    }
    return false;
}

static void llam_broker_task_compute(uint32_t kind,
                                     uint64_t arg0,
                                     uint64_t *out_result0,
                                     int *out_error_code) {
    uint64_t result0 = 0U;
    int error_code = 0;

    switch ((llam_broker_task_kind_t)kind) {
    case LLAM_BROKER_TASK_KIND_RETURN_U64:
        result0 = arg0;
        break;
    case LLAM_BROKER_TASK_KIND_INCREMENT_U64:
        result0 = arg0 + 1U;
        break;
    case LLAM_BROKER_TASK_KIND_POPCOUNT_U64:
        result0 = llam_broker_popcount_u64(arg0);
        break;
    case LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64:
        if (llam_sleep_ns(arg0) == 0) {
            result0 = arg0;
        } else {
            error_code = errno == 0 ? EINVAL : errno;
        }
        break;
    default:
        error_code = EINVAL;
        break;
    }
    if (out_result0 != NULL) {
        *out_result0 = result0;
    }
    if (out_error_code != NULL) {
        *out_error_code = error_code;
    }
}

static void llam_broker_task_trampoline(void *arg) {
    llam_broker_task_slot_t *slot = (llam_broker_task_slot_t *)arg;
    uint32_t expected;

    if (slot == NULL) {
        return;
    }
    llam_broker_task_compute(slot->kind, slot->arg0, &slot->result0, &slot->error_code);

    expected = LLAM_BROKER_TASK_STATE_SPAWNED;
    if (atomic_compare_exchange_strong_explicit(&slot->state,
                                                &expected,
                                                LLAM_BROKER_TASK_STATE_COMPLETED,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        return;
    }
    if (expected == LLAM_BROKER_TASK_STATE_DETACHED) {
        llam_broker_t *owner = slot->owner;
        llam_task_t *task = slot->task;

        /*
         * A failed transport response may have revoked the client-visible task
         * token while this command was already runnable. Detach from the task
         * itself instead of from the transport thread so final reclamation
         * happens only after the fiber has returned to the scheduler.
         */
        if (task != NULL) {
            (void)llam_detach(task);
        }

        /* Detached task slots remain private until the command actually
         * completes, preventing slot reuse while the runtime still holds this
         * address as the trampoline argument. */
        if (llam_broker_lock(owner) != 0) {
            return;
        }
        llam_broker_task_slot_reset(slot);
        llam_broker_unlock(owner);
    }
}

void llam_broker_clear_tasks(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }
    if (broker->runtime != NULL) {
        /* Broker tasks are predefined nonblocking commands; drain them before
         * invalidating the slot storage used as trampoline arguments. */
        (void)llam_runtime_run_handle(broker->runtime);
    }
    for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        llam_broker_task_slot_t *slot = &broker->tasks[i];

        if (slot->task != NULL) {
            if (atomic_load_explicit(&slot->state, memory_order_acquire) ==
                LLAM_BROKER_TASK_STATE_COMPLETED) {
                (void)llam_join(slot->task);
            } else {
                (void)llam_detach(slot->task);
            }
        }
        llam_broker_task_slot_reset(slot);
    }
}

int llam_broker_spawn_task(llam_broker_t *broker,
                           uint32_t kind,
                           uint64_t arg0,
                           uint64_t rights,
                           llam_capability_token_t *out_token) {
    llam_broker_task_slot_t *slot = NULL;
    size_t i;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      out_token == NULL ||
                      rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_TASK, rights) != 0) {
        return -1;
    }
    if (LLAM_UNLIKELY(!llam_broker_task_kind_valid(kind))) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        if (!broker->tasks[i].active) {
            slot = &broker->tasks[i];
            break;
        }
    }
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = ENOSPC;
        return -1;
    }
    if (llam_broker_validate_next_object_id(broker->next_task_id) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }

    memset(slot, 0, sizeof(*slot));
    slot->owner = broker;
    atomic_init(&slot->state, LLAM_BROKER_TASK_STATE_SPAWNED);
    slot->kind = kind;
    slot->arg0 = arg0;
    slot->id = broker->next_task_id++;
    slot->generation = 1U;
    slot->rights = rights;
    slot->active = true;
    llam_broker_unlock(broker);
    if (llam_broker_issue_object_cap(broker,
                                     LLAM_BROKER_CAP_FAMILY_TASK,
                                     slot->id,
                                     slot->generation,
                                     rights,
                                     out_token) != 0) {
        if (llam_broker_lock(broker) == 0) {
            llam_broker_task_slot_reset(slot);
            llam_broker_unlock(broker);
        }
        llam_broker_end_op(broker);
        return -1;
    }

    if (kind != (uint32_t)LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64) {
        llam_broker_task_compute(kind, arg0, &slot->result0, &slot->error_code);
        atomic_store_explicit(&slot->state, LLAM_BROKER_TASK_STATE_COMPLETED, memory_order_release);
        llam_broker_end_op(broker);
        return 0;
    }

    slot->task = llam_runtime_spawn_ex(broker->runtime, llam_broker_task_trampoline, slot, NULL, 0U);
    if (slot->task == NULL) {
        memset(out_token, 0, sizeof(*out_token));
        if (llam_broker_lock(broker) == 0) {
            llam_broker_task_slot_reset(slot);
            llam_broker_unlock(broker);
        }
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_task_join_runtime_drive_allowed(llam_broker_t *broker,
                                                const llam_capability_token_t *token,
                                                bool *out_allowed) {
    llam_broker_task_slot_t *slot;
    uint32_t state;

    if (out_allowed != NULL) {
        *out_allowed = false;
    }
    if (LLAM_UNLIKELY(out_allowed == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    slot = llam_broker_find_task(broker, token, LLAM_CAP_RIGHT_JOIN);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    state = atomic_load_explicit(&slot->state, memory_order_acquire);
    /*
     * The byte-stream transport is a broker control-plane request handler. It
     * may opportunistically drive short predefined commands so spawn+join smoke
     * paths remain one round-trip, but runtime_run_handle drains all broker
     * work, not just the requested task. If any live sleep command is pending,
     * even joining an unrelated quick task would wait out a client-selected
     * duration. Ring clients already receive EAGAIN for pending joins and can
     * retry; keep the wire transport equivalent whenever drain is unsafe.
     */
    *out_allowed = state != LLAM_BROKER_TASK_STATE_SPAWNED ||
                   (slot->kind != LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64 &&
                    !llam_broker_has_pending_sleep_task_locked(broker));
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_join_task(llam_broker_t *broker,
                          const llam_capability_token_t *token,
                          uint64_t *out_result0) {
    llam_broker_task_slot_t *slot;
    llam_task_t *task;
    uint64_t result0;
    int task_error;
    uint32_t state;

    if (LLAM_UNLIKELY(out_result0 == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Join publishes task output. Clear it before every validation step so a
     * failed join cannot leave a stale successful result in FFI-visible memory.
     */
    *out_result0 = 0U;
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    slot = llam_broker_find_task(broker, token, LLAM_CAP_RIGHT_JOIN);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }

    state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (state == LLAM_BROKER_TASK_STATE_SPAWNED) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EAGAIN;
        return -1;
    }
    if (state != LLAM_BROKER_TASK_STATE_COMPLETED) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    task = slot->task;
    result0 = slot->result0;
    task_error = slot->error_code;
    slot->task = NULL;
    slot->active = false;
    atomic_store_explicit(&slot->state, LLAM_BROKER_TASK_STATE_JOINED, memory_order_release);
    llam_broker_task_slot_reset(slot);
    llam_broker_unlock(broker);

    if (task != NULL && llam_join(task) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }

    if (task_error != 0) {
        errno = task_error;
        llam_broker_end_op(broker);
        return -1;
    }
    *out_result0 = result0;
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_detach_task(llam_broker_t *broker, const llam_capability_token_t *token) {
    llam_broker_task_slot_t *slot;
    llam_task_t *task = NULL;
    uint32_t state;

    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    slot = llam_broker_find_task(broker, token, LLAM_CAP_RIGHT_DETACH);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }

    for (;;) {
        state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state == LLAM_BROKER_TASK_STATE_COMPLETED) {
            task = slot->task;
            slot->task = NULL;
            llam_broker_task_slot_reset(slot);
            break;
        }
        if (state == LLAM_BROKER_TASK_STATE_SPAWNED) {
            uint32_t expected = LLAM_BROKER_TASK_STATE_SPAWNED;

            if (slot->task == NULL) {
                llam_broker_unlock(broker);
                llam_broker_end_op(broker);
                errno = EINVAL;
                return -1;
            }
            /*
             * Task completion publishes COMPLETED without taking the broker
             * lock. Claim SPAWNED with a CAS so detach cannot overwrite a
             * concurrently completed task with DETACHED after the trampoline
             * has already returned. For live tasks, keep slot->task intact; the
             * trampoline observes DETACHED, detaches the LLAM task handle, and
             * resets the broker slot once it is no longer used as the argument.
             */
            if (!atomic_compare_exchange_weak_explicit(&slot->state,
                                                       &expected,
                                                       LLAM_BROKER_TASK_STATE_DETACHED,
                                                       memory_order_acq_rel,
                                                       memory_order_acquire)) {
                continue;
            }
            slot->rights = 0U;
            break;
        }
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    llam_broker_unlock(broker);

    if (task != NULL) {
        if (llam_detach(task) != 0) {
            llam_broker_end_op(broker);
            return -1;
        }
    }
    llam_broker_end_op(broker);
    return 0;
}
