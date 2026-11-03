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

/* Isolated Windows x64 context experiment. This is not a LLAM runtime backend. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_RAX UINT64_C(0x1122334455667788)
#define PROBE_YMM_HIGH UINT64_C(0x8877665544332211)
#define PROBE_GPR_BASE UINT64_C(0xabc0000000000000)
#define PROBE_VECTOR_BASE UINT64_C(0x0badc0de00000000)
#define PROBE_AVX_XSTATE (UINT64_C(1) << 2)
#define PROBE_SUPPORTED_XSTATE (UINT64_C(0x7) | XSTATE_MASK_CET_U)
#define PROBE_MXCSR 0x3F80U
#define PROBE_X87CW 0x0B7FU
#define PROBE_LAST_ERROR 0x4A9DU
#define PROBE_WAIT_MS 3000U
#define PROBE_ALT_STACK_BYTES (64U * 1024U)
#define PROBE_STACK_CANARY UINT64_C(0x83b9f3ab2a41c7d5)
#define PROBE_STACK_CHECKSUM (0x3AU + 0x3BU + 0x3CU)

typedef struct probe_observation {
    uint64_t rax;
    uint64_t ymm0[4];
    uint32_t original_mxcsr;
    uint32_t observed_mxcsr;
    uint64_t nonvolatile[8];
    uint64_t ymm6_to_15[10][4];
    uint16_t original_x87cw;
    uint16_t observed_x87cw;
} probe_observation_t;

_Static_assert(offsetof(probe_observation_t, nonvolatile) == 48U,
               "nonvolatile observation offset must match assembly");
_Static_assert(offsetof(probe_observation_t, ymm6_to_15) == 112U,
               "YMM observation offset must match assembly");
_Static_assert(offsetof(probe_observation_t, original_x87cw) == 432U,
               "x87 observation offset must match assembly");

typedef struct probe_state {
    volatile LONG entered;
    volatile LONG start_leaf;
    volatile LONG release;
    volatile LONG landed;
    volatile LONG restore_allowed;
    volatile LONG gate_ready;
    probe_observation_t observed;
    DWORD landing_last_error;
    int landing_errno;
    unsigned landing_outside_system_stack;
    unsigned landing_stack_checksum;
    int landing_exception_value;
    DWORD final_last_error;
    int final_errno;
    int original_exception_value;
    int worker_pre_exception_value;
    uintptr_t native_landing_base;
    uintptr_t native_landing_top;
    uintptr_t saved_native_rsp;
    void *private_stack_top;
    unsigned native_canary_ok;
    unsigned native_landing_inside_system_stack;
} probe_state_t;

typedef struct probe_context {
    void *buffer;
    DWORD length;
    PCONTEXT context;
} probe_context_t;

extern void win_probe_leaf(volatile LONG *release, probe_observation_t *observed);
extern unsigned char win_probe_leaf_begin[];
extern unsigned char win_probe_leaf_end[];
extern void win_restore_gate(volatile LONG *gate_ready);
extern unsigned char win_restore_gate_begin[];
extern unsigned char win_restore_gate_end[];
extern void win_landing_entry(void);
extern unsigned char win_landing_entry_end[];
extern void win_enter_private_g(void *stack_top, volatile LONG *release,
                                probe_observation_t *observed,
                                uintptr_t *native_rsp_out);
extern int win_probe_local_exception(void);

static probe_state_t *g_probe_state;

static LONG probe_flag(volatile LONG *flag) {
    return InterlockedCompareExchange(flag, 0, 0);
}

static int probe_check_extended_registers(const probe_observation_t *observed) {
    unsigned i;

    for (i = 0U; i < 8U; ++i) {
        uint64_t expected = PROBE_GPR_BASE + (uint64_t)i + 1U;

        if (observed->nonvolatile[i] != expected) {
            fprintf(stderr, "context-probe: nonvolatile GPR %u mismatch\n", i);
            return -1;
        }
    }
    for (i = 0U; i < 10U; ++i) {
        uint64_t low = PROBE_VECTOR_BASE + (uint64_t)i + 6U;
        uint64_t high = ~low;

        if (observed->ymm6_to_15[i][0] != low ||
            observed->ymm6_to_15[i][1] != low ||
            observed->ymm6_to_15[i][2] != high ||
            observed->ymm6_to_15[i][3] != high) {
            fprintf(stderr, "context-probe: YMM%u mismatch\n", i + 6U);
            return -1;
        }
    }
    return 0;
}

static int probe_wait_flag(volatile LONG *flag, DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    while (probe_flag(flag) == 0) {
        if (GetTickCount64() >= deadline) {
            return -1;
        }
        Sleep(1);
    }
    return 0;
}

static int probe_context_reset(probe_context_t *storage, DWORD flags, DWORD64 features) {
    DWORD length = storage->length;
    PCONTEXT context = NULL;

    if (!InitializeContext(storage->buffer, flags, &context, &length) ||
        context == NULL || (context->ContextFlags & CONTEXT_XSTATE) == 0U ||
        !SetXStateFeaturesMask(context, features)) {
        return -1;
    }
    storage->context = context;
    return 0;
}

static int probe_context_init(probe_context_t *storage, DWORD flags, DWORD64 features) {
    DWORD length = 0U;
    PCONTEXT context = NULL;

    memset(storage, 0, sizeof(*storage));
    SetLastError(ERROR_SUCCESS);
    if (InitializeContext(NULL, flags, &context, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || length < sizeof(CONTEXT)) {
        return -1;
    }
    storage->buffer = calloc(1U, length);
    if (storage->buffer == NULL) {
        return -1;
    }
    storage->length = length;
    return probe_context_reset(storage, flags, features);
}

/* Success leaves precisely one LLAM-owned suspension on the target. */
static int probe_suspend_in_range(HANDLE thread,
                                  probe_context_t *storage,
                                  DWORD flags,
                                  DWORD64 features,
                                  uintptr_t begin,
                                  uintptr_t end,
                                  unsigned max_attempts) {
    unsigned attempt;

    for (attempt = 0U; attempt < max_attempts; ++attempt) {
        DWORD previous = SuspendThread(thread);
        uintptr_t rip;

        if (previous == (DWORD)-1) {
            return -1;
        }
        if (previous != 0U) {
            DWORD withdrawn = ResumeThread(thread);

            /* Withdraw exactly one increment and retain the other owner. */
            return withdrawn == previous + 1U ? -2 : -1;
        }
        if (probe_context_reset(storage, flags, features) != 0 ||
            !GetThreadContext(thread, storage->context)) {
            (void)ResumeThread(thread);
            return -1;
        }
        rip = (uintptr_t)storage->context->Rip;
        if (rip >= begin && rip < end) {
            return 0;
        }
        if (ResumeThread(thread) != 1U) {
            return -1;
        }
        Sleep(1);
    }
    return -3;
}

