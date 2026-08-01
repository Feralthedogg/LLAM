/**
 * @file src/internal/runtime_protos.h
 * @brief Aggregate internal prototype include for runtime subsystems.
 *
 * @details
 * This header exists to keep implementation includes short while preserving the
 * ownership split across core, scheduler, I/O, and synchronization prototypes.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#ifndef LLAM_RUNTIME_PROTOS_H
#define LLAM_RUNTIME_PROTOS_H

#include "runtime_proto_core.h"
#include "runtime_proto_sched.h"
#include "runtime_proto_io.h"
#include "runtime_proto_sync.h"

#endif
