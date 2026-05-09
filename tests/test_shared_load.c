/**
 * @file tests/test_shared_load.c
 * @brief Dynamic-library loading test for the stable LLAM ABI surface.
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

#include "llam/runtime.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t (*abi_version_fn)(void);
typedef const char *(*version_string_fn)(void);
typedef int (*abi_info_fn)(llam_abi_info_t *info, size_t info_size);
typedef int (*connect_fn)(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);

#if defined(__APPLE__) && defined(__MACH__)
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.1.dylib"
#elif defined(__linux__)
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.so.1.0.0"
#else
#define LLAM_TEST_DEFAULT_SHARED_PATH "./libllam_runtime.so"
#endif

#define LOAD_FN(handle, name, out)                                                           \
    do {                                                                                     \
        void *symbol__ = dlsym((handle), (name));                                            \
        _Static_assert(sizeof(out) == sizeof(symbol__), "function pointer size mismatch");   \
        if (symbol__ == NULL) {                                                              \
            fprintf(stderr, "[test_shared_load] missing symbol %s: %s\n", (name), dlerror()); \
            return 1;                                                                        \
        }                                                                                    \
        memcpy(&(out), &symbol__, sizeof(out));                                              \
    } while (0)

static int test_fail(const char *message) {
    fprintf(stderr, "[test_shared_load] %s\n", message);
    return 1;
}

static int require_symbol(void *handle, const char *name) {
    void *symbol;

    (void)dlerror();
    symbol = dlsym(handle, name);
    if (symbol == NULL) {
        const char *error = dlerror();

        fprintf(stderr,
                "[test_shared_load] missing public symbol %s: %s\n",
                name,
                error != NULL ? error : "unknown dlerror");
        return 1;
    }
    return 0;
}

static int require_symbols(void *handle, const char *const *symbols, size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (require_symbol(handle, symbols[i]) != 0) {
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
        "llam_spawn_opts_init",
        "llam_runtime_init_ex",
        "llam_runtime_init",
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
        "llam_channel_send_until",
        "llam_channel_recv_result",
        "llam_channel_recv_until_result",
        "llam_channel_recv",
        "llam_channel_recv_until",
        "llam_channel_close",
        "llam_read",
        "llam_read_handle",
        "llam_read_when_ready",
        "llam_write",
        "llam_write_handle",
        "llam_read_owned",
        "llam_recv_owned",
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
    const char *path = argc > 1 ? argv[1] : LLAM_TEST_DEFAULT_SHARED_PATH;
    char expected_version[32];
    abi_version_fn llam_abi_version_ptr = NULL;
    version_string_fn llam_version_string_ptr = NULL;
    abi_info_fn llam_abi_get_info_ptr = NULL;
    connect_fn llam_connect_ptr = NULL;
    llam_abi_info_t info;
    void *handle;

    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "[test_shared_load] dlopen(%s) failed: %s\n", path, dlerror());
        return 1;
    }

    LOAD_FN(handle, "llam_abi_version", llam_abi_version_ptr);
    LOAD_FN(handle, "llam_version_string", llam_version_string_ptr);
    LOAD_FN(handle, "llam_abi_get_info", llam_abi_get_info_ptr);
    LOAD_FN(handle, "llam_connect", llam_connect_ptr);
    if (require_symbols(handle, llam_symbols, sizeof(llam_symbols) / sizeof(llam_symbols[0])) != 0) {
        (void)dlclose(handle);
        return 1;
    }

    if (llam_abi_version_ptr() != LLAM_ABI_VERSION) {
        (void)dlclose(handle);
        return test_fail("loaded ABI version does not match headers");
    }
    (void)snprintf(expected_version, sizeof(expected_version), "%u.%u.%u",
                   LLAM_VERSION_MAJOR, LLAM_VERSION_MINOR, LLAM_VERSION_PATCH);
    if (llam_version_string_ptr() == NULL || strcmp(llam_version_string_ptr(), expected_version) != 0) {
        (void)dlclose(handle);
        return test_fail("loaded version string is unexpected");
    }
    memset(&info, 0, sizeof(info));
    if (llam_abi_get_info_ptr(&info, LLAM_ABI_INFO_CURRENT_SIZE) != 0) {
        fprintf(stderr, "[test_shared_load] loaded llam_abi_get_info failed: errno=%d (%s)\n", errno, strerror(errno));
        (void)dlclose(handle);
        return 1;
    }
    if (info.abi_major != LLAM_ABI_VERSION_MAJOR ||
        info.abi_minor != LLAM_ABI_VERSION_MINOR ||
        info.runtime_name == NULL ||
        strcmp(info.runtime_name, "LLAM") != 0) {
        (void)dlclose(handle);
        return test_fail("loaded ABI metadata is inconsistent");
    }
    if (llam_connect_ptr == NULL) {
        (void)dlclose(handle);
        return test_fail("llam_connect symbol resolved to NULL");
    }

    (void)dlclose(handle);
    printf("[test_shared_load] ok\n");
    return 0;
}
