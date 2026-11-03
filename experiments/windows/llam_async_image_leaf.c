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

#include <stdint.h>
#include <stdatomic.h>

extern void win_llam_async_image_leaf(void);
extern const unsigned char win_llam_async_image_safe_begin[];
extern const unsigned char win_llam_async_image_safe_end[];
#if !defined(LLAM_PE_METADATA_ABSENT)
extern const unsigned char win_llam_async_image_metadata_begin[];
#endif

atomic_uint g_win_llam_async_image_release = ATOMIC_VAR_INIT(0U);

__declspec(dllexport) void win_llam_async_image_run_leaf(void) {
    win_llam_async_image_leaf();
}

__declspec(dllexport) void win_llam_async_image_release_leaf(void) {
    atomic_store_explicit(&g_win_llam_async_image_release, 1U,
                          memory_order_release);
}

__declspec(dllexport) uintptr_t win_llam_async_image_begin(void) {
    return (uintptr_t)win_llam_async_image_safe_begin;
}

__declspec(dllexport) uintptr_t win_llam_async_image_end(void) {
    return (uintptr_t)win_llam_async_image_safe_end;
}

#if !defined(LLAM_PE_METADATA_ABSENT)
__declspec(dllexport) uintptr_t win_llam_async_image_metadata(void) {
    return (uintptr_t)win_llam_async_image_metadata_begin;
}
#endif
