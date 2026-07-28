/**
 * @file src/io/linux/runtime_io_segment_linux_internal.h
 * @brief Private Linux representation for compiled LEIR effect segments.
 *
 * @details
 * A native segment is a fixed-size, allocation-free description of a small
 * sequence of socket operations.  The Linux backend can encode the sequence as
 * one io_uring link while retaining an identity token for every CQE that may
 * reach userspace.
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

#ifndef LLAM_RUNTIME_IO_SEGMENT_LINUX_INTERNAL_H
#define LLAM_RUNTIME_IO_SEGMENT_LINUX_INTERNAL_H

#include "runtime_internal.h"

#define LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS 8U
#define LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS 8U
#define LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS 64U
#define LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS 64U

typedef enum llam_linux_native_op_kind {
    LLAM_LINUX_NATIVE_OP_RECV = 0,
    LLAM_LINUX_NATIVE_OP_SEND = 1,
} llam_linux_native_op_kind_t;

enum {
    LLAM_LINUX_NATIVE_OP_FIXED_FILE = 1U << 0,
    LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER = 1U << 1,
};

typedef enum llam_linux_native_segment_mode {
    LLAM_LINUX_NATIVE_SEGMENT_LINK = 0,
    LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP = 1,
} llam_linux_native_segment_mode_t;

typedef enum llam_linux_native_cqe_action {
    LLAM_LINUX_NATIVE_CQE_CONTINUE = 0,
    LLAM_LINUX_NATIVE_CQE_SEMANTIC = 1,
    LLAM_LINUX_NATIVE_CQE_RETIRED_OK = 2,
    LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR = 3,
    LLAM_LINUX_NATIVE_CQE_FATAL = 4,
} llam_linux_native_cqe_action_t;

enum {
    LLAM_LINUX_NATIVE_SEGMENT_IDLE = 0U,
    LLAM_LINUX_NATIVE_SEGMENT_QUEUED = 1U,
    LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_SEGMENT_RETIRING = 3U,
    LLAM_LINUX_NATIVE_SEGMENT_RETIRED = 4U,
};

enum {
    LLAM_LINUX_NATIVE_BATCH_IDLE = 0U,
    LLAM_LINUX_NATIVE_BATCH_QUEUED = 1U,
    LLAM_LINUX_NATIVE_BATCH_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_BATCH_RETIRING = 3U,
    LLAM_LINUX_NATIVE_BATCH_RETIRED = 4U,
};

enum {
    LLAM_LINUX_NATIVE_CANCEL_NONE = 0U,
    LLAM_LINUX_NATIVE_CANCEL_QUEUED = 1U,
    LLAM_LINUX_NATIVE_CANCEL_SUBMITTED = 2U,
    LLAM_LINUX_NATIVE_CANCEL_RETIRED = 3U,
};

typedef struct llam_linux_native_op {
    uint16_t kind;
    uint16_t result_slot;
    uint16_t flags;
    uint16_t fixed_file_slot;
    uint16_t fixed_buffer_slot;
    uint16_t reserved16;
    llam_fd_t fd;
    void *buffer;
    uint32_t length;
} llam_linux_native_op_t;

typedef struct llam_linux_native_segment
    llam_linux_native_segment_t;
typedef struct llam_linux_native_batch
    llam_linux_native_batch_t;

typedef struct llam_linux_native_resource_lease {
    struct llam_linux_native_resource_lease *next;
    unsigned
        file_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned
        buffer_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned file_count;
    unsigned buffer_count;
    unsigned node_index;
    bool attached;
} llam_linux_native_resource_lease_t;

typedef struct llam_linux_native_token {
    _Alignas(8) llam_linux_native_segment_t *owner;
    uint64_t generation;
    uint16_t operation_index;
    uint16_t reserved16;
    uint32_t reserved32;
} llam_linux_native_token_t;

typedef struct llam_linux_native_cancel_token {
    _Alignas(8) llam_linux_native_segment_t *owner;
    uint64_t generation;
    uint16_t operation_index;
    uint16_t reserved16;
    uint32_t reserved32;
} llam_linux_native_cancel_token_t;

struct llam_linux_native_segment {
    llam_runtime_t *owner_runtime;
    llam_node_t *owner_node;
    llam_io_req_t *req;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_token_t
        tokens[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_cancel_token_t
        cancel_tokens[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_segment_t *next;
    llam_linux_native_batch_t *batch;
    uint64_t generation;
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
    uint64_t observed_operation_mask;
    uint64_t observed_cancel_mask;
    unsigned op_count;
    unsigned completed_cqes;
    unsigned first_error_index;
    int first_error;
    int semantic_result;
    llam_linux_native_segment_mode_t mode;
    bool atomic_submission;
    atomic_uint state;
    atomic_uint semantic_claimed;
    atomic_uint target_retired;
    atomic_uint terminal_claimed;
};

struct llam_linux_native_batch {
    llam_runtime_t *owner_runtime;
    llam_node_t *owner_node;
    llam_io_req_t *req;
    llam_linux_native_segment_t
        *segments[LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS];
    llam_linux_native_batch_t *next;
    llam_linux_native_batch_t *cancel_next;
    unsigned segment_count;
    unsigned retired_segments;
    unsigned cancel_sqes_prepared;
    unsigned cancel_cqes_observed;
    int terminal_result;
    unsigned first_error_segment;
    atomic_uint state;
    atomic_uint terminal_claimed;
    atomic_uint cancel_state;
    atomic_uint cancel_requested;
};

LLAM_INTERNAL_API int llam_linux_native_segment_configure(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_op_t *ops,
    unsigned op_count,
    llam_linux_native_segment_mode_t mode);
LLAM_INTERNAL_API void llam_linux_native_segment_prepare_sqe(
    const llam_linux_native_segment_t *segment,
    unsigned index,
    struct io_uring_sqe *sqe);
LLAM_INTERNAL_API llam_linux_native_cqe_action_t
llam_linux_native_segment_apply_cqe(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_token_t *token,
    int result,
    int *terminal_result_out);
LLAM_INTERNAL_API bool llam_linux_native_batch_enqueue(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req);
LLAM_INTERNAL_API llam_linux_native_batch_t *
llam_linux_native_batch_take_all(llam_node_t *node);
LLAM_INTERNAL_API unsigned llam_linux_native_batch_submit_one(
    llam_node_t *node,
    llam_linux_native_batch_t *batch);
LLAM_INTERNAL_API bool llam_linux_native_batch_abort_queued(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req);
LLAM_INTERNAL_API bool llam_linux_native_batch_request_cancel(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req);
LLAM_INTERNAL_API llam_linux_native_batch_t *
llam_linux_native_cancel_take_all(llam_node_t *node);
LLAM_INTERNAL_API unsigned llam_linux_native_batch_submit_cancel(
    llam_node_t *node,
    llam_linux_native_batch_t *batch);
LLAM_INTERNAL_API void llam_linux_native_batch_maybe_complete(
    llam_node_t *node,
    llam_linux_native_batch_t *batch);
LLAM_INTERNAL_API void llam_linux_native_segment_handle_cqe(
    llam_node_t *node,
    llam_linux_native_token_t *token,
    int result);
LLAM_INTERNAL_API void llam_linux_native_cancel_handle_cqe(
    llam_node_t *node,
    llam_linux_native_cancel_token_t *token,
    int result);
LLAM_INTERNAL_API int llam_linux_native_resources_setup(
    llam_node_t *node);
LLAM_INTERNAL_API void
llam_linux_native_resources_before_ring_exit(
    llam_node_t *node);
LLAM_INTERNAL_API void
llam_linux_native_resources_after_ring_exit(
    llam_node_t *node);
LLAM_INTERNAL_API int llam_linux_native_resources_attach(
    llam_node_t *node,
    const int *fds,
    unsigned fd_count,
    const struct iovec *buffers,
    unsigned buffer_count,
    llam_linux_native_resource_lease_t *lease);
LLAM_INTERNAL_API int llam_linux_native_resources_detach(
    llam_node_t *node,
    llam_linux_native_resource_lease_t *lease);

#endif
