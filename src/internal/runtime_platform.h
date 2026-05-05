/**
 * @file src/internal/runtime_platform.h
 * @brief Internal platform abstraction for threads, atomics-adjacent helpers, timing, and OS integration.
 *
 * @details
 * Public platform detection lives in @c include/llam/nm_platform.h. This private
 * header converts those public platform macros into backend-selection flags used
 * by runtime implementation files.
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

#ifndef NM_RUNTIME_PLATFORM_H
#define NM_RUNTIME_PLATFORM_H

#include "llam/nm_platform.h"

// Backend flags are intentionally numeric macros so they work in #if expressions.
#if NM_PLATFORM_WINDOWS
#define NM_RUNTIME_BACKEND_WINDOWS 1
#define NM_RUNTIME_BACKEND_POSIX 0
#define NM_RUNTIME_BACKEND_LINUX 0
#define NM_RUNTIME_BACKEND_DARWIN 0
#elif NM_PLATFORM_LINUX
#define NM_RUNTIME_BACKEND_WINDOWS 0
#define NM_RUNTIME_BACKEND_POSIX 1
#define NM_RUNTIME_BACKEND_LINUX 1
#define NM_RUNTIME_BACKEND_DARWIN 0
#elif NM_PLATFORM_DARWIN
#define NM_RUNTIME_BACKEND_WINDOWS 0
#define NM_RUNTIME_BACKEND_POSIX 1
#define NM_RUNTIME_BACKEND_LINUX 0
#define NM_RUNTIME_BACKEND_DARWIN 1
#else
#define NM_RUNTIME_BACKEND_WINDOWS 0
#define NM_RUNTIME_BACKEND_POSIX 1
#define NM_RUNTIME_BACKEND_LINUX 0
#define NM_RUNTIME_BACKEND_DARWIN 0
#endif

// The Windows backend is declared at the platform layer but not implemented yet.
#if NM_RUNTIME_BACKEND_WINDOWS && !defined(NM_ENABLE_WINDOWS_BACKEND)
#error "Native Windows 10/11 backend is not implemented yet; use WSL/Linux or build after the IOCP/Fiber backend lands."
#endif

#endif
