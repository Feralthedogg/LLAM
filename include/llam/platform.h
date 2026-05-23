/**
 * @file include/llam/platform.h
 * @brief Canonical public platform contract and cross-platform fd/socket type definitions.
 *
 * @details
 * This header contains only public platform facts required by the LLAM API. It
 * intentionally avoids private runtime configuration so applications can include
 * it directly and write portable code around @c llam_fd_t, @c llam_handle_t, and the
 * @c LLAM_PLATFORM_* feature macros. It also guarantees the public socket and
 * ssize contracts used by @c llam/runtime.h: @c struct sockaddr, @c socklen_t,
 * and @c ssize_t are declared or typedef'd after including this header.
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

/** @brief Non-zero when compiling for x86-64. */
#if defined(__x86_64__) || defined(_M_X64)
#define LLAM_ARCH_X86_64 1
#else
#define LLAM_ARCH_X86_64 0
#endif

/** @brief Non-zero when compiling for AArch64/arm64. */
#if defined(__aarch64__) || defined(_M_ARM64)
#define LLAM_ARCH_AARCH64 1
#else
#define LLAM_ARCH_AARCH64 0
#endif

/**
 * @brief LLAM uses POSIX file descriptors on Unix-like platforms and SOCKET on
 * Windows.
 */
#if LLAM_PLATFORM_WINDOWS
#if defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error "Include llam/platform.h before windows.h, or define WIN32_LEAN_AND_MEAN before including windows.h."
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
typedef SOCKET llam_fd_t;
/** @brief Public Windows HANDLE-compatible type used by HANDLE I/O APIs. */
typedef void *llam_handle_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
/** @brief Public file/socket descriptor type on POSIX platforms. */
typedef int llam_fd_t;
/** @brief Public handle type on POSIX; aliases the file descriptor type. */
typedef int llam_handle_t;
#endif

/** @brief Invalid descriptor/socket sentinel for ::llam_fd_t. */
#if LLAM_PLATFORM_WINDOWS
#define LLAM_INVALID_FD ((llam_fd_t)INVALID_SOCKET)
#else
#define LLAM_INVALID_FD ((llam_fd_t)-1)
#endif

/** @brief Non-zero when @p fd is the platform invalid descriptor/socket value. */
static inline int llam_fd_is_invalid(llam_fd_t fd) {
    return fd == LLAM_INVALID_FD;
}

/** @brief Non-zero when @p fd is the platform invalid descriptor/socket value. */
#define LLAM_FD_IS_INVALID(fd) (llam_fd_is_invalid((fd)))

/** @brief Invalid generic handle sentinel for ::llam_handle_t. */
#if LLAM_PLATFORM_WINDOWS
#define LLAM_INVALID_HANDLE ((llam_handle_t)(intptr_t)-1)
#else
#define LLAM_INVALID_HANDLE ((llam_handle_t)-1)
#endif

/** @brief Non-zero when @p handle is the platform invalid generic handle value. */
#if LLAM_PLATFORM_WINDOWS
static inline int llam_handle_is_invalid(llam_handle_t handle) {
    return handle == NULL || handle == LLAM_INVALID_HANDLE;
}
#else
static inline int llam_handle_is_invalid(llam_handle_t handle) {
    return handle == LLAM_INVALID_HANDLE;
}
#endif

/** @brief Non-zero when @p handle is the platform invalid generic handle value. */
#define LLAM_HANDLE_IS_INVALID(handle) (llam_handle_is_invalid((handle)))

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

/**
 * @brief Public symbol visibility for the canonical LLAM ABI.
 *
 * @details
 * Define @c LLAM_BUILD_SHARED while building the LLAM shared library. Consumers
 * may define @c LLAM_SHARED when they want Windows dllimport annotations; the
 * declarations also remain link-compatible without it.
 */
#if LLAM_PLATFORM_WINDOWS && defined(LLAM_BUILD_SHARED)
#define LLAM_API __declspec(dllexport)
#elif LLAM_PLATFORM_WINDOWS && defined(LLAM_SHARED)
#define LLAM_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(LLAM_BUILD_SHARED)
#define LLAM_API __attribute__((visibility("default")))
#else
#define LLAM_API
#endif

#endif