static int probe_resume_owned(HANDLE thread) {
    return ResumeThread(thread) == 1U ? 0 : -1;
}

static DWORD WINAPI probe_worker(LPVOID argument) {
    probe_state_t *state = (probe_state_t *)argument;
    _Alignas(16) volatile unsigned char native_landing_area[PROBE_ALT_STACK_BYTES];
    volatile uint64_t *native_canary =
        (volatile uint64_t *)native_landing_area;
    size_t page;

    for (page = 0U; page < PROBE_ALT_STACK_BYTES; page += 4096U) {
        native_landing_area[page] = 0U;
    }
    *native_canary = PROBE_STACK_CANARY;
    state->native_landing_base = (uintptr_t)native_landing_area;
    state->native_landing_top =
        ((uintptr_t)(native_landing_area + PROBE_ALT_STACK_BYTES)) &
        ~(uintptr_t)15U;
    {
        ULONG_PTR stack_low = 0U;
        ULONG_PTR stack_high = 0U;

        GetCurrentThreadStackLimits(&stack_low, &stack_high);
        state->native_landing_inside_system_stack =
            state->native_landing_base >= (uintptr_t)stack_low &&
            state->native_landing_top <= (uintptr_t)stack_high;
    }
    state->worker_pre_exception_value = win_probe_local_exception();
    InterlockedExchange(&state->entered, 1);
    while (probe_flag(&state->start_leaf) == 0) {
        YieldProcessor();
    }
    errno = EDOM;
    SetLastError(PROBE_LAST_ERROR);
    win_enter_private_g(state->private_stack_top, &state->release,
                        &state->observed, &state->saved_native_rsp);
    state->native_canary_ok = *native_canary == PROBE_STACK_CANARY;
    state->final_last_error = GetLastError();
    state->final_errno = errno;
    state->original_exception_value = win_probe_local_exception();
    return 0U;
}

