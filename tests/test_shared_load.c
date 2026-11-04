/**
 * @file tests/test_shared_load.c
 * @brief Dynamic-library loading test for the stable LLAM ABI surface.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "llam/runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef uint32_t (*abi_version_fn)(void);
typedef const char *(*version_string_fn)(void);
typedef int (*abi_info_fn)(llam_abi_info_t *info, size_t info_size);
typedef int (*connect_fn)(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);

#if defined(_WIN32)
typedef HMODULE llam_test_shared_handle_t;

static llam_test_shared_handle_t llam_test_shared_open(const char *path) {
    return LoadLibraryA(path);
}

static void *llam_test_shared_symbol(llam_test_shared_handle_t handle,
                                     const char *name) {
    FARPROC procedure = GetProcAddress(handle, name);
    void *symbol = NULL;

    _Static_assert(sizeof(procedure) == sizeof(symbol),
                   "Windows procedure and object pointer sizes must match");
    memcpy(&symbol, &procedure, sizeof(symbol));
    return symbol;
}

static const char *llam_test_shared_error(void) {
    static char message[48];

    (void)snprintf(message, sizeof(message), "GetLastError=%lu",
                   (unsigned long)GetLastError());
    return message;
}

static void llam_test_shared_close(llam_test_shared_handle_t handle) {
    (void)FreeLibrary(handle);
}
#else
typedef void *llam_test_shared_handle_t;

static llam_test_shared_handle_t llam_test_shared_open(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *llam_test_shared_symbol(llam_test_shared_handle_t handle,
                                     const char *name) {
    (void)dlerror();
    return dlsym(handle, name);
}

static const char *llam_test_shared_error(void) {
    const char *message = dlerror();

    return message != NULL ? message : "unknown dlerror";
}

static void llam_test_shared_close(llam_test_shared_handle_t handle) {
    (void)dlclose(handle);
}
#endif

#if defined(_WIN32)
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.dll"
#elif defined(__APPLE__) && defined(__MACH__)
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.2.dylib"
#elif defined(__linux__)
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.so.3.0.0"
#else
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.so"
#endif

#define LOAD_FN(handle, name, out)                                                           \
    do {                                                                                     \
        void *symbol__ = llam_test_shared_symbol((handle), (name));                         \
        _Static_assert(sizeof(out) == sizeof(symbol__), "function pointer size mismatch");   \
        if (symbol__ == NULL) {                                                              \
            fprintf(stderr, "[test_shared_load] missing symbol %s: %s\n",                  \
                    (name), llam_test_shared_error());                                      \
            return 1;                                                                        \
        }                                                                                    \
        memcpy(&(out), &symbol__, sizeof(out));                                              \
    } while (0)

static int test_fail(const char *message) {
    fprintf(stderr, "[test_shared_load] %s\n", message);
    return 1;
}

static int require_symbol(llam_test_shared_handle_t handle, const char *name) {
    void *symbol;

    symbol = llam_test_shared_symbol(handle, name);
    if (symbol == NULL) {
        fprintf(stderr,
                "[test_shared_load] missing public symbol %s: %s\n",
                name,
                llam_test_shared_error());
        return 1;
    }
    return 0;
}

static int require_symbols(llam_test_shared_handle_t handle,
                           const char *const *symbols, size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (require_symbol(handle, symbols[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

static int require_absent_symbol(llam_test_shared_handle_t handle,
                                 const char *name) {
    void *symbol;

    symbol = llam_test_shared_symbol(handle, name);
    if (symbol != NULL) {
        fprintf(stderr, "[test_shared_load] internal symbol is exported: %s\n", name);
        return 1;
    }
    return 0;
}

static int require_absent_symbols(llam_test_shared_handle_t handle,
                                  const char *const *symbols, size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (require_absent_symbol(handle, symbols[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    static const char *const llam_symbols[] = {
        "llam_abi_version",
        "llam_version_string",
        "llam_abi_get_info",
        "llam_runtime_opts_init",
        "llam_runtime_enable_experimental_windows_async_preempt_ex",
        "llam_spawn_opts_init",
        "llam_runtime_init_compat_ex",
        "llam_runtime_init_compat",
        "llam_runtime_request_stop",
        "llam_runtime_shutdown",
        "llam_runtime_collect_stats_ex",
        "llam_runtime_collect_stats",
        "llam_runtime_write_stats_json",
        "llam_spawn_ex",
        "llam_spawn",
        "llam_run",
        "llam_yield",
        "llam_task_safepoint",
        "llam_join",
        "llam_join_until",
        "llam_detach",
        "llam_sleep_until",
        "llam_sleep_ns",
        "llam_call_blocking_result",
        "llam_call_blocking",
        "llam_enter_blocking",
        "llam_leave_blocking",
        "llam_task_set_class",
        "llam_dump_runtime_state",
        "llam_task_flags",
        "llam_cancel_token_create",
        "llam_cancel_token_destroy",
        "llam_cancel_token_cancel",
        "llam_cancel_token_is_cancelled",
        "llam_mutex_create",
        "llam_mutex_destroy",
        "llam_mutex_lock",
        "llam_mutex_lock_until",
        "llam_mutex_trylock",
        "llam_mutex_unlock",
        "llam_cond_create",
        "llam_cond_destroy",
        "llam_cond_wait",
        "llam_cond_wait_until",
        "llam_cond_signal",
        "llam_cond_broadcast",
        "llam_channel_create",
        "llam_channel_destroy",
        "llam_channel_send",
        "llam_channel_try_send",
        "llam_channel_send_until",
        "llam_channel_recv_result",
        "llam_channel_try_recv_result",
        "llam_channel_recv_until_result",
        "llam_channel_recv",
        "llam_channel_recv_until",
        "llam_channel_close",
        "llam_read",
        "llam_read_handle",
        "llam_read_when_ready",
        "llam_write",
        "llam_writev",
        "llam_pread",
        "llam_pwrite",
        "llam_preadv",
        "llam_pwritev",
        "llam_write_handle",
        "llam_pread_handle",
        "llam_pwrite_handle",
        "llam_preadv_handle",
        "llam_pwritev_handle",
        "llam_close",
        "llam_close_handle",
        "llam_read_owned",
        "llam_recv_owned",
        "llam_io_buffer_opts_init",
        "llam_io_buffer_alloc_ex",
        "llam_io_buffer_alloc",
        "llam_io_buffer_alloc_aligned",
        "llam_io_buffer_alignment",
        "llam_pread_owned_aligned",
        "llam_pread_handle_owned_aligned",
        "llam_io_buffer_release",
        "llam_io_buffer_data",
        "llam_io_buffer_size",
        "llam_io_buffer_capacity",
        "llam_accept",
        "llam_connect",
        "llam_poll_fd",
        "llam_poll_handle",
        "llam_now_ns",
        "llam_task_id",
        "llam_task_state_name",
        "llam_task_class",
        "llam_current_task",
    };
    static const char *const internal_symbols[] = {
        "llam_ctx_switch",
        "llam_fiber_bootstrap",
        "llam_eventfd_try_claim",
        "llam_linux_futex_wait_private",
        "llam_linux_futex_wait_private_timeout",
        "llam_linux_futex_wake_private",
        "llam_capability_test_force_entropy_failure",
        "llam_broker_test_buffer_free_count",
        "llam_broker_test_buffer_free_count_reset",
        "llam_broker_test_force_subject_entropy_failure",
        "llam_broker_test_force_subject_value",
        "llam_runtime_test_force_live_iter_snapshot_alloc_failure",
        "llam_broker_client_self_test_exchange",
        "llam_broker_client_self_test_local",
        "llam_broker_client_self_test_unix",
        "llam_windows_async_lifetime_enable_certified_ranges",
        "llam_windows_async_pe_collect_ranges",
        "llam_windows_async_pe_release_ranges",
        "llam_windows_async_test_ctrl_entry_gate",
    };
    const char *path = argc > 1 ? argv[1] : LLAM_TEST_DEFAULT_SHARED_PATH;
    char expected_version[32];
    abi_version_fn llam_abi_version_ptr = NULL;
    version_string_fn llam_version_string_ptr = NULL;
    abi_info_fn llam_abi_get_info_ptr = NULL;
    connect_fn llam_connect_ptr = NULL;
    llam_abi_info_t info;
    llam_test_shared_handle_t handle;

    handle = llam_test_shared_open(path);
    if (handle == NULL) {
        fprintf(stderr, "[test_shared_load] open(%s) failed: %s\n",
                path, llam_test_shared_error());
        return 1;
    }

    LOAD_FN(handle, "llam_abi_version", llam_abi_version_ptr);
    LOAD_FN(handle, "llam_version_string", llam_version_string_ptr);
    LOAD_FN(handle, "llam_abi_get_info", llam_abi_get_info_ptr);
    LOAD_FN(handle, "llam_connect", llam_connect_ptr);
    if (require_symbols(handle, llam_symbols, sizeof(llam_symbols) / sizeof(llam_symbols[0])) != 0) {
        llam_test_shared_close(handle);
        return 1;
    }
    /*
     * The shared library may link these helpers internally, but they are not
     * part of the stable C ABI and must not be reachable through the loader.
     */
    if (require_absent_symbols(handle, internal_symbols, sizeof(internal_symbols) / sizeof(internal_symbols[0])) != 0) {
        llam_test_shared_close(handle);
        return 1;
    }

    if (llam_abi_version_ptr() != LLAM_ABI_VERSION) {
        llam_test_shared_close(handle);
        return test_fail("loaded ABI version does not match headers");
    }
    (void)snprintf(expected_version, sizeof(expected_version), "%u.%u.%u",
                   LLAM_VERSION_MAJOR, LLAM_VERSION_MINOR, LLAM_VERSION_PATCH);
    if (llam_version_string_ptr() == NULL || strcmp(llam_version_string_ptr(), expected_version) != 0) {
        llam_test_shared_close(handle);
        return test_fail("loaded version string is unexpected");
    }
    memset(&info, 0, sizeof(info));
    if (llam_abi_get_info_ptr(&info, LLAM_ABI_INFO_CURRENT_SIZE) != 0) {
        fprintf(stderr, "[test_shared_load] loaded llam_abi_get_info failed: errno=%d (%s)\n", errno, strerror(errno));
        llam_test_shared_close(handle);
        return 1;
    }
    if (info.abi_major != LLAM_ABI_VERSION_MAJOR ||
        info.abi_minor != LLAM_ABI_VERSION_MINOR ||
        info.runtime_name == NULL ||
        strcmp(info.runtime_name, "LLAM") != 0) {
        llam_test_shared_close(handle);
        return test_fail("loaded ABI metadata is inconsistent");
    }
    if (llam_connect_ptr == NULL) {
        llam_test_shared_close(handle);
        return test_fail("llam_connect symbol resolved to NULL");
    }

    llam_test_shared_close(handle);
    printf("[test_shared_load] ok\n");
    return 0;
}
