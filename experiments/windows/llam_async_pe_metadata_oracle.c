/*
 * Copyright 2026 Feralthedogg
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

/* Private PE/LLPM collector, temporary pin and public-enable handoff. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>
#include <string.h>

typedef uintptr_t (*pe_bound_fn_t)(void);
typedef void (*pe_void_fn_t)(void);

typedef struct pe_run_state {
    llam_runtime_t *runtime;
    pe_void_fn_t run_leaf;
    pe_void_fn_t release_leaf;
    atomic_uint entered;
    atomic_uint done;
} pe_run_state_t;

static void pe_fail(const char *where) {
    fprintf(stderr, "llam-async-pe: %s failed (errno=%d)\n", where, errno);
    ExitProcess(1U);
}

static pe_bound_fn_t pe_bound(HMODULE module, const char *name) {
    FARPROC procedure = GetProcAddress(module, name);
    pe_bound_fn_t function = NULL;

    _Static_assert(sizeof(function) == sizeof(procedure),
                   "Windows function pointer sizes must match");
    memcpy(&function, &procedure, sizeof(function));
    if (function == NULL) {
        pe_fail("GetProcAddress bound");
    }
    return function;
}

static pe_void_fn_t pe_void(HMODULE module, const char *name) {
    FARPROC procedure = GetProcAddress(module, name);
    pe_void_fn_t function = NULL;

    _Static_assert(sizeof(function) == sizeof(procedure),
                   "Windows function pointer sizes must match");
    memcpy(&function, &procedure, sizeof(function));
    if (function == NULL) {
        pe_fail("GetProcAddress leaf");
    }
    return function;
}

static void pe_task(void *opaque) {
    pe_run_state_t *state = (pe_run_state_t *)opaque;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    state->run_leaf();
    atomic_store_explicit(&state->done, 1U, memory_order_release);
}

static DWORD WINAPI pe_monitor(void *opaque) {
    pe_run_state_t *state = (pe_run_state_t *)opaque;
    ULONGLONG deadline = GetTickCount64() + 6000U;

    while (GetTickCount64() < deadline) {
        llam_windows_async_stats_t stats = {0};

        if (atomic_load_explicit(&state->entered,
                                 memory_order_acquire) != 0U &&
            llam_windows_async_lifetime_stats(state->runtime, &stats) &&
            stats.captures != 0U && stats.restores != 0U &&
            stats.direct_witnesses != 0U) {
            state->release_leaf();
            return 0U;
        }
        Sleep(1U);
    }
    pe_fail("metadata leaf automatic capture/restore/witness");
    return 1U;
}

static void pe_join(HANDLE thread) {
    DWORD exit_code = 1U;

    if (thread == NULL || WaitForSingleObject(thread, 4000U) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0U) {
        pe_fail("monitor thread completion");
    }
    CloseHandle(thread);
}

static bool pe_is_mapped(HMODULE module, uintptr_t address) {
    HMODULE observed = NULL;

    return GetModuleHandleExA(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               (LPCSTR)(const void *)address, &observed) &&
           observed == module;
}

static void pe_check_empty(const llam_windows_async_pe_range_set_t *set) {
    if (set->held_module != NULL || set->ranges != NULL ||
        set->range_count != 0U) {
        pe_fail("collector failure retained an output resource");
    }
}

static int pe_positive(HMODULE module) {
    llam_windows_async_pe_range_set_t set = {0};
    uintptr_t begin = pe_bound(module, "win_llam_async_image_begin")();
    uintptr_t end = pe_bound(module, "win_llam_async_image_end")();

    if (llam_windows_async_pe_collect_ranges(module, &set) != 0 ||
        set.held_module != module || set.range_count != 1U ||
        set.ranges == NULL || set.ranges[0].begin_pc != begin ||
        set.ranges[0].end_pc != end || set.ranges[0].flags != 0U) {
        pe_fail("exact LLPM leaf range");
    }
    llam_windows_async_pe_release_ranges(&set);
    pe_check_empty(&set);
    FreeLibrary(module);
    if (pe_is_mapped(module, begin)) {
        pe_fail("positive collector release leaked module reference");
    }
    return 0;
}

static int pe_absent(HMODULE module) {
    llam_windows_async_pe_range_set_t set = {0};
    uintptr_t begin = pe_bound(module, "win_llam_async_image_begin")();

    if (llam_windows_async_pe_collect_ranges(module, &set) !=
        LLAM_WINDOWS_ASYNC_PE_METADATA_ABSENT) {
        pe_fail("absent LLPM section");
    }
    pe_check_empty(&set);
    FreeLibrary(module);
    if (pe_is_mapped(module, begin)) {
        pe_fail("absent collector leaked module reference");
    }
    return 0;
}

static int pe_rejected(HMODULE module, int expected_errno) {
    llam_windows_async_pe_range_set_t set = {0};
    uintptr_t begin = pe_bound(module, "win_llam_async_image_begin")();

    errno = 0;
    if (llam_windows_async_pe_collect_ranges(module, &set) != -1 ||
        errno != expected_errno) {
        pe_fail("rejected LLPM record");
    }
    pe_check_empty(&set);
    FreeLibrary(module);
    if (pe_is_mapped(module, begin)) {
        pe_fail("rejected collector leaked module reference");
    }
    return 0;
}

static int pe_invalid(HMODULE module) {
    llam_windows_async_pe_range_set_t set = {0};
    void *private_page = VirtualAlloc(NULL, 4096U,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
    uintptr_t address = pe_bound(module, "win_llam_async_image_begin")();

    if (private_page == NULL) {
        pe_fail("private fake module allocation");
    }
    if (llam_windows_async_pe_collect_ranges((HMODULE)private_page,
                                              &set) != -1) {
        pe_fail("MEM_PRIVATE fake HMODULE accepted");
    }
    pe_check_empty(&set);
    if (llam_windows_async_pe_collect_ranges(
            (HMODULE)((uintptr_t)module + 1U), &set) != -1 ||
        errno != EINVAL) {
        pe_fail("interior non-base HMODULE accepted");
    }
    pe_check_empty(&set);
    VirtualFree(private_page, 0U, MEM_RELEASE);
    FreeLibrary(module);
    if (pe_is_mapped(module, address)) {
        pe_fail("invalid collection leaked module reference");
    }
    return 0;
}

static int pe_pin(HMODULE module) {
    llam_windows_async_pe_range_set_t set = {0};
    uintptr_t begin = pe_bound(module, "win_llam_async_image_begin")();

    if (llam_windows_async_pe_collect_ranges(module, &set) != 0 ||
        !FreeLibrary(module) || !pe_is_mapped(module, begin)) {
        pe_fail("temporary collector module pin");
    }
    llam_windows_async_pe_release_ranges(&set);
    pe_check_empty(&set);
    if (pe_is_mapped(module, begin)) {
        pe_fail("collector release did not unload unowned module");
    }
    return 0;
}

static int pe_handoff(HMODULE module) {
    llam_windows_async_pe_range_set_t set = {0};
    llam_windows_async_stats_t stats = {0};
    llam_windows_async_preempt_opts_t async_options = {0};
    llam_runtime_opts_t runtime_options;
    llam_runtime_t *runtime = NULL;
    pe_run_state_t state = {0};
    llam_task_t *task;
    HANDLE monitor;
    uintptr_t begin = pe_bound(module, "win_llam_async_image_begin")();
    int result;

    if (llam_windows_async_pe_collect_ranges(module, &set) != 0 ||
        llam_runtime_opts_init(&runtime_options,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        pe_fail("collect/initialize handoff");
    }
    runtime_options.deterministic = 1U;
    runtime_options.preempt_mode = LLAM_PREEMPT_STRICT;
    if (llam_runtime_create(&runtime_options,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        pe_fail("handoff runtime create");
    }
    async_options.flags =
        LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    async_options.ranges = set.ranges;
    async_options.range_count = set.range_count;
    result = llam_runtime_enable_experimental_windows_async_preempt_ex(
        runtime, &async_options, sizeof(async_options));
    if (result != 0 && errno == ENOTSUP) {
        llam_windows_async_pe_release_ranges(&set);
        llam_runtime_destroy(runtime);
        FreeLibrary(module);
        return 77;
    }
    if (result != 0 || !FreeLibrary(module)) {
        pe_fail("public PE metadata enable");
    }
    llam_windows_async_pe_release_ranges(&set);
    if (!pe_is_mapped(module, begin)) {
        pe_fail("backend did not retain collector image");
    }
    state.runtime = runtime;
    state.run_leaf = pe_void(module, "win_llam_async_image_run_leaf");
    state.release_leaf = pe_void(module, "win_llam_async_image_release_leaf");
    atomic_init(&state.entered, 0U);
    atomic_init(&state.done, 0U);
    task = llam_runtime_spawn_ex(runtime, pe_task, &state, NULL, 0U);
    if (task == NULL || llam_detach(task) != 0) {
        pe_fail("metadata leaf task spawn");
    }
    monitor = CreateThread(NULL, 0U, pe_monitor, &state, 0U, NULL);
    if (monitor == NULL) {
        pe_fail("metadata leaf monitor");
    }
    result = llam_runtime_run_handle(runtime);
    pe_join(monitor);
    if (result != 0 ||
        atomic_load_explicit(&state.done, memory_order_acquire) != 1U ||
        !llam_windows_async_lifetime_stats(runtime, &stats) ||
        stats.captures == 0U || stats.restores == 0U ||
        stats.direct_witnesses == 0U || stats.binding_ops != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 0U ||
        llam_windows_async_lifetime_charged_bytes(runtime) != 0U) {
        pe_fail("metadata leaf run/debt drain");
    }
    llam_runtime_destroy(runtime);
    if (pe_is_mapped(module, begin)) {
        pe_fail("backend did not release image at shutdown");
    }
    return 0;
}

int main(int argc, char **argv) {
    HMODULE module;
    int result;

    if (argc != 3 || argv[1] == NULL || argv[2] == NULL) {
        return 1;
    }
    module = LoadLibraryA(argv[2]);
    if (module == NULL) {
        pe_fail("LoadLibrary fixture");
    }
    if (strcmp(argv[1], "positive") == 0) {
        result = pe_positive(module);
    } else if (strcmp(argv[1], "absent") == 0) {
        result = pe_absent(module);
    } else if (strcmp(argv[1], "malformed") == 0) {
        result = pe_rejected(module, EINVAL);
    } else if (strcmp(argv[1], "unsupported") == 0) {
        result = pe_rejected(module, ENOTSUP);
    } else if (strcmp(argv[1], "invalid") == 0) {
        result = pe_invalid(module);
    } else if (strcmp(argv[1], "pin") == 0) {
        result = pe_pin(module);
    } else if (strcmp(argv[1], "handoff") == 0) {
        result = pe_handoff(module);
    } else {
        FreeLibrary(module);
        return 1;
    }
    if (result == 0) {
        printf("llam-async-pe: %s passed\n", argv[1]);
    }
    return result;
}
