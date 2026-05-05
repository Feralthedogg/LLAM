/**
 * @file include/llam/platform.h
 * @brief Canonical public platform contract and cross-platform fd/socket type definitions.
 *
 * @details
 * This header contains only public platform facts required by the LLAM API. It
 * intentionally avoids private runtime configuration so applications can include
 * it directly and write portable code around @c llam_fd_t and the
 * @c LLAM_PLATFORM_* feature macros.
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

#ifndef LLAM_PLATFORM_H
#define LLAM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/** @brief Non-zero when compiling for Windows. */
#if defined(_WIN32)
#define LLAM_PLATFORM_WINDOWS 1
#else
#define LLAM_PLATFORM_WINDOWS 0
#endif

/** @brief Non-zero when compiling for Linux. */
#if defined(__linux__) && !LLAM_PLATFORM_WINDOWS
#define LLAM_PLATFORM_LINUX 1
#else
#define LLAM_PLATFORM_LINUX 0
#endif

/** @brief Non-zero when compiling for Darwin/macOS. */
#if defined(__APPLE__) && defined(__MACH__) && !LLAM_PLATFORM_WINDOWS
#define LLAM_PLATFORM_DARWIN 1
#else
#define LLAM_PLATFORM_DARWIN 0
#endif

/** @brief Non-zero when the public fd type is POSIX-compatible. */
#if !LLAM_PLATFORM_WINDOWS
#define LLAM_PLATFORM_POSIX 1
#else
#define LLAM_PLATFORM_POSIX 0
#endif

/**
 * @brief LLAM uses POSIX file descriptors on Unix-like platforms and SOCKET on
 * Windows.
 */
#if LLAM_PLATFORM_WINDOWS
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
typedef SOCKET llam_fd_t;
#else
#include <sys/socket.h>
#include <sys/types.h>
/** @brief Public file/socket descriptor type on POSIX platforms. */
typedef int llam_fd_t;
#endif

/** @brief Human-readable platform name used by diagnostics and simple feature checks. */
#if LLAM_PLATFORM_WINDOWS
#define LLAM_PLATFORM_NAME "windows"
#elif LLAM_PLATFORM_LINUX
#define LLAM_PLATFORM_NAME "linux"
#elif LLAM_PLATFORM_DARWIN
#define LLAM_PLATFORM_NAME "darwin"
#else
#define LLAM_PLATFORM_NAME "posix"
#endif

#endif
