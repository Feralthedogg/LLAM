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

#include "runtime_internal.h"
#include "async_context_darwin_x86_64.h"

#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <cpuid.h>
#include <sys/sysctl.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "native gate publication must be lock-free");
_Static_assert(offsetof(x86_avx_state64_t, __fpu_ymmh0) >= sizeof(x86_float_state64_t),
               "Mach AVX state must contain the complete legacy FP state");

static pthread_once_t g_darwin_x86_profile_once = PTHREAD_ONCE_INIT;
static bool g_darwin_x86_profile_known;
static bool g_darwin_x86_has_avx;

static bool feature(const char *name, int *value) {
    size_t bytes = sizeof(*value);
    return sysctlbyname(name, value, &bytes, NULL, 0U) == 0 && bytes == sizeof(*value) &&
        (*value == 0 || *value == 1);
}

static void prepare_profile(void) {
    int avx = -1, avx512 = -1;
    int translated = 0;
    size_t translated_bytes = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &translated_bytes, NULL, 0U) != 0) {
        if (errno != ENOENT) return;
    } else if (translated_bytes != sizeof(translated) || translated != 0) {
        return;
    }
    /* XNU promotes AVX512 state lazily. A failed AVX512 get-state does not
     * prove its absence, so this profile refuses that hardware rather than
     * truncating a future promoted thread to the AVX flavor. Query before any
     * owned suspend; no sysctl/allocator/loader operation enters a transaction. */
    g_darwin_x86_profile_known = feature("hw.optional.avx1_0", &avx) &&
        feature("hw.optional.avx512f", &avx512) && avx512 == 0;
    g_darwin_x86_has_avx = avx == 1;
    /* Resolve every Mach client stub before a target can own our suspension.
     * A null destination cannot consume or modify any thread's native state. */
    natural_t word = 0U;
    mach_msg_type_number_t count = 1U;
    (void)thread_suspend(MACH_PORT_NULL);
    (void)thread_resume(MACH_PORT_NULL);
    (void)thread_get_state(MACH_PORT_NULL, x86_THREAD_STATE64, &word, &count);
    (void)thread_set_state(MACH_PORT_NULL, x86_THREAD_STATE64, &word, 0U);
    count = 1U;
    (void)thread_info(MACH_PORT_NULL, THREAD_BASIC_INFO, (thread_info_t)&word, &count);
}

static bool current_cpu_profile(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1U, &a, &b, &c, &d) ||
        (d & ((1U << 24U) | (1U << 26U))) != ((1U << 24U) | (1U << 26U))) return false;
    if ((c & (1U << 27U)) != 0U) {
        uint32_t low, high;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
        if (high != 0U || low != (g_darwin_x86_has_avx ? 7U : 3U)) return false;
    } else if (g_darwin_x86_has_avx) {
        return false;
    }
    if (__get_cpuid_max(0U, NULL) >= 7U) {
        __cpuid_count(7U, 0U, a, b, c, d);
        /* No public Mach state flavor here captures CET. OSPKE indicates
         * enabled PKRU state, which must not be lost through FP-only flavors. */
        if ((c & ((1U << 4U) | (1U << 7U))) != 0U || (d & (1U << 20U)) != 0U) return false;
    }
    return true;
}

static int refused(int error) {
    errno = error;
    return -1;
}

static LLAM_NORETURN void uncertain(void) {
    _Exit(125);
}

int llam_darwin_x86_async_bind(llam_darwin_x86_async_binding_t *binding) {
    if (binding == NULL || binding->thread != MACH_PORT_NULL) return refused(EINVAL);
    if (pthread_once(&g_darwin_x86_profile_once, prepare_profile) != 0 ||
        !g_darwin_x86_profile_known || !current_cpu_profile() || g_llam_fp_control_context == 0U) {
        return refused(ENOTSUP);
    }
    if (llam_async_shared_block_native_signals(g_llam_tls_worker, &binding->previous_mask) != 0) return -1;
    binding->mask_saved = true;
    binding->restore_mask = g_llam_tls_worker == NULL ||
        g_llam_tls_worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE;
    binding->thread = mach_thread_self();
    if (binding->thread == MACH_PORT_NULL) {
        (void)pthread_sigmask(SIG_SETMASK, &binding->previous_mask, NULL);
        binding->mask_saved = false;
        return refused(EIO);
    }
    binding->floating_flavor = g_darwin_x86_has_avx ? x86_AVX_STATE64 : x86_FLOAT_STATE64;
    binding->floating_count = g_darwin_x86_has_avx ? x86_AVX_STATE64_COUNT : x86_FLOAT_STATE64_COUNT;
    __asm__ volatile("movw %%cs, %0" : "=r"(binding->code_selector));
    __asm__ volatile("movw %%fs, %0" : "=r"(binding->fs_selector));
    __asm__ volatile("movw %%gs, %0" : "=r"(binding->gs_selector));
    binding->suspended = false;
    binding->mutated = false;
    return 0;
}

