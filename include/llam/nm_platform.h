/**
 * @file include/llam/nm_platform.h
 * @brief Compatibility public platform contract for the legacy nm_* API names.
 *
 * @details
 * This header exposes platform facts for code using the legacy @c nm_* public
 * API. It mirrors @c <llam/platform.h> with @c NM_PLATFORM_* macro names.
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

#ifndef NM_PLATFORM_H
#define NM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/** @brief Defined as 1 when compiling for a native Windows target. */
#if defined(_WIN32)
#define NM_PLATFORM_WINDOWS 1
#else
#define NM_PLATFORM_WINDOWS 0
#endif

/** @brief Defined as 1 when compiling for Linux. */
#if defined(__linux__) && !NM_PLATFORM_WINDOWS
#define NM_PLATFORM_LINUX 1
#else
#define NM_PLATFORM_LINUX 0
#endif

/** @brief Defined as 1 when compiling for Darwin/macOS. */
#if defined(__APPLE__) && defined(__MACH__) && !NM_PLATFORM_WINDOWS
#define NM_PLATFORM_DARWIN 1
#else
#define NM_PLATFORM_DARWIN 0
#endif

/** @brief Defined as 1 for non-Windows POSIX-style targets. */
#if !NM_PLATFORM_WINDOWS
#define NM_PLATFORM_POSIX 1
#else
#define NM_PLATFORM_POSIX 0
#endif

/** @brief Cross-platform file/socket descriptor type used by the legacy API. */
#if NM_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <sys/types.h>
#endif
#if defined(_MSC_VER)
#include <BaseTsd.h>
#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#ifndef _SOCKLEN_T_DEFINED
typedef int socklen_t;
#define _SOCKLEN_T_DEFINED
#endif
#endif
typedef SOCKET nm_fd_t;
#else
#include <sys/socket.h>
#include <sys/types.h>
typedef int nm_fd_t;
#endif

/** @brief Human-readable platform name used by diagnostics. */
#if NM_PLATFORM_WINDOWS
#define NM_PLATFORM_NAME "windows"
#elif NM_PLATFORM_LINUX
#define NM_PLATFORM_NAME "linux"
#elif NM_PLATFORM_DARWIN
#define NM_PLATFORM_NAME "darwin"
#else
#define NM_PLATFORM_NAME "posix"
#endif

#endif
