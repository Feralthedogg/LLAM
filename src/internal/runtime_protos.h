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

#ifndef NM_RUNTIME_PROTOS_H
#define NM_RUNTIME_PROTOS_H

#include "runtime_proto_core.h"
#include "runtime_proto_sched.h"
#include "runtime_proto_io.h"
#include "runtime_proto_sync.h"

#endif
