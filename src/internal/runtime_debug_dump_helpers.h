/**
 * @file src/internal/runtime_debug_dump_helpers.h
 * @brief Internal helpers for runtime dump diagnostics.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
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
