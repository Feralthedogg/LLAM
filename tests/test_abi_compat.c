/**
 * @file tests/test_abi_compat.c
 * @brief Prefix-size ABI compatibility tests that simulate older C bindings.
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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct old_runtime_opts_v1 {
    uint32_t deterministic;
} old_runtime_opts_v1_t;

typedef struct old_spawn_opts_v1 {
    uint32_t task_class;
} old_spawn_opts_v1_t;

typedef struct old_runtime_stats_v1 {
    uint64_t ctx_switches;
    uint64_t yields;
} old_runtime_stats_v1_t;

static atomic_uint g_compat_task_ran;

static int fail_msg(const char *message) {
    fprintf(stderr, "[test_abi_compat] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_abi_compat] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int guard_unchanged(const unsigned char *bytes, size_t len) {
    for (size_t i = 0U; i < len; ++i) {
        if (bytes[i] != 0xA5U) {
            return 0;
        }
    }
    return 1;
}

static void compat_task(void *arg) {
    (void)arg;
    atomic_fetch_add_explicit(&g_compat_task_ran, 1U, memory_order_relaxed);
}

static int test_old_runtime_opts_prefix(void) {
    unsigned char storage[sizeof(old_runtime_opts_v1_t) + 16U];
    old_runtime_opts_v1_t *old_opts = (old_runtime_opts_v1_t *)(void *)storage;

    memset(storage, 0xA5, sizeof(storage));
    if (llam_runtime_opts_init((llam_runtime_opts_t *)(void *)old_opts, sizeof(*old_opts)) != 0) {
        return fail_errno("llam_runtime_opts_init old-prefix call failed");
    }
    if (old_opts->deterministic != 1U) {
        return fail_msg("old runtime opts prefix did not receive default deterministic value");
    }
    if (!guard_unchanged(storage + sizeof(*old_opts), sizeof(storage) - sizeof(*old_opts))) {
        return fail_msg("llam_runtime_opts_init wrote past old runtime opts prefix");
    }
    return 0;
}

static int test_old_spawn_opts_prefix(void) {
    unsigned char storage[sizeof(old_spawn_opts_v1_t) + 16U];
    old_spawn_opts_v1_t *old_opts = (old_spawn_opts_v1_t *)(void *)storage;

    memset(storage, 0xA5, sizeof(storage));
    if (llam_spawn_opts_init((llam_spawn_opts_t *)(void *)old_opts, sizeof(*old_opts)) != 0) {
        return fail_errno("llam_spawn_opts_init old-prefix call failed");
    }
    if (old_opts->task_class != LLAM_TASK_CLASS_DEFAULT) {
        return fail_msg("old spawn opts prefix did not receive default task class");
    }
    if (!guard_unchanged(storage + sizeof(*old_opts), sizeof(storage) - sizeof(*old_opts))) {
        return fail_msg("llam_spawn_opts_init wrote past old spawn opts prefix");
    }
    return 0;
}

static int test_old_binding_runtime_roundtrip(void) {
    old_runtime_opts_v1_t runtime_opts = {
        .deterministic = 1U,
    };
    old_spawn_opts_v1_t spawn_opts = {
        .task_class = LLAM_TASK_CLASS_LATENCY,
    };
    unsigned char stats_storage[sizeof(old_runtime_stats_v1_t) + 16U];
    old_runtime_stats_v1_t *old_stats = (old_runtime_stats_v1_t *)(void *)stats_storage;
    llam_task_t *task;

    atomic_store_explicit(&g_compat_task_ran, 0U, memory_order_relaxed);
    if (llam_runtime_init_ex((const llam_runtime_opts_t *)(const void *)&runtime_opts, sizeof(runtime_opts)) != 0) {
        return fail_errno("llam_runtime_init_ex old-prefix call failed");
    }

    task = llam_spawn_ex(compat_task,
                         NULL,
                         (const llam_spawn_opts_t *)(const void *)&spawn_opts,
                         sizeof(spawn_opts));
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("llam_spawn_ex old-prefix call failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run old-prefix roundtrip failed");
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_join old-prefix task failed");
    }
    if (atomic_load_explicit(&g_compat_task_ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_msg("old-prefix spawned task did not run exactly once");
    }

    memset(stats_storage, 0xA5, sizeof(stats_storage));
    if (llam_runtime_collect_stats_ex((llam_runtime_stats_t *)(void *)old_stats, sizeof(*old_stats)) != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_runtime_collect_stats_ex old-prefix call failed");
    }
    if (!guard_unchanged(stats_storage + sizeof(*old_stats), sizeof(stats_storage) - sizeof(*old_stats))) {
        llam_runtime_shutdown();
        return fail_msg("llam_runtime_collect_stats_ex wrote past old stats prefix");
    }
    llam_runtime_shutdown();
    return 0;
}

int main(void) {
    if (test_old_runtime_opts_prefix() != 0 ||
        test_old_spawn_opts_prefix() != 0 ||
        test_old_binding_runtime_roundtrip() != 0) {
        return 1;
    }
    puts("[test_abi_compat] ok");
    return 0;
}
