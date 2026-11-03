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

#include "async_context_linux_arm64.h"

#if LLAM_PLATFORM_LINUX && LLAM_ARCH_AARCH64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <asm/ptrace.h>
#include <asm/sigcontext.h>
#include <errno.h>
#include <string.h>

enum {
    LLAM_LINUX_ARM64_FPSIMD_MAGIC = 0x46508001U,
    LLAM_LINUX_ARM64_ESR_MAGIC = 0x45535201U,
    LLAM_LINUX_ARM64_SVE_MAGIC = 0x53564501U,
    LLAM_LINUX_ARM64_FPSIMD_BYTES = 528U,
    LLAM_LINUX_ARM64_ESR_BYTES = 16U,
    LLAM_LINUX_ARM64_RECORD_ALIGN = 16U
};

#define LLAM_LINUX_ARM64_NZCV_MASK UINT64_C(0xf0000000)
#define LLAM_LINUX_ARM64_PSTATE_ALLOWED (LLAM_LINUX_ARM64_NZCV_MASK | PSR_SSBS_BIT)

_Static_assert(PSR_SSBS_BIT == 0x1000U, "Linux AArch64 native SSBS bit");

_Static_assert(NSIG == 65, "Linux AArch64 kernel signal set has 64 bits");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "initial Linux AArch64 signal transport requires little endian");
_Static_assert(sizeof(sigset_t) >= sizeof(uint64_t), "kernel mask fits userspace storage");
_Static_assert(offsetof(ucontext_t, uc_sigmask) == 40U &&
               offsetof(ucontext_t, uc_mcontext) == 176U,
               "Linux AArch64 mask reservation plus native alignment padding");
_Static_assert(sizeof(struct fpsimd_context) == 528U &&
               offsetof(struct fpsimd_context, fpsr) == 8U &&
               offsetof(struct fpsimd_context, fpcr) == 12U &&
               offsetof(struct fpsimd_context, vregs) == 16U, "Linux AArch64 FPSIMD ABI");
_Static_assert(sizeof(((ucontext_t *)0)->uc_mcontext.regs) == 31U * sizeof(uint64_t),
               "Linux AArch64 general-register payload");
_Static_assert(sizeof(((ucontext_t *)0)->uc_mcontext.__reserved) == 4096U,
               "Linux AArch64 inline signal records");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "assembly gate publication is lock-free");

static bool llam_linux_arm64_async_address(uint64_t address, uint64_t alignment) {
    /* Initial untagged address profile. No PAC/tagged PC or SP inference. */
    return address != 0U && (address >> 56U) == 0U && (address & (alignment - 1U)) == 0U;
}

static bool llam_linux_arm64_async_general(uint64_t pc, uint64_t sp, uint64_t pstate) {
    return llam_linux_arm64_async_address(pc, 4U) &&
           llam_linux_arm64_async_address(sp, 16U) &&
           (pstate & ~LLAM_LINUX_ARM64_PSTATE_ALLOWED) == 0U;
}

static int llam_linux_arm64_async_profile(const ucontext_t *frame, size_t *fpsimd_offset,
                                         uint16_t *sve_vl) {
    const unsigned char *records;
    size_t offset = 0U;
    size_t fpsimd = SIZE_MAX;
    bool esr_seen = false;
    uint16_t vector_length = 0U;

    if (frame == NULL) return EINVAL;
    if (!llam_linux_arm64_async_general(frame->uc_mcontext.pc, frame->uc_mcontext.sp,
                                       frame->uc_mcontext.pstate)) return ENOTSUP;
    records = frame->uc_mcontext.__reserved;
    while (offset <= sizeof(frame->uc_mcontext.__reserved) - LLAM_LINUX_ARM64_RECORD_ALIGN) {
        uint32_t header[2];
        memcpy(header, records + offset, sizeof(header));
        if (header[0] == 0U && header[1] == 0U) {
            if (fpsimd == SIZE_MAX) return ENOTSUP;
            *fpsimd_offset = fpsimd;
            *sve_vl = vector_length;
            return 0;
        }
        if (header[1] < LLAM_LINUX_ARM64_RECORD_ALIGN ||
            (header[1] & (LLAM_LINUX_ARM64_RECORD_ALIGN - 1U)) != 0U ||
            header[1] > sizeof(frame->uc_mcontext.__reserved) - offset) return ENOTSUP;
        if (header[0] == LLAM_LINUX_ARM64_FPSIMD_MAGIC) {
            if (fpsimd != SIZE_MAX || header[1] != LLAM_LINUX_ARM64_FPSIMD_BYTES) return ENOTSUP;
            fpsimd = offset;
        } else if (header[0] == LLAM_LINUX_ARM64_ESR_MAGIC) {
            if (esr_seen || header[1] != LLAM_LINUX_ARM64_ESR_BYTES) return ENOTSUP;
            esr_seen = true; /* Informational; never copied to the fresh frame. */
        } else if (header[0] == LLAM_LINUX_ARM64_SVE_MAGIC) {
            uint16_t properties[4];
            /* Linux emits this header even when SVE registers are not live.
             * No scalable payload exists in the exact 16-byte, non-streaming
             * form. Keep its per-thread VL; never shrink a live SVE record. */
            if (vector_length != 0U || header[1] != 16U) return ENOTSUP;
            memcpy(properties, records + offset + 8U, sizeof(properties));
            if (!sve_vl_valid(properties[0]) || properties[1] != 0U ||
                properties[2] != 0U || properties[3] != 0U) return ENOTSUP;
            vector_length = properties[0];
        } else {
            /* Includes EXTRA pointers, SME, TPIDR2,
             * FPMR and access restrictions. Never follow extension pointers. */
            return ENOTSUP;
        }
        offset += header[1];
    }
    return ENOTSUP;
}

