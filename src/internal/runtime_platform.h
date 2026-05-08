/**
 * @file src/internal/runtime_platform.h
 * @brief Internal platform abstraction for threads, atomics-adjacent helpers, timing, and OS integration.
 *
 * @details
 * Public platform detection lives in @c include/llam/platform.h. This private
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

#ifndef LLAM_RUNTIME_PLATFORM_H
#define LLAM_RUNTIME_PLATFORM_H

#include "llam/platform.h"

// Backend flags are intentionally numeric macros so they work in #if expressions.
#if LLAM_PLATFORM_WINDOWS
#define LLAM_RUNTIME_BACKEND_WINDOWS 1
#define LLAM_RUNTIME_BACKEND_POSIX 0
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#elif LLAM_PLATFORM_LINUX
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 1
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#elif LLAM_PLATFORM_DARWIN
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 1
#else
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#endif

// Native Windows builds must opt into the staged backend explicitly so accidental
// partial toolchain builds fail with a clear diagnostic.
#if LLAM_RUNTIME_BACKEND_WINDOWS && !defined(LLAM_ENABLE_WINDOWS_BACKEND)
#error "Native Windows 10/11 backend requires LLAM_ENABLE_WINDOWS_BACKEND; use the CMake Windows configuration."
#endif

#endif
