/**
 * @file src/core/runtime_broker_ops.c
 * @brief Broker active-operation and per-thread subject tracking.
 *
 * @details
 * The secure broker allows nested internal transport/ring calls while still
 * rejecting new external work during destroy. This file owns the active-op pin
 * counter and per-thread effective subject stack used by bearer-token checks.
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

#include <limits.h>

#define LLAM_BROKER_TLS_OP_DEPTH 8U

typedef struct llam_broker_tls_op {
    llam_broker_t *broker;
    uint32_t depth;
    /*
     * Effective subject per nested op frame. This prevents an inner
     * transport/ring call from leaking subject authority back to an outer
     * bearer-token operation.
     */
    uint64_t subject_stack[LLAM_BROKER_TLS_OP_DEPTH];
} llam_broker_tls_op_t;

/*
 * Accepted transport/ring requests may call public broker helpers internally.
 * Track that per thread so destroy rejects only new external work.
 */
static _Thread_local llam_broker_tls_op_t g_llam_broker_tls_ops[LLAM_BROKER_TLS_OP_DEPTH];

static bool llam_broker_tls_has_op(const llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return false;
    }
    for (i = 0U; i < LLAM_BROKER_TLS_OP_DEPTH; ++i) {
        if (g_llam_broker_tls_ops[i].broker == broker &&
            g_llam_broker_tls_ops[i].depth > 0U) {
            return true;
        }
    }
    return false;
}

static int llam_broker_tls_enter_op(llam_broker_t *broker, uint64_t subject_id) {
    size_t first_empty = LLAM_BROKER_TLS_OP_DEPTH;
    size_t i;

    if (broker == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_TLS_OP_DEPTH; ++i) {
        if (g_llam_broker_tls_ops[i].broker == broker) {
            uint64_t current_subject;
            uint64_t effective_subject;

            if (LLAM_UNLIKELY(g_llam_broker_tls_ops[i].depth >= LLAM_BROKER_TLS_OP_DEPTH)) {
                errno = EOVERFLOW;
                return -1;
            }
            current_subject = g_llam_broker_tls_ops[i].depth > 0U
                ? g_llam_broker_tls_ops[i].subject_stack[g_llam_broker_tls_ops[i].depth - 1U]
                : 0U;
            if (LLAM_UNLIKELY(subject_id != 0U &&
                              current_subject != 0U &&
                              current_subject != subject_id)) {
                errno = EACCES;
                return -1;
            }
            effective_subject = subject_id != 0U ? subject_id : current_subject;
            g_llam_broker_tls_ops[i].subject_stack[g_llam_broker_tls_ops[i].depth] = effective_subject;
            g_llam_broker_tls_ops[i].depth += 1U;
            return 0;
        }
        if (first_empty == LLAM_BROKER_TLS_OP_DEPTH &&
            g_llam_broker_tls_ops[i].broker == NULL) {
            first_empty = i;
        }
    }
    if (first_empty == LLAM_BROKER_TLS_OP_DEPTH) {
        errno = EOVERFLOW;
        return -1;
    }
    g_llam_broker_tls_ops[first_empty].broker = broker;
    g_llam_broker_tls_ops[first_empty].depth = 1U;
    g_llam_broker_tls_ops[first_empty].subject_stack[0] = subject_id;
    return 0;
}

static void llam_broker_tls_leave_op(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_TLS_OP_DEPTH; ++i) {
        if (g_llam_broker_tls_ops[i].broker == broker &&
            g_llam_broker_tls_ops[i].depth > 0U) {
            g_llam_broker_tls_ops[i].depth -= 1U;
            g_llam_broker_tls_ops[i].subject_stack[g_llam_broker_tls_ops[i].depth] = 0U;
            if (g_llam_broker_tls_ops[i].depth == 0U) {
                g_llam_broker_tls_ops[i].broker = NULL;
            }
            return;
        }
    }
}

uint64_t llam_broker_current_subject(const llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return 0U;
    }
    for (i = 0U; i < LLAM_BROKER_TLS_OP_DEPTH; ++i) {
        if (g_llam_broker_tls_ops[i].broker == broker &&
            g_llam_broker_tls_ops[i].depth > 0U) {
            return g_llam_broker_tls_ops[i].subject_stack[g_llam_broker_tls_ops[i].depth - 1U];
        }
    }
    return 0U;
}

int llam_broker_lock(llam_broker_t *broker) {
    int rc;

    if (LLAM_UNLIKELY(broker == NULL || !broker->lock_initialized)) {
        errno = EINVAL;
        return -1;
    }
    rc = pthread_mutex_lock(&broker->lock);
    if (LLAM_UNLIKELY(rc != 0)) {
        errno = rc;
        return -1;
    }
    return 0;
}

void llam_broker_unlock(llam_broker_t *broker) {
    if (broker != NULL && broker->lock_initialized) {
        (void)pthread_mutex_unlock(&broker->lock);
    }
}

int llam_broker_begin_op_subject(llam_broker_t *broker, uint64_t subject_id) {
    bool nested;

    if (llam_broker_lock(broker) != 0) {
        return -1;
    }
    nested = llam_broker_tls_has_op(broker);
    if (LLAM_UNLIKELY(!broker->initialized ||
                      broker->runtime == NULL ||
                      (broker->destroying && !nested))) {
        llam_broker_unlock(broker);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(broker->active_ops >= LLAM_BROKER_ACTIVE_OP_BUSY_SENTINEL - 1U)) {
        /*
         * The high half of active_ops is reserved for saturated/corrupt
         * lifecycle state. Reject before incrementing into that range so
         * destroy and end paths never have to treat a high-half value as a
         * valid active-operation count.
         */
        llam_broker_unlock(broker);
        errno = EBUSY;
        return -1;
    }
    if (llam_broker_tls_enter_op(broker, subject_id) != 0) {
        llam_broker_unlock(broker);
        return -1;
    }
    broker->active_ops += 1U;
    llam_broker_unlock(broker);
    return 0;
}

int llam_broker_begin_op(llam_broker_t *broker) {
    return llam_broker_begin_op_subject(broker, 0U);
}

void llam_broker_end_op(llam_broker_t *broker) {
    if (llam_broker_lock(broker) != 0) {
        return;
    }
    if (LLAM_UNLIKELY(broker->active_ops >= LLAM_BROKER_ACTIVE_OP_BUSY_SENTINEL)) {
        /*
         * Saturated/corrupt counters are permanent busy sentinels. Do not
         * decrement them: UINT32_MAX - 1 would manufacture a different
         * high-half value and obscure the fail-closed lifecycle state.
         */
    } else if (broker->active_ops > 0U) {
        broker->active_ops -= 1U;
    }
    llam_broker_tls_leave_op(broker);
    if (broker->destroying && broker->active_ops == 0U && broker->idle_cond_initialized) {
        (void)pthread_cond_broadcast(&broker->idle_cond);
    }
    llam_broker_unlock(broker);
}
