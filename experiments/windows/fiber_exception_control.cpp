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

#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" int win_probe_local_exception(void) noexcept;

struct fiber_state {
    void *main_fiber;
    int result;
    unsigned outside_system_stack_limits;
};

static void WINAPI probe_fiber_entry(void *argument) {
    auto *state = static_cast<fiber_state *>(argument);
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    char marker = 0;

    GetCurrentThreadStackLimits(&low, &high);
    state->outside_system_stack_limits =
        reinterpret_cast<std::uintptr_t>(&marker) < static_cast<std::uintptr_t>(low) ||
        reinterpret_cast<std::uintptr_t>(&marker) >= static_cast<std::uintptr_t>(high);
    state->result = win_probe_local_exception();
    SwitchToFiber(state->main_fiber);
    std::abort();
}

int main() {
    fiber_state state{};

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    if (win_probe_local_exception() != 42) {
        std::fprintf(stderr, "fiber-control: original stack exception failed\n");
        return 1;
    }
    std::puts("fiber-control: original stack exception passed");
    state.main_fiber = ConvertThreadToFiber(nullptr);
    if (state.main_fiber == nullptr) {
        std::fprintf(stderr, "fiber-control: ConvertThreadToFiber failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    void *fiber = CreateFiber(64U * 1024U, probe_fiber_entry, &state);
    if (fiber == nullptr) {
        std::fprintf(stderr, "fiber-control: CreateFiber failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    if (!ConvertFiberToThread()) {
        std::fprintf(stderr, "fiber-control: ConvertFiberToThread failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    std::printf("fiber-control: fiber exception=%d outside_system_stack_limits=%u\n",
                state.result, state.outside_system_stack_limits);
    return state.result == 42 ? 0 : 1;
}
