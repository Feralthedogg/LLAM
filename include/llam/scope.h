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

#ifndef LLAM_SCOPE_H
#define LLAM_SCOPE_H

#include "llam/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed-layout value description. Bindings assign semantic IDs, not RTTI addresses.
 * Equal size/alignment alone does not establish type compatibility. Both ID words
 * zero denote unit and require size zero and alignment one. Reserved must be zero.
 * This value record has a fixed layout within ABI major 2. */
typedef struct llam_type_descriptor {
    uint64_t id_high;
    uint64_t id_low;
    uint64_t size;
    uint32_t alignment;
    uint32_t reserved;
} llam_type_descriptor_t;

/* Reserved semantic IDs for interoperable scalar values. Custom IDs must not
 * use this high word. Representation is native-endian, not a wire format. */
#define LLAM_TYPE_SCALAR_ID_HIGH UINT64_C(0x4c4c414d5f545950)
#ifdef __cplusplus
#define LLAM_SCOPE_ALIGNOF(type) alignof(type)
#else
#define LLAM_SCOPE_ALIGNOF(type) _Alignof(type)
#endif
#define LLAM_TYPE_UNIT_INIT {0U, 0U, 0U, 1U, 0U}
#define LLAM_TYPE_U64_INIT {LLAM_TYPE_SCALAR_ID_HIGH, 1U, sizeof(uint64_t), LLAM_SCOPE_ALIGNOF(uint64_t), 0U}
#define LLAM_TYPE_I64_INIT {LLAM_TYPE_SCALAR_ID_HIGH, 2U, sizeof(int64_t), LLAM_SCOPE_ALIGNOF(int64_t), 0U}
#define LLAM_TYPE_U32_INIT {LLAM_TYPE_SCALAR_ID_HIGH, 3U, sizeof(uint32_t), LLAM_SCOPE_ALIGNOF(uint32_t), 0U}
#define LLAM_TYPE_I32_INIT {LLAM_TYPE_SCALAR_ID_HIGH, 4U, sizeof(int32_t), LLAM_SCOPE_ALIGNOF(int32_t), 0U}

/** Fixed-layout success/error type pair; both default to unit. */
typedef struct llam_result_contract {
    llam_type_descriptor_t success;
    llam_type_descriptor_t error;
} llam_result_contract_t;

/** Cleanup runs once, outside runtime locks, in a native guard. It must be
 * nonblocking, thread-neutral, non-throwing, and must not call LLAM APIs.
 * Its code and context must remain valid until the owned value is disposed.
 * A provider module must remain loaded through every such callback. */
typedef void (*llam_value_drop_fn)(void *data, void *context);

typedef struct llam_value_spec {
    llam_type_descriptor_t type;
    void *data;
    llam_value_drop_fn drop; /**< NULL selects free(data). */
    void *drop_context;
} llam_value_spec_t;

typedef struct llam_scope_opts {
    uint32_t flags;     /**< Reserved; must be zero. */
    uint32_t reserved0;
} llam_scope_opts_t;

typedef struct llam_typed_task_spec {
    llam_task_fn entry;             /**< Qualified entry on a required runtime. */
    llam_value_t *argument;         /**< Consumed only on successful spawn; NULL passes NULL. */
    const llam_spawn_opts_t *spawn_opts; /**< Borrowed only during the call. */
    uint64_t spawn_opts_size;
    llam_result_contract_t result;
} llam_typed_task_spec_t;

#define LLAM_OUTCOME_OK UINT32_C(1)
#define LLAM_OUTCOME_APP_ERROR UINT32_C(2)
#define LLAM_OUTCOME_CANCELLED UINT32_C(3)
#define LLAM_OUTCOME_RUNTIME_ERROR UINT32_C(4)

typedef struct llam_outcome {
    uint32_t kind;
    int32_t runtime_error;  /**< Only RUNTIME_ERROR uses this field. */
    llam_value_t *value;    /**< Owned on successful take; consumed on successful set. */
    uint64_t task_id;       /**< Output-only diagnostic identity. */
} llam_outcome_t;

#define LLAM_SCOPE_OPEN UINT32_C(0)
#define LLAM_SCOPE_CLOSING UINT32_C(1)
#define LLAM_SCOPE_DRAINED UINT32_C(2)
#define LLAM_SCOPE_REPORT_F_CANCEL_REQUESTED UINT32_C(1)
#define LLAM_SCOPE_REPORT_F_QUIESCENT UINT32_C(2)
#define LLAM_SCOPE_REPORT_F_NEEDS_RUNNER UINT32_C(4)

typedef struct llam_scope_report {
    uint32_t state;
    uint32_t flags;
    uint64_t admitted_tasks;
    uint64_t pending_tasks; /**< Includes backend/native retirement and owned cleanup. */
    uint64_t active_spawns;
    uint64_t succeeded;
    uint64_t application_errors;
    uint64_t cancelled;
    uint64_t runtime_errors;
    int32_t runtime_error;
    uint32_t reserved0;
} llam_scope_report_t;