static unsigned __attribute__((noinline)) probe_stack_pages(void) {
    volatile unsigned char frame[3U * 4096U];

    frame[0] = 0x3AU;
    frame[4096] = 0x3BU;
    frame[8192] = 0x3CU;
    return (unsigned)frame[0] + (unsigned)frame[4096] +
           (unsigned)frame[8192];
}

void __attribute__((noreturn, noinline)) win_landing_c(void) {
    probe_state_t *state = g_probe_state;
    DWORD saved_last_error = GetLastError();
    int saved_errno = errno;
    ULONG_PTR stack_low = 0U;
    ULONG_PTR stack_high = 0U;
    char stack_marker = 0;

    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    state->landing_outside_system_stack =
        (uintptr_t)&stack_marker < (uintptr_t)stack_low ||
        (uintptr_t)&stack_marker >= (uintptr_t)stack_high;
    state->landing_stack_checksum = probe_stack_pages();
    state->landing_exception_value = win_probe_local_exception();
    state->landing_last_error = saved_last_error;
    state->landing_errno = saved_errno;
    InterlockedExchange(&state->landed, 1);
    while (probe_flag(&state->restore_allowed) == 0) {
        YieldProcessor();
    }
    errno = saved_errno;
    SetLastError(saved_last_error);
    win_restore_gate(&state->gate_ready);
    abort();
}

static void __attribute__((noreturn, noinline)) probe_landing_returned(void) {
    abort();
}

