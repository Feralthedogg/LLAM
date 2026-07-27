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

typedef enum llam_linux_native_op_kind {
    LLAM_LINUX_NATIVE_OP_RECV = 0,
    LLAM_LINUX_NATIVE_OP_SEND = 1,
} llam_linux_native_op_kind_t;

typedef enum llam_linux_native_segment_mode {
    LLAM_LINUX_NATIVE_SEGMENT_LINK = 0,
    LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP = 1,
} llam_linux_native_segment_mode_t;

typedef enum llam_linux_native_cqe_action {
    LLAM_LINUX_NATIVE_CQE_CONTINUE = 0,
    LLAM_LINUX_NATIVE_CQE_COMPLETE_OK = 1,
    LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR = 2,
    LLAM_LINUX_NATIVE_CQE_FATAL = 3,
} llam_linux_native_cqe_action_t;

enum {
    LLAM_LINUX_NATIVE_SEGMENT_IDLE = 0U,
    LLAM_LINUX_NATIVE_SEGMENT_QUEUED = 1U,
    LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_SEGMENT_TERMINAL = 3U,
};

typedef struct llam_linux_native_op {
    uint16_t kind;
    uint16_t result_slot;
    llam_fd_t fd;
    void *buffer;
    uint32_t length;
} llam_linux_native_op_t;

typedef struct llam_linux_native_segment
    llam_linux_native_segment_t;

typedef struct llam_linux_native_token {
    _Alignas(8) llam_linux_native_segment_t *owner;
    uint64_t generation;
    uint16_t operation_index;
    uint16_t reserved16;
    uint32_t reserved32;
} llam_linux_native_token_t;

struct llam_linux_native_segment {
    llam_runtime_t *owner_runtime;
    llam_node_t *owner_node;
    llam_io_req_t *req;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_token_t
        tokens[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_segment_t *next;
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
    unsigned op_count;
    unsigned completed_cqes;
    unsigned first_error_index;
    int first_error;
    llam_linux_native_segment_mode_t mode;
    atomic_uint state;
    atomic_uint terminal_claimed;
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
LLAM_INTERNAL_API bool llam_linux_native_segment_enqueue(
    llam_node_t *node,
    llam_linux_native_segment_t *segment,
    llam_io_req_t *req);
LLAM_INTERNAL_API llam_linux_native_segment_t *
llam_linux_native_segment_take_all(llam_node_t *node);
LLAM_INTERNAL_API unsigned llam_linux_native_segment_submit_one(
    llam_node_t *node,
    llam_linux_native_segment_t *segment);

#endif
