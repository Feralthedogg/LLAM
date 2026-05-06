/**
 * @file tests/test_abi_contract.c
 * @brief Public ABI/version contract tests for canonical and compatibility APIs.
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

#include "llam/nm_runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct abi_prefix {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t version_major;
} abi_prefix_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_abi_contract] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_abi_contract] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int test_llam_full_info(void) {
    llam_abi_info_t info;

    memset(&info, 0xA5, sizeof(info));
    if (llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE) != 0) {
        return test_fail_errno("llam_abi_get_info full-size call failed");
    }
    if (llam_abi_version() != LLAM_ABI_VERSION) {
        return test_fail("llam_abi_version does not match header macro");
    }
    if (info.abi_major != LLAM_ABI_VERSION_MAJOR || info.abi_minor != LLAM_ABI_VERSION_MINOR) {
        return test_fail("llam ABI version fields do not match header macros");
    }
    if (info.version_major != LLAM_VERSION_MAJOR ||
        info.version_minor != LLAM_VERSION_MINOR ||
        info.version_patch != LLAM_VERSION_PATCH) {
        return test_fail("llam source version fields do not match header macros");
    }
    if (info.struct_size != sizeof(llam_abi_info_t) ||
        info.runtime_opts_size != sizeof(llam_runtime_opts_t) ||
        info.spawn_opts_size != sizeof(llam_spawn_opts_t) ||
        info.runtime_stats_size != sizeof(llam_runtime_stats_t)) {
        return test_fail("llam ABI size metadata is inconsistent");
    }
    if (info.runtime_name == NULL || strcmp(info.runtime_name, "LLAM") != 0) {
        return test_fail("llam runtime_name is not stable");
    }
    if (info.version_string == NULL || strcmp(info.version_string, llam_version_string()) != 0) {
        return test_fail("llam version_string does not match accessor");
    }
    if (info.platform_name == NULL || strcmp(info.platform_name, LLAM_PLATFORM_NAME) != 0) {
        return test_fail("llam platform_name does not match platform macro");
    }
    return 0;
}

static int test_llam_prefix_info(void) {
    unsigned char storage[sizeof(abi_prefix_t)];
    abi_prefix_t prefix;

    memset(storage, 0xA5, sizeof(storage));
    if (llam_abi_get_info((llam_abi_info_t *)(void *)storage, sizeof(storage)) != 0) {
        return test_fail_errno("llam_abi_get_info prefix-size call failed");
    }
    memcpy(&prefix, storage, sizeof(prefix));
    if (prefix.abi_major != LLAM_ABI_VERSION_MAJOR ||
        prefix.abi_minor != LLAM_ABI_VERSION_MINOR ||
        prefix.version_major != LLAM_VERSION_MAJOR) {
        return test_fail("llam ABI prefix handshake returned wrong fields");
    }
    return 0;
}

static int test_nm_full_info(void) {
    nm_abi_info_t info;

    memset(&info, 0xA5, sizeof(info));
    if (nm_abi_get_info(&info, NM_ABI_INFO_CURRENT_SIZE) != 0) {
        return test_fail_errno("nm_abi_get_info full-size call failed");
    }
    if (nm_abi_version() != NM_ABI_VERSION || nm_abi_version() != llam_abi_version()) {
        return test_fail("nm ABI version does not match expected compatibility value");
    }
    if (info.abi_major != NM_ABI_VERSION_MAJOR || info.abi_minor != NM_ABI_VERSION_MINOR) {
        return test_fail("nm ABI version fields do not match header macros");
    }
    if (info.struct_size != sizeof(nm_abi_info_t) ||
        info.runtime_opts_size != sizeof(nm_runtime_opts_t) ||
        info.spawn_opts_size != sizeof(nm_spawn_opts_t) ||
        info.runtime_stats_size != sizeof(nm_runtime_stats_t)) {
        return test_fail("nm ABI size metadata is inconsistent");
    }
    if (info.runtime_name == NULL || strcmp(info.runtime_name, "LLAM") != 0) {
        return test_fail("nm runtime_name is not stable");
    }
    if (info.version_string == NULL || strcmp(info.version_string, nm_version_string()) != 0) {
        return test_fail("nm version_string does not match accessor");
    }
    if (info.platform_name == NULL || strcmp(info.platform_name, NM_PLATFORM_NAME) != 0) {
        return test_fail("nm platform_name does not match platform macro");
    }
    return 0;
}

static int test_invalid_arguments(void) {
    errno = 0;
    if (llam_abi_get_info(NULL, LLAM_ABI_INFO_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return test_fail("llam_abi_get_info(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_abi_get_info((llam_abi_info_t *)(void *)&errno, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_abi_get_info(size=0) did not fail with EINVAL");
    }
    errno = 0;
    if (nm_abi_get_info(NULL, NM_ABI_INFO_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return test_fail("nm_abi_get_info(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (nm_abi_get_info((nm_abi_info_t *)(void *)&errno, 0U) != -1 || errno != EINVAL) {
        return test_fail("nm_abi_get_info(size=0) did not fail with EINVAL");
    }
    return 0;
}

int main(void) {
    if (test_llam_full_info() != 0 ||
        test_llam_prefix_info() != 0 ||
        test_nm_full_info() != 0 ||
        test_invalid_arguments() != 0) {
        return 1;
    }
    printf("[test_abi_contract] ok\n");
    return 0;
}
