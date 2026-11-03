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

#ifndef LLAM_ASYNC_CONTEXT_SIGNAL_NATIVE_H
#define LLAM_ASYNC_CONTEXT_SIGNAL_NATIVE_H

/* Only scalar/payload operations enter the signal handler. OS-specific frame
 * validation stays in each native transport; no old kernel frame is retained. */
#if LLAM_PLATFORM_LINUX && LLAM_ARCH_AARCH64
#include "async_context_linux_arm64.h"
typedef llam_linux_arm64_async_machine_t llam_async_signal_machine_t;
#define LLAM_ASYNC_SIGNAL_MASK_WORDS 1U
#define LLAM_ASYNC_SIGNAL_SENDER_CODE SI_TKILL
#define llam_signal_save_gate llam_linux_arm64_async_save_gate
#define llam_signal_gate_begin llam_linux_arm64_async_gate_begin
#define llam_signal_gate_end llam_linux_arm64_async_gate_end
static inline uintptr_t llam_signal_frame_pc(const ucontext_t *frame) { return frame->uc_mcontext.pc; }
static inline uintptr_t llam_signal_frame_sp(const ucontext_t *frame) { return frame->uc_mcontext.sp; }
static inline uint64_t llam_signal_frame_flags(const ucontext_t *frame) { return frame->uc_mcontext.pstate; }
static inline uint64_t llam_signal_machine_flags(const llam_async_signal_machine_t *machine) { return machine->pstate; }
static inline int llam_signal_read(const ucontext_t *frame, int error, uintptr_t low, uintptr_t high,
                                    llam_async_signal_machine_t *machine) {
    (void)low; (void)high;
    return llam_linux_arm64_async_read(frame, error, machine);
}
static inline int llam_signal_redirect(ucontext_t *frame, const llam_ctx_t *scheduler,
                                        const sigset_t *mask, uintptr_t low, uintptr_t high) {
    (void)low; (void)high;
    return llam_linux_arm64_async_redirect(frame, scheduler, mask);
}
static inline int llam_signal_restore(ucontext_t *frame, const llam_async_signal_machine_t *machine,
                                       uintptr_t low, uintptr_t high) {
    (void)low; (void)high;
    return llam_linux_arm64_async_restore(frame, machine);
}
#elif defined(__FreeBSD__) && LLAM_ARCH_X86_64
#include "async_context_freebsd_x86_64.h"
typedef llam_freebsd_x86_async_machine_t llam_async_signal_machine_t;
#define LLAM_ASYNC_SIGNAL_MASK_WORDS 2U
#define LLAM_ASYNC_SIGNAL_SENDER_CODE SI_LWP
#define llam_signal_save_gate llam_freebsd_x86_async_save_gate
#define llam_signal_gate_begin llam_freebsd_x86_async_gate_begin
#define llam_signal_gate_end llam_freebsd_x86_async_gate_end
_Static_assert(sizeof(sigset_t) == 16U, "FreeBSD native signal mask must retain all 128 bits");
static inline uintptr_t llam_signal_frame_pc(const ucontext_t *frame) { return frame->uc_mcontext.mc_rip; }
static inline uintptr_t llam_signal_frame_sp(const ucontext_t *frame) { return frame->uc_mcontext.mc_rsp; }
static inline uint64_t llam_signal_frame_flags(const ucontext_t *frame) { return frame->uc_mcontext.mc_rflags; }
static inline uint64_t llam_signal_machine_flags(const llam_async_signal_machine_t *machine) { return machine->flags; }
static inline int llam_signal_read(const ucontext_t *frame, int error, uintptr_t low, uintptr_t high,
                                    llam_async_signal_machine_t *machine) {
    return llam_freebsd_x86_async_read(frame, error, low, high, machine);
}
static inline int llam_signal_redirect(ucontext_t *frame, const llam_ctx_t *scheduler,
                                        const sigset_t *mask, uintptr_t low, uintptr_t high) {
    return llam_freebsd_x86_async_redirect(frame, scheduler, mask, low, high);
}
static inline int llam_signal_restore(ucontext_t *frame, const llam_async_signal_machine_t *machine,
                                       uintptr_t low, uintptr_t high) {
    return llam_freebsd_x86_async_restore(frame, machine, low, high);
}
#else
#error "No qualified signal-native transport for this target"
#endif

static inline bool llam_signal_mask_blocks(const sigset_t *mask, int number) {
    uint64_t words[LLAM_ASYNC_SIGNAL_MASK_WORDS];
    memcpy(words, mask, sizeof(words));
    unsigned bit = (unsigned)number - 1U;
    return number <= 0 || bit >= LLAM_ASYNC_SIGNAL_MASK_WORDS * 64U ||
        (words[bit / 64U] & (UINT64_C(1) << (bit % 64U))) != 0U;
}

static inline void llam_signal_machine_mask(const llam_async_signal_machine_t *machine, sigset_t *mask) {
    memset(mask, 0, sizeof(*mask));
    memcpy(mask, &machine->signal_mask, LLAM_ASYNC_SIGNAL_MASK_WORDS * sizeof(uint64_t));
}

#endif /* LLAM_ASYNC_CONTEXT_SIGNAL_NATIVE_H */
