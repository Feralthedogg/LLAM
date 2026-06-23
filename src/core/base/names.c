/**
 * @file src/core/base/names.c
 * @brief Task state, wait reason, and runtime name formatting helpers.
 *
 * @details
 * Diagnostic dumps and trace output use these helpers instead of duplicating
 * switch statements across modules.
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

/**
 * @brief Convert a trace-kind enum value to a stable diagnostic string.
 *
 * @param kind Trace kind.
 *
 * @return Static string name.
 */
const char *llam_trace_kind_name(llam_trace_kind_t kind) {
    switch (kind) {
    case LLAM_TRACE_STATE:
        return "state";
    case LLAM_TRACE_WAKE:
        return "wake";
    case LLAM_TRACE_BLOCK_SUBMIT:
        return "block_submit";
    case LLAM_TRACE_BLOCK_COMPLETE:
        return "block_complete";
    case LLAM_TRACE_IO_SUBMIT:
        return "io_submit";
    case LLAM_TRACE_IO_COMPLETE:
        return "io_complete";
    case LLAM_TRACE_IDLE:
        return "idle";
    case LLAM_TRACE_STEAL:
        return "steal";
    case LLAM_TRACE_WATCHDOG:
        return "watchdog";
    default:
        return "unknown";
    }
}

/**
 * @brief Convert a task-state enum value to a stable diagnostic string.
 *
 * @param state Task state.
 *
 * @return Static string name.
 */
const char *llam_state_name_from_id(llam_task_state_id_t state) {
    switch (state) {
    case LLAM_TASK_STATE_NEW:
        return "NEW";
    case LLAM_TASK_STATE_RUNNABLE:
        return "RUNNABLE";
    case LLAM_TASK_STATE_RUNNING:
        return "RUNNING";
    case LLAM_TASK_STATE_PARKED:
        return "PARKED";
    case LLAM_TASK_STATE_BLOCKED_OPAQUE:
        return "BLOCKED_OPAQUE";
    case LLAM_TASK_STATE_DEAD:
        return "DEAD";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Convert a wait-reason enum value to a stable diagnostic string.
 *
 * @param reason Wait reason.
 *
 * @return Static string name.
 */
const char *llam_wait_reason_name(llam_wait_reason_t reason) {
    switch (reason) {
    case LLAM_WAIT_NONE:
        return "none";
    case LLAM_WAIT_YIELD:
        return "yield";
    case LLAM_WAIT_JOIN:
        return "join";
    case LLAM_WAIT_SLEEP:
        return "sleep";
    case LLAM_WAIT_BLOCKING:
        return "blocking";
    case LLAM_WAIT_IO:
        return "io";
    case LLAM_WAIT_CANCEL:
        return "cancel";
    case LLAM_WAIT_MUTEX:
        return "mutex";
    case LLAM_WAIT_COND:
        return "cond";
    case LLAM_WAIT_CHANNEL_SEND:
        return "channel_send";
    case LLAM_WAIT_CHANNEL_RECV:
        return "channel_recv";
    case LLAM_WAIT_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}
