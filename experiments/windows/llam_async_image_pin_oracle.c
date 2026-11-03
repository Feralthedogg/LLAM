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
 * See LICENSES/Apache-2.0.txt.
 */

/* The safe PC's image remains mapped after the caller drops its DLL handle. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <errno.h>
#include <stdio.h>

typedef uintptr_t (*image_bound_fn_t)(void);

static void image_pin_fail(const char *where) {
    fprintf(stderr, "llam-async-image-pin: %s failed\n", where);
    ExitProcess(1U);
}

int main(int argc, char **argv) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime;
    HMODULE module;
    HMODULE observed = NULL;
    image_bound_fn_t begin_fn;
    image_bound_fn_t end_fn;
    uintptr_t begin;
    uintptr_t end;
    void *jit_code;

    if (argc != 2 || argv[1] == NULL) {
        return 1;
    }
    module = LoadLibraryA(argv[1]);
    if (module == NULL) {
        image_pin_fail("LoadLibrary");
    }
    begin_fn = (image_bound_fn_t)GetProcAddress(module,
                                                "win_llam_async_image_begin");
    end_fn = (image_bound_fn_t)GetProcAddress(module,
                                              "win_llam_async_image_end");
    if (begin_fn == NULL || end_fn == NULL) {
        image_pin_fail("GetProcAddress");
    }
    begin = begin_fn();
    end = end_fn();
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        image_pin_fail("runtime opts");
    }
    options.deterministic = 1U;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        image_pin_fail("runtime init");
    }
    runtime = llam_runtime_default_storage();
    if (llam_windows_async_lifetime_register_safe_range(runtime,
                                                        begin, end) != 0 ||
        llam_windows_async_lifetime_enable(runtime) != 0) {
        if (errno == ENOTSUP) {
            llam_runtime_shutdown();
            FreeLibrary(module);
            return 77;
        }
        image_pin_fail("register/enable image");
    }
    if (!FreeLibrary(module) ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(const void *)begin, &observed) ||
        observed != module) {
        image_pin_fail("image was not retained");
    }
    llam_runtime_shutdown();
    observed = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(const void *)begin, &observed)) {
        image_pin_fail("image pin was not released");
    }

    jit_code = VirtualAlloc(NULL, 4096U, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    if (jit_code == NULL ||
        llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        image_pin_fail("JIT control init");
    }
    runtime = llam_runtime_default_storage();
    if (llam_windows_async_lifetime_register_safe_range(
            runtime, (uintptr_t)jit_code,
            (uintptr_t)jit_code + 16U) != 0) {
        image_pin_fail("JIT range registration");
    }
    errno = 0;
    if (llam_windows_async_lifetime_enable(runtime) == 0 ||
        errno != ENOTSUP) {
        image_pin_fail("JIT range was accepted");
    }
    llam_runtime_shutdown();
    if (!VirtualFree(jit_code, 0U, MEM_RELEASE)) {
        image_pin_fail("VirtualFree");
    }
    puts("llam-async-image-pin: image retained, released, JIT rejected");
    return 0;
}
