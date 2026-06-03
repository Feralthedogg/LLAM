/**
 * @file src/core/runtime_broker_buffer.c
 * @brief Broker-owned memory buffer capability data plane.
 *
 * @details
 * Buffer bytes live only in the trusted broker address space. Clients receive
 * MAC-protected tokens and request bounded reads/writes through this module;
 * raw buffer pointers are never exported across the broker boundary.
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

#include <stdlib.h>
#include <string.h>

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint_fast64_t g_llam_broker_test_buffer_free_count;

uint64_t llam_broker_test_buffer_free_count(void) {
    return atomic_load_explicit(&g_llam_broker_test_buffer_free_count, memory_order_relaxed);
}

void llam_broker_test_buffer_free_count_reset(void) {
    atomic_store_explicit(&g_llam_broker_test_buffer_free_count, 0U, memory_order_relaxed);
}
#endif

static void llam_broker_buffer_free_data(unsigned char *data) {
    if (data == NULL) {
        return;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    atomic_fetch_add_explicit(&g_llam_broker_test_buffer_free_count, 1U, memory_order_relaxed);
#endif
    free(data);
}

void llam_broker_buffer_slot_reset(llam_broker_buffer_slot_t *slot) {
    if (slot == NULL) {
        return;
    }
    if (slot->data != NULL) {
        llam_broker_buffer_free_data(slot->data);
    }
    memset(slot, 0, sizeof(*slot));
}

void llam_broker_clear_buffers(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
        llam_broker_buffer_slot_reset(&broker->buffers[i]);
    }
}

llam_broker_buffer_slot_t *llam_broker_find_buffer_unlocked(llam_broker_t *broker,
                                                            const llam_capability_token_t *token,
                                                            uint64_t required_rights) {
    size_t i;

    if (llam_broker_validate_token_family_unlocked(broker,
                                                   token,
                                                   LLAM_BROKER_CAP_FAMILY_BUFFER,
                                                   required_rights) != 0) {
        return NULL;
    }
    for (i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
        llam_broker_buffer_slot_t *slot = &broker->buffers[i];

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

int llam_broker_register_buffer(llam_broker_t *broker,
                                const void *initial_data,
                                size_t length,
                                uint64_t rights,
                                llam_capability_token_t *out_token) {
    llam_broker_buffer_slot_t *slot = NULL;
    unsigned char *data;
    size_t i;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      length == 0U ||
                      length > LLAM_BROKER_BUFFER_MAX_BYTES ||
                      rights == 0U ||
                      out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_BUFFER, rights) != 0) {
        return -1;
    }
    data = (unsigned char *)calloc(1U, length);
    if (data == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (initial_data != NULL) {
        memcpy(data, initial_data, length);
    }

    if (llam_broker_begin_op(broker) != 0) {
        free(data);
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        free(data);
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        free(data);
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
        if (!broker->buffers[i].active) {
            slot = &broker->buffers[i];
            break;
        }
    }
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        free(data);
        errno = ENOSPC;
        return -1;
    }
    if (llam_broker_validate_next_object_id(broker->next_buffer_id) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        free(data);
        return -1;
    }

    /*
     * Registration reuses inactive slots. Reclaim any stale broker-owned heap
     * storage first so a partially invalidated slot cannot be overwritten and
     * leaked before normal destroy cleanup sees it.
     */
    llam_broker_buffer_slot_reset(slot);
    slot->data = data;
    slot->length = length;
    slot->id = broker->next_buffer_id++;
    slot->generation = 1U;
    slot->rights = rights;
    slot->active = true;
    if (llam_broker_issue_object_cap_unlocked(broker,
                                              LLAM_BROKER_CAP_FAMILY_BUFFER,
                                              slot->id,
                                              slot->generation,
                                              rights,
                                              out_token) != 0) {
        llam_broker_buffer_slot_reset(slot);
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_read_buffer(llam_broker_t *broker,
                            const llam_capability_token_t *token,
                            uint64_t relative_offset,
                            void *out_data,
                            size_t length) {
    llam_broker_buffer_slot_t *slot;
    uint64_t end;

    if (LLAM_UNLIKELY(out_data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return llam_broker_fail_clear_output(out_data, length, errno);
    }
    if (llam_broker_lock(broker) != 0) {
        int saved_errno = errno;

        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output(out_data, length, saved_errno);
    }
    slot = llam_broker_find_buffer_unlocked(broker, token, LLAM_CAP_RIGHT_READ);
    if (slot == NULL) {
        int saved_errno = errno;

        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output(out_data, length, saved_errno);
    }
    end = relative_offset + (uint64_t)length;
    if (LLAM_UNLIKELY(end < relative_offset || end > (uint64_t)slot->length)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output(out_data, length, EINVAL);
    }
    memcpy(out_data, slot->data + relative_offset, length);
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_write_buffer(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             uint64_t relative_offset,
                             const void *data,
                             size_t length) {
    llam_broker_buffer_slot_t *slot;
    uint64_t end;

    if (LLAM_UNLIKELY(data == NULL || length == 0U)) {
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
    slot = llam_broker_find_buffer_unlocked(broker, token, LLAM_CAP_RIGHT_WRITE);
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    end = relative_offset + (uint64_t)length;
    if (LLAM_UNLIKELY(end < relative_offset || end > (uint64_t)slot->length)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    memcpy(slot->data + relative_offset, data, length);
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}