#define LLAM_TYPE_DESCRIPTOR_CURRENT_SIZE ((size_t)sizeof(llam_type_descriptor_t))
#define LLAM_RESULT_CONTRACT_CURRENT_SIZE ((size_t)sizeof(llam_result_contract_t))
#define LLAM_VALUE_SPEC_CURRENT_SIZE ((size_t)sizeof(llam_value_spec_t))
#define LLAM_SCOPE_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_scope_opts_t))
#define LLAM_TYPED_TASK_SPEC_CURRENT_SIZE ((size_t)sizeof(llam_typed_task_spec_t))
#define LLAM_OUTCOME_CURRENT_SIZE ((size_t)sizeof(llam_outcome_t))
#define LLAM_SCOPE_REPORT_CURRENT_SIZE ((size_t)sizeof(llam_scope_report_t))

LLAM_API int llam_result_contract_init(llam_result_contract_t *out, size_t out_size);
LLAM_API int llam_value_spec_init(llam_value_spec_t *out, size_t out_size);
LLAM_API int llam_scope_opts_init(llam_scope_opts_t *out, size_t out_size);
LLAM_API int llam_typed_task_spec_init(llam_typed_task_spec_t *out, size_t out_size);
LLAM_API int llam_outcome_init(llam_outcome_t *out, size_t out_size);

/** Adopt an initialized, aligned value. Failure leaves data and output untouched.
 * Success transfers data/drop_context ownership; the value handle is move-only. */
LLAM_API int llam_value_create_ex(const llam_value_spec_t *spec, size_t spec_size,
                                  llam_value_t **out);
/** Copy a trivially byte-copyable representation into independently owned storage. */
LLAM_API int llam_value_copy_ex(const llam_type_descriptor_t *type, size_t type_size,
                                const void *data, llam_value_t **out);
/** Borrow storage while retaining exclusive ownership of the handle. Type failure
 * leaves output untouched. Concurrent handle consumption invalidates the borrow. */
LLAM_API int llam_value_data_ex(llam_value_t *value, const llam_type_descriptor_t *expected,
                                size_t expected_size, void **out);
/** Consume a value and invoke its cleanup once. Claimed values return EBUSY. */
LLAM_API int llam_value_release(llam_value_t *value);

LLAM_API int llam_runtime_scope_create_ex(llam_runtime_t *runtime,
                                          const llam_scope_opts_t *opts, size_t opts_size,
                                          llam_scope_t **out);
/** Spawn owned work. External cancellation is ORed with the private scope token.
 * Failure preserves argument and output. Closed admission returns EPIPE. */
LLAM_API int llam_scope_spawn_ex(llam_scope_t *scope, const llam_typed_task_spec_t *spec,
                                  size_t spec_size, llam_future_t **out);
/** Set one candidate outcome on the current scoped task. Values are consumed only
 * on success; a second set returns EBUSY. Public readiness follows full retirement. */
LLAM_API int llam_task_set_outcome_ex(const llam_outcome_t *outcome, size_t outcome_size);
/** Wait without consuming. Caller cancellation/deadline affects this wait only.
 * Ready results win over an expired deadline. Host waits never drive a runtime. */
LLAM_API int llam_future_wait_until(llam_future_t *future, uint64_t deadline_ns);
/** Nonblocking single-consumer take. Expected types and the complete V1 output
 * prefix are validated before ownership moves. EAGAIN means not ready. A take
 * returning zero may yield APP_ERROR, CANCELLED or RUNTIME_ERROR, not only OK. */
LLAM_API int llam_future_take_ex(llam_future_t *future,
                                  const llam_result_contract_t *expected, size_t expected_size,
                                  llam_outcome_t *out, size_t out_size);
/** Consume result interest; scope ownership of execution is retained. */
LLAM_API int llam_future_release(llam_future_t *future);

/** Seal admission and request cancellation; idempotent and scope-local. */
LLAM_API int llam_scope_cancel(llam_scope_t *scope);
LLAM_API int llam_scope_get_report_ex(llam_scope_t *scope,
                                       llam_scope_report_t *out, size_t out_size);
/** Seal admission and wait for complete retirement. This cleanup wait ignores
 * caller cancellation and its inherited request budget, but preserves both for
 * subsequent operations. Only cleanup_deadline_ns bounds this wait; zero never
 * waits and UINT64_MAX is unbounded. A timeout retains every outstanding resource.
 * Runtime failure/stop can interrupt incomplete drain. A host needs an active
 * driver for pending work (EBUSY/NEEDS_RUNNER); a scope child cannot drain itself
 * (EDEADLK). Only one drainer is admitted. Reports are written after validation,
 * including on wait failure. This API does not support borrowed lexical stacks. */
LLAM_API int llam_scope_drain_until(llam_scope_t *scope, uint64_t cleanup_deadline_ns,
                                    llam_scope_report_t *out, size_t out_size);
LLAM_API int llam_scope_cancel_and_drain_until(llam_scope_t *scope, uint64_t cleanup_deadline_ns,
                                               llam_scope_report_t *out, size_t out_size);
/** Consume a drained scope with no concurrent public operation. Ready futures and
 * taken values remain owned independently; their callbacks must remain loaded. */
LLAM_API int llam_scope_destroy(llam_scope_t *scope);

#ifdef __cplusplus
}
#endif
#endif
