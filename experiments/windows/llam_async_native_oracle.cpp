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

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <cstdint>

extern "C" {
volatile LONG g_win_llam_async_cleanup_count = 0;
}

struct win_local_cleanup {
    ~win_local_cleanup() noexcept {
        InterlockedIncrement(&g_win_llam_async_cleanup_count);
    }
};

extern "C" int win_llam_async_native_oracle(void) noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    volatile std::uint32_t marker = 0x6c6c616dU;

    GetCurrentThreadStackLimits(&low, &high);
    if (reinterpret_cast<ULONG_PTR>(&marker) < low ||
        reinterpret_cast<ULONG_PTR>(&marker) >= high) {
        return -3;
    }
    try {
        win_local_cleanup cleanup;
        throw 42;
    } catch (int value) {
        return value;
    } catch (...) {
        return -2;
    }
}
