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

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : LLAM_TEST_DEFAULT_SHARED_PATH;
    abi_version_fn llam_abi_version_ptr = NULL;
    version_string_fn llam_version_string_ptr = NULL;
    abi_info_fn llam_abi_get_info_ptr = NULL;
    abi_version_fn nm_abi_version_ptr = NULL;
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
    LOAD_FN(handle, "nm_abi_version", nm_abi_version_ptr);

    if (llam_abi_version_ptr() != LLAM_ABI_VERSION || nm_abi_version_ptr() != LLAM_ABI_VERSION) {
        (void)dlclose(handle);
        return test_fail("loaded ABI version does not match headers");
    }
    if (llam_version_string_ptr() == NULL || strcmp(llam_version_string_ptr(), "0.1.0") != 0) {
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
