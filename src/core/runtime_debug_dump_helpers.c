/**
 * @file src/core/runtime_debug_dump_helpers.c
 * @brief Helper routines for human-readable runtime state dumps.
 *
 * @details
 * These helpers keep the public diagnostics file focused on collection and
 * output while isolating dump-only name conversion, queue counting, and parked
 * task ownership attribution.
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

#include "runtime_debug_dump_helpers.h"

const char *llam_io_kind_name_diag(llam_io_kind_t kind) {
    switch (kind) {
    case LLAM_IO_KIND_READ:
        return "read";
    case LLAM_IO_KIND_WRITE:
        return "write";
    case LLAM_IO_KIND_ACCEPT:
        return "accept";
    case LLAM_IO_KIND_POLL:
        return "poll";
    case LLAM_IO_KIND_CONNECT:
        return "connect";
    case LLAM_IO_KIND_HANDLE_READ:
        return "handle_read";
    case LLAM_IO_KIND_HANDLE_WRITE:
        return "handle_write";
    default:
        return "unknown";
    }
}

const char *llam_io_wait_mode_name_diag(unsigned mode) {
    switch (mode) {
    case LLAM_IO_WAIT_MODE_NONE:
        return "none";
    case LLAM_IO_WAIT_MODE_SUBMIT_QUEUE:
        return "submit_queue";
    case LLAM_IO_WAIT_MODE_INFLIGHT:
        return "inflight";
    case LLAM_IO_WAIT_MODE_POLL_WATCH:
        return "poll_watch";
    case LLAM_IO_WAIT_MODE_ACCEPT_WATCH:
        return "accept_watch";
    case LLAM_IO_WAIT_MODE_RECV_WATCH:
        return "recv_watch";
    default:
        return "unknown";
    }
}

const char *llam_io_abort_reason_name_diag(unsigned reason) {
    switch (reason) {
    case LLAM_IO_ABORT_NONE:
        return "none";
    case LLAM_IO_ABORT_CANCEL:
        return "cancel";
    case LLAM_IO_ABORT_TIMEOUT:
        return "timeout";
    case LLAM_IO_ABORT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *llam_block_job_state_name_diag(unsigned state) {
    switch (state) {
    case LLAM_BLOCK_JOB_QUEUED:
        return "queued";
    case LLAM_BLOCK_JOB_RUNNING:
        return "running";
    case LLAM_BLOCK_JOB_FINISHED:
        return "finished";
    case LLAM_BLOCK_JOB_ABORTED:
        return "aborted";
    default:
        return "unknown";
    }
}

unsigned llam_io_req_list_count_diag(const llam_io_req_t *head) {
    unsigned count = 0U;

    for (const llam_io_req_t *req = head; req != NULL; req = req->next) {
        count += 1U;
    }
    return count;
}

unsigned llam_io_control_list_count_diag(const llam_io_control_op_t *head) {
    unsigned count = 0U;

    for (const llam_io_control_op_t *op = head; op != NULL; op = op->next) {
        count += 1U;
    }
    return count;
}

const char *llam_task_wait_owner_name_diag(const llam_task_t *task) {
    if (task == NULL) {
        return "none";
    }
    if (llam_task_active_io_req_load(task) != NULL) {
        return "io_req";
    }
    if (task->active_select_state != NULL) {
        return "select";
    }
    if (task->active_wait_node != NULL) {
        return "wait_node";
    }
    if (llam_task_active_block_job_load(task) != NULL) {
        return "blocking_job";
    }
    if (task->join_target != NULL) {
        return "join_target";
    }
    if (task->active_timer != NULL) {
        return "timer";
    }
    return "none";
}
