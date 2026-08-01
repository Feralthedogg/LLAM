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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
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
#define LLAM_RUNTIME_BACKEND_BSD 0
#define LLAM_RUNTIME_BACKEND_KQUEUE 0
#elif LLAM_PLATFORM_LINUX
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 1
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#define LLAM_RUNTIME_BACKEND_BSD 0
#define LLAM_RUNTIME_BACKEND_KQUEUE 0
#elif LLAM_PLATFORM_DARWIN
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 1
#define LLAM_RUNTIME_BACKEND_BSD 0
#define LLAM_RUNTIME_BACKEND_KQUEUE 1
#elif LLAM_PLATFORM_BSD
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#define LLAM_RUNTIME_BACKEND_BSD 1
#define LLAM_RUNTIME_BACKEND_KQUEUE 1
#else
#define LLAM_RUNTIME_BACKEND_WINDOWS 0
#define LLAM_RUNTIME_BACKEND_POSIX 1
#define LLAM_RUNTIME_BACKEND_LINUX 0
#define LLAM_RUNTIME_BACKEND_DARWIN 0
#define LLAM_RUNTIME_BACKEND_BSD 0
#define LLAM_RUNTIME_BACKEND_KQUEUE 0
#endif

// Native Windows builds must opt into the staged backend explicitly so accidental
// partial toolchain builds fail with a clear diagnostic.
#if LLAM_RUNTIME_BACKEND_WINDOWS && !defined(LLAM_ENABLE_WINDOWS_BACKEND)
#error "Native Windows 10/11 backend requires LLAM_ENABLE_WINDOWS_BACKEND; use the CMake Windows configuration."
#endif

#endif
