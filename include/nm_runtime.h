/**
 * @file include/nm_runtime.h
 * @brief Top-level compatibility include for projects that still include nm_runtime.h directly.
 *
 * @details
 * New code should prefer @c <llam/runtime.h> or @c <llam/nm_runtime.h>. This
 * wrapper preserves the historical include path without duplicating API
 * declarations.
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

#ifndef NM_RUNTIME_COMPAT_H
#define NM_RUNTIME_COMPAT_H

// Compatibility wrapper: all public declarations live in the namespaced header.
#include "llam/nm_runtime.h"

#endif