void llam_darwin_x86_async_close(llam_darwin_x86_async_binding_t *binding) {
    if (binding == NULL || binding->suspended || binding->thread == MACH_PORT_NULL ||
        mach_port_deallocate(mach_task_self(), binding->thread) != KERN_SUCCESS) uncertain();
    if (binding->mask_saved && binding->restore_mask &&
        pthread_sigmask(SIG_SETMASK, &binding->previous_mask, NULL) != 0) uncertain();
    memset(binding, 0, sizeof(*binding));
}

int llam_darwin_x86_async_suspend(llam_darwin_x86_async_binding_t *binding) {
    if (binding == NULL || binding->suspended || binding->thread == MACH_PORT_NULL ||
        binding->thread == pthread_mach_thread_np(pthread_self())) return refused(EINVAL);
    if (thread_suspend(binding->thread) != KERN_SUCCESS) return refused(ESRCH);
    binding->suspended = true;
    binding->mutated = false;
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t result = thread_info(binding->thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    if (result != KERN_SUCCESS || count != THREAD_BASIC_INFO_COUNT || info.suspend_count != 1) {
        llam_darwin_x86_async_resume(binding);
        return refused(result == KERN_SUCCESS ? EBUSY : EIO);
    }
    return 0;
}

static bool native_selectors(const llam_darwin_x86_async_binding_t *binding,
                             const x86_thread_state64_t *state) {
    return state->__cs == binding->code_selector && state->__fs == binding->fs_selector &&
        state->__gs == binding->gs_selector && (state->__cs & 3U) == 3U;
}

static bool native_profile(const llam_darwin_x86_async_binding_t *binding) {
    x86_thread_full_state64_t full;
    mach_msg_type_number_t count = x86_THREAD_FULL_STATE64_COUNT;
    /* This flavor succeeds only for custom-LDT processes. Ordinary Mach
     * THREAD_STATE64 cannot round-trip that ABI, so it is never accepted. */
    if (thread_get_state(binding->thread, x86_THREAD_FULL_STATE64, (thread_state_t)&full, &count) !=
        KERN_INVALID_ARGUMENT) return false;
    x86_debug_state64_t debug = {0};
    count = x86_DEBUG_STATE64_COUNT;
    return thread_get_state(binding->thread, x86_DEBUG_STATE64, (thread_state_t)&debug, &count) == KERN_SUCCESS &&
        count == x86_DEBUG_STATE64_COUNT && (debug.__dr7 & ~UINT64_C(0x400)) == 0U;
}

int llam_darwin_x86_async_read(llam_darwin_x86_async_binding_t *binding,
                                llam_darwin_x86_async_machine_t *out) {
    if (binding == NULL || !binding->suspended || binding->mutated || out == NULL) return refused(EINVAL);
    memset(out, 0, sizeof(*out));
    mach_msg_type_number_t general_count = x86_THREAD_STATE64_COUNT;
    mach_msg_type_number_t floating_count = binding->floating_count;
    if (!native_profile(binding) ||
        thread_get_state(binding->thread, x86_THREAD_STATE64, (thread_state_t)&out->general, &general_count) != KERN_SUCCESS ||
        general_count != x86_THREAD_STATE64_COUNT ||
        thread_get_state(binding->thread, binding->floating_flavor, (thread_state_t)&out->floating, &floating_count) != KERN_SUCCESS ||
        floating_count != binding->floating_count || !native_selectors(binding, &out->general) ||
        (out->general.__rflags & ~UINT64_C(0xad7)) != 0U || (out->general.__rflags & UINT64_C(0x202)) != UINT64_C(0x202) ||
        out->general.__rip == 0U || out->general.__rip > UINT64_C(0x00007fffffffffff) ||
        out->general.__rsp == 0U || out->general.__rsp > UINT64_C(0x00007fffffffffff)) return refused(ENOTSUP);
    out->origin_thread = binding->thread;
    out->floating_flavor = binding->floating_flavor;
    out->valid = true;
    return 0;
}

static bool same_origin(const llam_darwin_x86_async_binding_t *binding,
                        const llam_darwin_x86_async_machine_t *captured) {
    return binding != NULL && binding->suspended && !binding->mutated && captured != NULL && captured->valid &&
        captured->origin_thread == binding->thread && captured->floating_flavor == binding->floating_flavor &&
        native_selectors(binding, &captured->general);
}

int llam_darwin_x86_async_redirect(llam_darwin_x86_async_binding_t *binding,
                                    const llam_darwin_x86_async_machine_t *captured,
                                    const llam_ctx_t *scheduler, uintptr_t begin, uintptr_t end,
                                    uintptr_t stack_low, uintptr_t stack_high) {
    if (!same_origin(binding, captured) || scheduler == NULL || begin == 0U || begin >= end ||
        captured->general.__rip < begin || captured->general.__rip >= end || stack_low >= stack_high ||
        captured->general.__rsp < stack_low || captured->general.__rsp >= stack_high ||
        captured->general.__rsp - stack_low < 128U || (captured->general.__rsp & 7U) != 0U ||
        scheduler->rsp == 0U || (scheduler->rsp & 7U) != 0U || scheduler->xsave_area != NULL) return refused(EINVAL);
    x86_thread_state64_t redirect = captured->general;
    redirect.__rip = (uint64_t)(uintptr_t)llam_darwin_x86_async_restore_scheduler;
    redirect.__rdi = (uint64_t)(uintptr_t)scheduler;
    /* The assembly landing does not touch G's stack or its 128-byte red zone. */
    if (thread_set_state(binding->thread, x86_THREAD_STATE64, (thread_state_t)&redirect,
                         x86_THREAD_STATE64_COUNT) != KERN_SUCCESS) uncertain();
    binding->mutated = true;
    return 0;
}

int llam_darwin_x86_async_restore(llam_darwin_x86_async_binding_t *binding,
                                   const llam_darwin_x86_async_machine_t *captured) {
    if (!same_origin(binding, captured)) return refused(EINVAL);
    if (!native_profile(binding)) return refused(ENOTSUP);
    if (thread_set_state(binding->thread, captured->floating_flavor,
                         (thread_state_t)&captured->floating, binding->floating_count) != KERN_SUCCESS) uncertain();
    binding->mutated = true;
    if (thread_set_state(binding->thread, x86_THREAD_STATE64, (thread_state_t)&captured->general,
                         x86_THREAD_STATE64_COUNT) != KERN_SUCCESS) uncertain();
    return 0;
}

void llam_darwin_x86_async_resume(llam_darwin_x86_async_binding_t *binding) {
    if (binding == NULL || !binding->suspended || thread_resume(binding->thread) != KERN_SUCCESS) uncertain();
    binding->suspended = false;
}

#if LLAM_ASYNC_CONTEXT_SHARED
static int native_bind(void *binding) { return llam_darwin_x86_async_bind(binding); }
static void native_close(void *binding) { llam_darwin_x86_async_close(binding); }
static int native_suspend(void *binding) { return llam_darwin_x86_async_suspend(binding); }
static int native_read(void *binding, void *context) { return llam_darwin_x86_async_read(binding, context); }
static int native_observe_sp(void *opaque, uintptr_t *sp) {
    const llam_darwin_x86_async_binding_t *binding = opaque;
    x86_thread_state64_t general;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (binding == NULL || sp == NULL || !binding->suspended || binding->mutated) return refused(EINVAL);
    if (thread_get_state(binding->thread, x86_THREAD_STATE64, (thread_state_t)&general, &count) != KERN_SUCCESS ||
        count != x86_THREAD_STATE64_COUNT) return refused(ENOTSUP);
    *sp = general.__rsp;
    return 0;
}
static int native_redirect(void *binding, const void *context, const llam_ctx_t *scheduler,
                            uintptr_t begin, uintptr_t end, uintptr_t low, uintptr_t high) {
    return llam_darwin_x86_async_redirect(binding, context, scheduler, begin, end, low, high);
}
static int native_restore(void *binding, const void *context) { return llam_darwin_x86_async_restore(binding, context); }
static void native_resume(void *binding) { llam_darwin_x86_async_resume(binding); }
static uintptr_t native_pc(const void *context) {
    const llam_darwin_x86_async_machine_t *machine = context;
    return machine->valid ? machine->general.__rip : 0U;
}
static uintptr_t native_sp(const void *context) {
    const llam_darwin_x86_async_machine_t *machine = context;
    return machine->valid ? machine->general.__rsp : 0U;
}
static bool native_at_gate(const void *context) {
    uintptr_t pc = native_pc(context);
    return pc >= (uintptr_t)llam_darwin_x86_async_gate_begin && pc < (uintptr_t)llam_darwin_x86_async_gate_end;
}
const llam_async_native_ops_t g_llam_darwin_async_ops = {
    .binding_size = sizeof(llam_darwin_x86_async_binding_t),
    .context_size = sizeof(llam_darwin_x86_async_machine_t),
    .bind_current = native_bind, .close = native_close, .suspend = native_suspend,
    .read = native_read, .redirect = native_redirect, .restore = native_restore,
    .resume = native_resume, .pc = native_pc, .sp = native_sp, .at_gate = native_at_gate,
    .save_gate = llam_darwin_x86_async_save_gate, .observe_sp = native_observe_sp
};
#endif
#endif