int llam_linux_arm64_async_read(const ucontext_t *frame, int entry_errno,
                              llam_linux_arm64_async_machine_t *out) {
    size_t fpsimd;
    uint16_t sve_vl;
    int error;
    if (out == NULL) return EINVAL;
    error = llam_linux_arm64_async_profile(frame, &fpsimd, &sve_vl);
    if (error != 0) return error;

    memcpy(out->x, frame->uc_mcontext.regs, sizeof(out->x));
    out->sp = frame->uc_mcontext.sp;
    out->pc = frame->uc_mcontext.pc;
    out->pstate = frame->uc_mcontext.pstate;
    /* Only the kernel's 64 signal bits, not glibc's reserved sigset padding. */
    memcpy(&out->signal_mask, &frame->uc_sigmask, sizeof(out->signal_mask));
    memcpy(&out->fpsr, frame->uc_mcontext.__reserved + fpsimd + 8U, sizeof(out->fpsr));
    memcpy(&out->fpcr, frame->uc_mcontext.__reserved + fpsimd + 12U, sizeof(out->fpcr));
    memcpy(out->q, frame->uc_mcontext.__reserved + fpsimd + 16U, sizeof(out->q));
    out->entry_errno = entry_errno;
    out->sve_vl = sve_vl;
    out->valid = true; /* Caller release-publishes the containing bank afterwards. */
    return 0;
}

int llam_linux_arm64_async_redirect(ucontext_t *frame, const llam_ctx_t *scheduler,
                                  const sigset_t *scheduler_mask) {
    size_t fpsimd;
    uint16_t sve_vl;
    int error;
    if (scheduler == NULL || scheduler_mask == NULL) return EINVAL;
    error = llam_linux_arm64_async_profile(frame, &fpsimd, &sve_vl);
    if (error != 0) return error;
    if (!llam_linux_arm64_async_address(scheduler->sp, 16U) ||
        !llam_linux_arm64_async_address(scheduler->lr, 4U) || scheduler->simd_flags != 0U) return ENOTSUP;
    (void)fpsimd;
    (void)sve_vl;

    /* Native OS return goes to this bridge on M's already-saved stack. The
     * bridge restores M's ABI state and returns to the actual switch caller.
     * G's interrupted stack is not written and G.ctx is not substituted. */
    frame->uc_mcontext.regs[0] = (uint64_t)(uintptr_t)scheduler;
    frame->uc_mcontext.sp = scheduler->sp;
    frame->uc_mcontext.pc = (uint64_t)(uintptr_t)llam_linux_arm64_async_restore_scheduler;
    /* M inherits the interrupted native thread's SSBS, not a cached policy.
     * Complete validation above still refuses every other non-NZCV bit. */
    frame->uc_mcontext.pstate &= ~LLAM_LINUX_ARM64_NZCV_MASK;
    memcpy(&frame->uc_sigmask, scheduler_mask, sizeof(uint64_t));
    return 0;
}

int llam_linux_arm64_async_restore(ucontext_t *fresh_frame,
                                 const llam_linux_arm64_async_machine_t *machine) {
    size_t fpsimd;
    uint16_t sve_vl;
    int error;
    if (machine == NULL || !machine->valid) return EINVAL;
    if (!llam_linux_arm64_async_general(machine->pc, machine->sp, machine->pstate)) return ENOTSUP;
    error = llam_linux_arm64_async_profile(fresh_frame, &fpsimd, &sve_vl);
    if (error != 0) return error;
    if (sve_vl != machine->sve_vl) return ENOTSUP;

    /* Complete validation precedes the first write. Keep every OS-owned field
     * and the NEW record layout; replace only native payload and kernel mask. */
    memcpy(fresh_frame->uc_mcontext.regs, machine->x, sizeof(machine->x));
    fresh_frame->uc_mcontext.sp = machine->sp;
    fresh_frame->uc_mcontext.pc = machine->pc;
    /* Exact admitted G state, including SSBS; fresh gate state is not G state.
     * Native speculation policy must remain unchanged while Gs are saved. */
    fresh_frame->uc_mcontext.pstate = machine->pstate;
    memcpy(&fresh_frame->uc_sigmask, &machine->signal_mask, sizeof(machine->signal_mask));
    memcpy(fresh_frame->uc_mcontext.__reserved + fpsimd + 8U, &machine->fpsr, sizeof(machine->fpsr));
    memcpy(fresh_frame->uc_mcontext.__reserved + fpsimd + 12U, &machine->fpcr, sizeof(machine->fpcr));
    memcpy(fresh_frame->uc_mcontext.__reserved + fpsimd + 16U, machine->q, sizeof(machine->q));
    return 0;
}
#endif
