/**
 * @file include/llam/nm_platform.h
 * @brief Compatibility public platform contract for the legacy nm_* API names.
 *
 * @details
 * This header exposes platform facts for code using the legacy @c nm_* public
 * API. It mirrors @c <llam/platform.h> with @c NM_PLATFORM_* macro names.
 * It also guarantees the socket and ssize contracts used by
 * @c <llam/nm_runtime.h>: @c struct sockaddr, @c socklen_t, and @c ssize_t are
 * declared or typedef'd after including this header.
 *
 * On Windows, include this header before @c windows.h when possible so the
 * public contract can select @c winsock2.h without conflicting with the older
 * @c winsock.h declarations that @c windows.h may pull in.
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
#if defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error "Include llam/nm_platform.h before windows.h, or define WIN32_LEAN_AND_MEAN before including windows.h."
#endif
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
#include <sys/types.h>
#include <sys/socket.h>
typedef int nm_fd_t;
#endif

/** @brief Invalid descriptor/socket sentinel for ::nm_fd_t. */
#if NM_PLATFORM_WINDOWS
#define NM_INVALID_FD ((nm_fd_t)INVALID_SOCKET)
#else
#define NM_INVALID_FD ((nm_fd_t)-1)
#endif

/** @brief Non-zero when @p fd is the platform invalid descriptor/socket value. */
#define NM_FD_IS_INVALID(fd) ((fd) == NM_INVALID_FD)

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

/**
 * @brief Public symbol visibility for the legacy nm_* compatibility ABI.
 */
#if NM_PLATFORM_WINDOWS && defined(NM_BUILD_SHARED)
#define NM_API __declspec(dllexport)
#elif NM_PLATFORM_WINDOWS && defined(NM_SHARED)
#define NM_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(NM_BUILD_SHARED)
#define NM_API __attribute__((visibility("default")))
#else
#define NM_API
#endif

#endif
