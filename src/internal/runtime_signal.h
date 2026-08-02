/**
 * @file src/internal/runtime_signal.h
 * @brief Private process-signal and per-thread alternate-stack state.
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

#ifndef LLAM_RUNTIME_SIGNAL_INTERNAL_H
#define LLAM_RUNTIME_SIGNAL_INTERNAL_H

#include "runtime_platform.h"

#if LLAM_RUNTIME_BACKEND_WINDOWS
#include "runtime_windows_compat.h"
#else
#include <signal.h>
#endif
#include <stdbool.h>
#include <stddef.h>

/** @brief One native thread's alternate-signal-stack ownership scope. */
typedef struct llam_thread_signal_stack {
    void *mapping;       /**< Guarded mapping base owned by this thread. */
    size_t mapping_size; /**< Full mapping size including guard pages. */
    void *stack_sp;      /**< Usable alternate-stack base. */
    size_t stack_size;   /**< Usable alternate-stack byte count. */
    stack_t previous;    /**< Host stack restored by the owning thread. */
    bool installed;      /**< Whether this scope entered signal-stack state. */
    bool borrowed;       /**< Whether an already-valid host stack is in use. */
} llam_thread_signal_stack_t;

#endif
