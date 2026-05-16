/**
 * @file src/internal/runtime_debug_dump_helpers.h
 * @brief Internal helpers for runtime dump diagnostics.
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

#ifndef LLAM_RUNTIME_DEBUG_DUMP_HELPERS_H
#define LLAM_RUNTIME_DEBUG_DUMP_HELPERS_H

#include "runtime_internal.h"

const char *llam_io_kind_name_diag(llam_io_kind_t kind);
const char *llam_io_wait_mode_name_diag(unsigned mode);
const char *llam_io_abort_reason_name_diag(unsigned reason);
const char *llam_block_job_state_name_diag(unsigned state);
unsigned llam_io_req_list_count_diag(const llam_io_req_t *head);
unsigned llam_io_control_list_count_diag(const llam_io_control_op_t *head);
const char *llam_task_wait_owner_name_diag(const llam_task_t *task);

#endif
