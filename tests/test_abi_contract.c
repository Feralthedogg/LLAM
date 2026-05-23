/**
 * @file tests/test_abi_contract.c
 * @brief Public ABI/version contract tests for the canonical LLAM API.
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

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct abi_prefix {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t version_major;
} abi_prefix_t;

#define ASSERT_FIELD_U32(type, field) \
    _Static_assert(sizeof(((type *)0)->field) == sizeof(uint32_t), #type "." #field " must be fixed-width")
#define ASSERT_EXPR_TYPE(expr, type, message) \
    _Static_assert(_Generic((expr), type: 1, default: 0), message)

ASSERT_FIELD_U32(llam_spawn_opts_t, task_class);
ASSERT_FIELD_U32(llam_spawn_opts_t, stack_class);
ASSERT_FIELD_U32(llam_spawn_opts_t, flags);
ASSERT_FIELD_U32(llam_spawn_opts_t, reserved0);
_Static_assert(offsetof(llam_spawn_opts_t, reserved0) == offsetof(llam_spawn_opts_t, flags) + sizeof(uint32_t),
               "llam_spawn_opts_t.reserved0 must occupy the post-flags ABI padding");
_Static_assert(offsetof(llam_spawn_opts_t, deadline_ns) == offsetof(llam_spawn_opts_t, reserved0) + sizeof(uint32_t),
               "llam_spawn_opts_t.deadline_ns offset must not move after reserved0");
ASSERT_FIELD_U32(llam_runtime_opts_t, deterministic);
ASSERT_FIELD_U32(llam_runtime_opts_t, forced_yield_every);
ASSERT_FIELD_U32(llam_runtime_opts_t, idle_spin_max_iters);
ASSERT_FIELD_U32(llam_runtime_opts_t, profile);
ASSERT_FIELD_U32(llam_runtime_opts_t, reserved0);
ASSERT_FIELD_U32(llam_runtime_opts_t, preempt_mode);
ASSERT_FIELD_U32(llam_runtime_opts_t, preempt_poll_period);
_Static_assert(sizeof(((llam_runtime_opts_t *)0)->experimental_flags) == sizeof(uint64_t),
               "llam_runtime_opts_t.experimental_flags must be fixed-width");
_Static_assert(sizeof(((llam_runtime_opts_t *)0)->preempt_quantum_ns) == sizeof(uint64_t),
               "llam_runtime_opts_t.preempt_quantum_ns must be fixed-width");
_Static_assert(sizeof(((llam_runtime_opts_t *)0)->sqpoll_cpu) == sizeof(int32_t),
               "llam_runtime_opts_t.sqpoll_cpu must be fixed-width");
ASSERT_FIELD_U32(llam_runtime_stats_t, active_workers);
ASSERT_FIELD_U32(llam_runtime_stats_t, online_workers);
ASSERT_FIELD_U32(llam_runtime_stats_t, online_workers_floor);
ASSERT_FIELD_U32(llam_runtime_stats_t, online_workers_min);
ASSERT_FIELD_U32(llam_runtime_stats_t, online_workers_max);
ASSERT_FIELD_U32(llam_runtime_stats_t, active_nodes);
ASSERT_FIELD_U32(llam_runtime_stats_t, dynamic_workers);
ASSERT_FIELD_U32(llam_runtime_stats_t, worker_rings);
ASSERT_FIELD_U32(llam_runtime_stats_t, worker_rings_multishot);
ASSERT_FIELD_U32(llam_runtime_stats_t, lockfree_normq);
ASSERT_FIELD_U32(llam_runtime_stats_t, huge_alloc);
ASSERT_FIELD_U32(llam_runtime_stats_t, sqpoll);
ASSERT_FIELD_U32(llam_runtime_stats_t, preempt_mode);
ASSERT_FIELD_U32(llam_runtime_stats_t, preempt_poll_period);
ASSERT_EXPR_TYPE(llam_task_class((const llam_task_t *)0), uint32_t,
                 "llam_task_class result must be fixed-width");
ASSERT_EXPR_TYPE(llam_task_flags((const llam_task_t *)0), uint32_t,
                 "llam_task_flags result must be fixed-width");
ASSERT_EXPR_TYPE(llam_task_set_class(LLAM_TASK_CLASS_DEFAULT), int,
                 "llam_task_set_class result must be int");
_Static_assert(sizeof(((llam_mut_iovec_t *)0)->iov_base) == sizeof(void *),
               "llam_mut_iovec_t.iov_base must be pointer-sized");
_Static_assert(sizeof(((llam_mut_iovec_t *)0)->iov_len) == sizeof(size_t),
               "llam_mut_iovec_t.iov_len must be size_t");
_Static_assert(offsetof(llam_io_buffer_opts_t, alignment) == sizeof(size_t),
               "llam_io_buffer_opts_t.alignment offset must follow capacity");
ASSERT_FIELD_U32(llam_io_buffer_opts_t, flags);
ASSERT_FIELD_U32(llam_io_buffer_opts_t, reserved0);
_Static_assert(LLAM_RUNTIME_OPTS_CURRENT_SIZE == sizeof(llam_runtime_opts_t),
               "LLAM_RUNTIME_OPTS_CURRENT_SIZE must match llam_runtime_opts_t");
_Static_assert(LLAM_SPAWN_OPTS_CURRENT_SIZE == sizeof(llam_spawn_opts_t),
               "LLAM_SPAWN_OPTS_CURRENT_SIZE must match llam_spawn_opts_t");
_Static_assert(LLAM_RUNTIME_STATS_CURRENT_SIZE == sizeof(llam_runtime_stats_t),
               "LLAM_RUNTIME_STATS_CURRENT_SIZE must match llam_runtime_stats_t");
_Static_assert(LLAM_IO_BUFFER_OPTS_CURRENT_SIZE == sizeof(llam_io_buffer_opts_t),
               "LLAM_IO_BUFFER_OPTS_CURRENT_SIZE must match llam_io_buffer_opts_t");

#undef ASSERT_FIELD_U32

static int test_fail(const char *message) {
    fprintf(stderr, "[test_abi_contract] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_abi_contract] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static llam_fd_t invalid_fd_with_side_effect(unsigned *calls) {
    *calls += 1U;
    return LLAM_INVALID_FD;
}

static llam_handle_t invalid_handle_with_side_effect(unsigned *calls) {
    *calls += 1U;
    return LLAM_INVALID_HANDLE;
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
        info.runtime_opts_size != LLAM_RUNTIME_OPTS_CURRENT_SIZE ||
        info.spawn_opts_size != LLAM_SPAWN_OPTS_CURRENT_SIZE ||
        info.runtime_stats_size != LLAM_RUNTIME_STATS_CURRENT_SIZE) {
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
    if (llam_runtime_opts_init(NULL, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_opts_init(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_spawn_opts_init((llam_spawn_opts_t *)(void *)&errno, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_spawn_opts_init(size=0) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_io_buffer_opts_init(NULL, LLAM_IO_BUFFER_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return test_fail("llam_io_buffer_opts_init(NULL) did not fail with EINVAL");
    }
    return 0;
}

static int test_platform_fd_contracts(void) {
    llam_fd_t llam_fd = LLAM_INVALID_FD;
    llam_handle_t llam_handle = LLAM_INVALID_HANDLE;
    unsigned calls = 0U;

    if (!LLAM_FD_IS_INVALID(llam_fd)) {
        return test_fail("LLAM invalid fd predicate does not recognize LLAM_INVALID_FD");
    }
    if (!LLAM_HANDLE_IS_INVALID(llam_handle)) {
        return test_fail("LLAM invalid handle predicate does not recognize LLAM_INVALID_HANDLE");
    }
    if (!LLAM_FD_IS_INVALID(invalid_fd_with_side_effect(&calls)) || calls != 1U) {
        return test_fail("LLAM fd invalid predicate evaluated its argument more than once");
    }
    calls = 0U;
    if (!LLAM_HANDLE_IS_INVALID(invalid_handle_with_side_effect(&calls)) || calls != 1U) {
        return test_fail("LLAM handle invalid predicate evaluated its argument more than once");
    }
    return 0;
}

static int test_llam_option_initializers(void) {
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_io_buffer_opts_t buffer_opts;
    uint32_t prefix_value;

    memset(&runtime_opts, 0xA5, sizeof(runtime_opts));
    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("llam_runtime_opts_init full-size call failed");
    }
    if (runtime_opts.deterministic != 1U ||
        runtime_opts.sqpoll_cpu != -1 ||
        runtime_opts.profile != LLAM_RUNTIME_PROFILE_BALANCED ||
        runtime_opts.experimental_flags != 0U) {
        return test_fail("llam runtime option defaults are inconsistent");
    }

    memset(&spawn_opts, 0xA5, sizeof(spawn_opts));
    if (llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("llam_spawn_opts_init full-size call failed");
    }
    if (spawn_opts.task_class != LLAM_TASK_CLASS_DEFAULT ||
        spawn_opts.stack_class != LLAM_STACK_CLASS_DEFAULT ||
        spawn_opts.flags != 0U ||
        spawn_opts.reserved0 != 0U ||
        spawn_opts.cancel_token != NULL) {
        return test_fail("llam spawn option defaults are inconsistent");
    }

    memset(&buffer_opts, 0xA5, sizeof(buffer_opts));
    if (llam_io_buffer_opts_init(&buffer_opts, LLAM_IO_BUFFER_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("llam_io_buffer_opts_init full-size call failed");
    }
    if (buffer_opts.capacity != 0U ||
        buffer_opts.alignment != 0U ||
        buffer_opts.flags != 0U ||
        buffer_opts.reserved0 != 0U) {
        return test_fail("llam io buffer option defaults are inconsistent");
    }

    memset(&runtime_opts, 0xA5, sizeof(runtime_opts));
    if (llam_runtime_opts_init(&runtime_opts, sizeof(prefix_value)) != 0) {
        return test_fail_errno("llam_runtime_opts_init prefix call failed");
    }
    memcpy(&prefix_value, &runtime_opts, sizeof(prefix_value));
    if (prefix_value != 1U) {
        return test_fail("llam runtime option prefix init returned wrong deterministic default");
    }
    return 0;
}

static int test_preempt_poll_macro_hygiene(void) {
    size_t llam_preempt_poll_counter_ = 0U;
    size_t llam_preempt_poll_interval_ = 2U;
    size_t side_effect_counter = 0U;
    size_t side_effect_interval = 2U;

    /*
     * Public macros must be safe in ordinary user scopes.  This intentionally
     * uses the names from the old macro implementation to catch accidental
     * reintroduction of implementation-local declarations.
     */
    LLAM_PREEMPT_POLL_EVERY(llam_preempt_poll_counter_, llam_preempt_poll_interval_);
    llam_preempt_poll_counter_ += 1U;
    LLAM_PREEMPT_POLL_EVERY(llam_preempt_poll_counter_, 0U);
    LLAM_PREEMPT_POLL_EVERY(side_effect_counter++, side_effect_interval++);
    if (side_effect_counter != 1U || side_effect_interval != 3U) {
        return test_fail("LLAM_PREEMPT_POLL_EVERY evaluated an argument more than once");
    }
    return 0;
}

int main(void) {
    if (test_llam_full_info() != 0 ||
        test_llam_prefix_info() != 0 ||
        test_llam_option_initializers() != 0 ||
        test_preempt_poll_macro_hygiene() != 0 ||
        test_platform_fd_contracts() != 0 ||
        test_invalid_arguments() != 0) {
        return 1;
    }
    printf("[test_abi_contract] ok\n");
    return 0;
}