int main(void) {
    const DWORD flags = CONTEXT_ALL | CONTEXT_XSTATE;
    const DWORD64 features = GetEnabledXStateFeatures();
    probe_context_t captured;
    probe_context_t landing;
    probe_state_t state;
    HANDLE thread = NULL;
    void *alternate_stack = NULL;
    void *private_g_stack = NULL;
    uint64_t *stack_canary = NULL;
    DWORD previous_page_protection = 0U;
    uintptr_t top;
    uintptr_t entry_rsp;
    uintptr_t landing_low;
    uintptr_t landing_high;
    DWORD64 valid_features = 0U;
    DWORD64 post_restore_features = 0U;
    DWORD64 image_base = 0U;
    DWORD64 landing_image_base = 0U;
    DWORD64 gate_image_base = 0U;
    DWORD64 unwind_frame = 0U;
    PRUNTIME_FUNCTION landing_unwind = NULL;
    PRUNTIME_FUNCTION entry_unwind = NULL;
    PRUNTIME_FUNCTION gate_unwind = NULL;
    PVOID unwind_handler_data = NULL;
    CONTEXT unwind_context;
    DWORD cet_bytes = 0U;
    DWORD copied_cet_bytes = 0U;
    void *original_cet = NULL;
    void *copied_cet = NULL;
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY shadow_stack_policy;
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg_policy;
    char corrupt_request[2];
    char native_landing_request[2];
    bool corrupt_restore;
    bool use_native_landing;
    int status;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    corrupt_restore = GetEnvironmentVariableA("LLAM_PROBE_CORRUPT_RBX",
                                               corrupt_request,
                                               sizeof(corrupt_request)) != 0U;
    use_native_landing = GetEnvironmentVariableA("LLAM_PROBE_NATIVE_LANDING",
                                                 native_landing_request,
                                                 sizeof(native_landing_request)) != 0U;
    printf("context-probe: landing_mode=%s\n",
           use_native_landing ? "native-M-stack" : "manual-standalone-stack");
    if (corrupt_restore) {
        puts("context-probe: negative RBX corruption enabled");
    }
    memset(&state, 0, sizeof(state));
    memset(&shadow_stack_policy, 0, sizeof(shadow_stack_policy));
    if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                    ProcessUserShadowStackPolicy,
                                    &shadow_stack_policy,
                                    sizeof(shadow_stack_policy))) {
        printf("context-probe: shadow-stack policy unavailable: %lu\n",
               (unsigned long)GetLastError());
        return 77;
    }
    memset(&cfg_policy, 0, sizeof(cfg_policy));
    if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                    ProcessControlFlowGuardPolicy,
                                    &cfg_policy,
                                    sizeof(cfg_policy))) {
        printf("context-probe: CFG policy unavailable: %lu\n",
               (unsigned long)GetLastError());
        return 77;
    }
    printf("context-probe: shadow_stack_flags=0x%lx cfg_flags=0x%lx\n",
           (unsigned long)shadow_stack_policy.Flags,
           (unsigned long)cfg_policy.Flags);
    if (shadow_stack_policy.EnableUserShadowStack != 0U ||
        shadow_stack_policy.AuditUserShadowStack != 0U ||
        shadow_stack_policy.EnableUserShadowStackStrictMode != 0U ||
        shadow_stack_policy.SetContextIpValidation != 0U ||
        shadow_stack_policy.AuditSetContextIpValidation != 0U ||
        shadow_stack_policy.SetContextIpValidationRelaxedMode != 0U) {
        puts("context-probe: unqualified CET/context-IP validation");
        return 77;
    }
    if ((features & ~PROBE_SUPPORTED_XSTATE) != 0U) {
        printf("context-probe: unclassified enabled XSTATE: 0x%llx\n",
               (unsigned long long)(features & ~PROBE_SUPPORTED_XSTATE));
        return 77;
    }
    landing_unwind = RtlLookupFunctionEntry((DWORD64)(uintptr_t)&win_landing_c,
                                              &landing_image_base, NULL);
    if (RtlLookupFunctionEntry((DWORD64)(uintptr_t)&win_landing_entry,
                               &image_base, NULL) == NULL ||
        landing_unwind == NULL ||
        RtlLookupFunctionEntry((DWORD64)(uintptr_t)&probe_stack_pages,
                               &image_base, NULL) == NULL ||
        RtlLookupFunctionEntry((DWORD64)(uintptr_t)&win_probe_local_exception,
                               &image_base, NULL) == NULL) {
        puts("context-probe: landing call-chain unwind metadata unavailable");
        return 77;
    }
    if ((features & PROBE_AVX_XSTATE) == 0U) {
        puts("context-probe: AVX XSTATE is unavailable");
        return 77;
    }
    if (probe_context_init(&captured, flags, features) != 0 ||
        probe_context_init(&landing, flags, features) != 0) {
        puts("context-probe: unsupported or unavailable CONTEXT_XSTATE storage");
        return 77;
    }
    alternate_stack = VirtualAlloc(NULL, PROBE_ALT_STACK_BYTES,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (alternate_stack == NULL) {
        fprintf(stderr, "context-probe: alternate stack allocation failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    stack_canary = (uint64_t *)((unsigned char *)alternate_stack + 4096U);
    *stack_canary = PROBE_STACK_CANARY;
    if (!VirtualProtect(alternate_stack, 4096U, PAGE_NOACCESS,
                        &previous_page_protection)) {
        fprintf(stderr, "context-probe: stack boundary protection failed\n");
        return 1;
    }
    private_g_stack = VirtualAlloc(NULL, PROBE_ALT_STACK_BYTES,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (private_g_stack == NULL ||
        !VirtualProtect(private_g_stack, 4096U, PAGE_NOACCESS,
                        &previous_page_protection)) {
        fprintf(stderr, "context-probe: private G stack setup failed\n");
        return 1;
    }
    state.private_stack_top = (void *)
        (((uintptr_t)private_g_stack + PROBE_ALT_STACK_BYTES) & ~(uintptr_t)15U);
    g_probe_state = &state;
    thread = CreateThread(NULL, 0U, probe_worker, &state, 0U, NULL);
    if (thread == NULL || probe_wait_flag(&state.entered, PROBE_WAIT_MS) != 0) {
        fprintf(stderr, "context-probe: worker did not start\n");
        return 1;
    }
    if (state.worker_pre_exception_value != 42) {
        fprintf(stderr, "context-probe: original worker stack exception control failed\n");
        return 1;
    }
    if (state.native_landing_base == 0U ||
        state.native_landing_top <= state.native_landing_base ||
        state.native_landing_inside_system_stack == 0U) {
        fprintf(stderr, "context-probe: native landing reservation invalid\n");
        return 1;
    }
    top = use_native_landing
              ? state.native_landing_top
              : (((uintptr_t)alternate_stack + PROBE_ALT_STACK_BYTES) &
                 ~(uintptr_t)15U);
    landing_low = use_native_landing
                      ? state.native_landing_base
                      : (uintptr_t)alternate_stack;
    landing_high = use_native_landing
                       ? state.native_landing_top
                       : (uintptr_t)alternate_stack + PROBE_ALT_STACK_BYTES;
    entry_rsp = top - 40U; /* Return address plus Windows x64 shadow space. */
    *(uintptr_t *)entry_rsp = (uintptr_t)&probe_landing_returned;
    puts("context-probe: original worker stack exception control passed");
    Sleep(5);

    status = probe_suspend_in_range(thread, &captured, flags, features,
                                    (uintptr_t)win_probe_leaf_begin,
                                    (uintptr_t)win_probe_leaf_end, 1U);
    if (status != -3) {
        if (status == 0) {
            (void)probe_resume_owned(thread);
        }
        fprintf(stderr, "context-probe: unsafe-PC refusal failed: %d\n", status);
        return 1;
    }
    InterlockedExchange(&state.start_leaf, 1);
    Sleep(5);
    {
        DWORD previous = SuspendThread(thread);
        DWORD withdrawn;

        if (previous != 0U) {
            fprintf(stderr, "context-probe: preexisting suspend count: %lu\n",
                    (unsigned long)previous);
            return 1;
        }
        status = probe_suspend_in_range(thread, &captured, flags, features,
                                        (uintptr_t)win_probe_leaf_begin,
                                        (uintptr_t)win_probe_leaf_end, 1U);
        withdrawn = ResumeThread(thread);
        if (status != -2 || withdrawn != 1U) {
            fprintf(stderr,
                    "context-probe: prior-suspend refusal failed: %d/%lu\n",
                    status, (unsigned long)withdrawn);
            return 1;
        }
    }
    Sleep(1);

    status = probe_suspend_in_range(thread, &captured, flags, features,
                                    (uintptr_t)win_probe_leaf_begin,
                                    (uintptr_t)win_probe_leaf_end, 100U);
    if (status != 0) {
        fprintf(stderr, "context-probe: safe leaf suspension failed: %d\n", status);
        return 1;
    }
    if (captured.context->Rax != PROBE_RAX ||
        (uintptr_t)captured.context->Rsp <
            (uintptr_t)private_g_stack + 4096U ||
        (uintptr_t)captured.context->Rsp >=
            (uintptr_t)private_g_stack + PROBE_ALT_STACK_BYTES ||
        captured.context->MxCsr != PROBE_MXCSR ||
        !GetXStateFeaturesMask(captured.context, &valid_features) ||
        (valid_features & PROBE_AVX_XSTATE) == 0U) {
        fprintf(stderr, "context-probe: captured state mismatch\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    printf("context-probe: flags=0x%lx enabled_xstate=0x%llx valid_xstate=0x%llx\n",
           (unsigned long)captured.context->ContextFlags,
           (unsigned long long)features, (unsigned long long)valid_features);

    if (!CopyContext(landing.context, captured.context->ContextFlags,
                     captured.context)) {
        fprintf(stderr, "context-probe: CopyContext failed: %lu\n",
                (unsigned long)GetLastError());
        (void)probe_resume_owned(thread);
        return 1;
    }
    if ((valid_features & XSTATE_MASK_CET_U) != 0U) {
        original_cet = LocateXStateFeature(captured.context, XSTATE_CET_U,
                                            &cet_bytes);
        copied_cet = LocateXStateFeature(landing.context, XSTATE_CET_U,
                                          &copied_cet_bytes);
        if (original_cet == NULL || copied_cet == NULL || cet_bytes == 0U ||
            cet_bytes != copied_cet_bytes ||
            memcmp(original_cet, copied_cet, cet_bytes) != 0) {
            fprintf(stderr, "context-probe: CET_U copy was not exact\n");
            (void)probe_resume_owned(thread);
            return 1;
        }
        printf("context-probe: CET_U copy matched, bytes=%lu\n",
               (unsigned long)cet_bytes);
    }
    landing.context->Rip = (DWORD64)(uintptr_t)&win_landing_entry;
    landing.context->Rsp = (DWORD64)entry_rsp;
    landing.context->EFlags &= ~0x400U; /* Windows ABI calls require DF clear. */
    if (!SetThreadContext(thread, landing.context)) {
        DWORD error_code = GetLastError();

        (void)probe_resume_owned(thread);
        fprintf(stderr, "context-probe: landing redirect failed: %lu\n",
                (unsigned long)error_code);
        return 1;
    }
    if (probe_resume_owned(thread) != 0) {
        fprintf(stderr, "context-probe: landing resume failed\n");
        return 1;
    }
    if (probe_wait_flag(&state.landed, PROBE_WAIT_MS) != 0) {
        fprintf(stderr, "context-probe: landing was not reached\n");
        return 1;
    }
    status = probe_suspend_in_range(
        thread, &landing, flags, features,
        (uintptr_t)(landing_image_base + landing_unwind->BeginAddress),
        (uintptr_t)(landing_image_base + landing_unwind->EndAddress), 100U);
    if (status != 0) {
        fprintf(stderr, "context-probe: landing-body suspension failed: %d\n", status);
        return 1;
    }
    memcpy(&unwind_context, landing.context, sizeof(unwind_context));
    (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                           landing_image_base,
                           unwind_context.Rip,
                           landing_unwind,
                           &unwind_context,
                           &unwind_handler_data,
                           &unwind_frame,
                           NULL);
    if (unwind_context.Rip < (DWORD64)(uintptr_t)&win_landing_entry ||
        unwind_context.Rip >= (DWORD64)(uintptr_t)win_landing_entry_end ||
        (uintptr_t)unwind_context.Rsp < landing_low ||
        (uintptr_t)unwind_context.Rsp >= landing_high) {
        fprintf(stderr, "context-probe: landing-body unwind escaped its stack\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    entry_unwind = RtlLookupFunctionEntry(unwind_context.Rip, &image_base, NULL);
    if (entry_unwind == NULL) {
        fprintf(stderr, "context-probe: landing-entry unwind info missing\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    unwind_handler_data = NULL;
    unwind_frame = 0U;
    (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                           image_base,
                           unwind_context.Rip,
                           entry_unwind,
                           &unwind_context,
                           &unwind_handler_data,
                           &unwind_frame,
                           NULL);
    if (unwind_context.Rip != (DWORD64)(uintptr_t)&probe_landing_returned) {
        fprintf(stderr, "context-probe: synthetic unwind boundary mismatch\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    puts("context-probe: landing unwind reached synthetic boundary");
    if (probe_resume_owned(thread) != 0) {
        fprintf(stderr, "context-probe: landing-body resume failed\n");
        return 1;
    }
    InterlockedExchange(&state.restore_allowed, 1);
    if (probe_wait_flag(&state.gate_ready, PROBE_WAIT_MS) != 0) {
        fprintf(stderr, "context-probe: restore gate was not reached\n");
        return 1;
    }
    status = probe_suspend_in_range(thread, &landing, flags, features,
                                    (uintptr_t)win_restore_gate_begin,
                                    (uintptr_t)win_restore_gate_end, 100U);
    if (status != 0) {
        fprintf(stderr, "context-probe: restore-gate suspension failed: %d\n", status);
        return 1;
    }
    gate_unwind = RtlLookupFunctionEntry(landing.context->Rip,
                                          &gate_image_base, NULL);
    if (gate_unwind == NULL) {
        fprintf(stderr, "context-probe: restore-gate unwind info missing\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    memcpy(&unwind_context, landing.context, sizeof(unwind_context));
    unwind_handler_data = NULL;
    unwind_frame = 0U;
    (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                           gate_image_base,
                           unwind_context.Rip,
                           gate_unwind,
                           &unwind_context,
                           &unwind_handler_data,
                           &unwind_frame,
                           NULL);
    if (unwind_context.Rip < landing_image_base + landing_unwind->BeginAddress ||
        unwind_context.Rip >= landing_image_base + landing_unwind->EndAddress ||
        (uintptr_t)unwind_context.Rsp < landing_low ||
        (uintptr_t)unwind_context.Rsp >= landing_high) {
        fprintf(stderr, "context-probe: restore-gate unwind escaped its stack\n");
        (void)probe_resume_owned(thread);
        return 1;
    }
    puts("context-probe: restore-gate unwind reached landing body");
    if (corrupt_restore) {
        captured.context->Rbx ^= 1U;
    }
    if (!SetThreadContext(thread, captured.context)) {
        DWORD error_code = GetLastError();

        (void)probe_resume_owned(thread);
        fprintf(stderr, "context-probe: full-context restoration failed: %lu\n",
                (unsigned long)error_code);
        return 1;
    }
    if (probe_resume_owned(thread) != 0) {
        fprintf(stderr, "context-probe: restore resume failed\n");
        return 1;
    }
    status = probe_suspend_in_range(thread, &landing, flags, features,
                                    (uintptr_t)win_probe_leaf_begin,
                                    (uintptr_t)win_probe_leaf_end, 100U);
    if (status != 0 ||
        !GetXStateFeaturesMask(landing.context, &post_restore_features) ||
        landing.context->Rax != PROBE_RAX ||
        landing.context->MxCsr != PROBE_MXCSR) {
        fprintf(stderr, "context-probe: post-restore context mismatch: %d\n", status);
        if (status == 0) {
            (void)probe_resume_owned(thread);
        }
        return 1;
    }
    if ((valid_features & XSTATE_MASK_CET_U) != 0U) {
        DWORD restored_cet_bytes = 0U;
        void *restored_cet = LocateXStateFeature(landing.context, XSTATE_CET_U,
                                                  &restored_cet_bytes);

        if ((post_restore_features & XSTATE_MASK_CET_U) == 0U ||
            restored_cet == NULL || restored_cet_bytes != cet_bytes ||
            memcmp(original_cet, restored_cet, cet_bytes) != 0) {
            fprintf(stderr, "context-probe: CET_U restore was not exact\n");
            (void)probe_resume_owned(thread);
            return 1;
        }
        puts("context-probe: CET_U post-restore state matched");
    }
    if (probe_resume_owned(thread) != 0) {
        fprintf(stderr, "context-probe: post-restore check resume failed\n");
        return 1;
    }
    InterlockedExchange(&state.release, 1);
    if (WaitForSingleObject(thread, PROBE_WAIT_MS) != WAIT_OBJECT_0) {
        fprintf(stderr, "context-probe: resumed leaf did not return\n");
        return 1;
    }
    if (probe_check_extended_registers(&state.observed) != 0) {
        return 1;
    }
    if (state.observed.rax != PROBE_RAX ||
        state.observed.ymm0[0] != PROBE_RAX ||
        state.observed.ymm0[1] != PROBE_RAX ||
        state.observed.ymm0[2] != PROBE_YMM_HIGH ||
        state.observed.ymm0[3] != PROBE_YMM_HIGH ||
        state.observed.observed_mxcsr != PROBE_MXCSR ||
        state.observed.observed_x87cw != PROBE_X87CW ||
        state.landing_last_error != PROBE_LAST_ERROR ||
        state.landing_errno != EDOM ||
        state.landing_stack_checksum != PROBE_STACK_CHECKSUM ||
        state.landing_exception_value != 42 ||
        *stack_canary != PROBE_STACK_CANARY ||
        state.native_canary_ok == 0U ||
        state.final_last_error != PROBE_LAST_ERROR ||
        state.final_errno != EDOM ||
        state.original_exception_value != 42) {
        fprintf(stderr,
                "context-probe: state mismatch rax=%llx ymm0=%llx/%llx/%llx/%llx "
                "mxcsr=%x x87cw=%x landing_error=%lu/%d stack=%u exception=%d "
                "final_error=%lu/%d original_exception=%d\n",
                (unsigned long long)state.observed.rax,
                (unsigned long long)state.observed.ymm0[0],
                (unsigned long long)state.observed.ymm0[1],
                (unsigned long long)state.observed.ymm0[2],
                (unsigned long long)state.observed.ymm0[3],
                state.observed.observed_mxcsr,
                state.observed.observed_x87cw,
                (unsigned long)state.landing_last_error, state.landing_errno,
                state.landing_stack_checksum,
                state.landing_exception_value,
                (unsigned long)state.final_last_error, state.final_errno,
                state.original_exception_value);
        return 1;
    }
    if (use_native_landing && state.landing_outside_system_stack != 0U) {
        fprintf(stderr, "context-probe: native landing left system stack bounds\n");
        return 1;
    }
    printf("context-probe: landing_outside_system_stack_limits=%u\n",
           state.landing_outside_system_stack);
    puts("context-probe: remote landing and RAX/YMM0/MXCSR/error-state restore passed");
    CloseHandle(thread);
    VirtualFree(alternate_stack, 0U, MEM_RELEASE);
    VirtualFree(private_g_stack, 0U, MEM_RELEASE);
    free(captured.buffer);
    free(landing.buffer);
    return 0;
}
