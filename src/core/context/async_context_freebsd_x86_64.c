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

#include "async_context_freebsd_x86_64.h"

#if defined(__FreeBSD__) && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <errno.h>
#include <sys/param.h>

_Static_assert(sizeof(sigset_t) == 16U, "FreeBSD signal masks have 128 bits");
_Static_assert(sizeof(mcontext_t) == 800U && sizeof(ucontext_t) == 880U,
               "qualified FreeBSD amd64 signal ABI");
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 16U && offsetof(mcontext_t, mc_fpstate) == 224U,
               "FreeBSD amd64 native frame offsets");
_Static_assert(offsetof(mcontext_t, mc_r15) - offsetof(mcontext_t, mc_rdi) == 14U * sizeof(uint64_t),
               "FreeBSD contiguous general-register payload");
_Static_assert(sizeof(((mcontext_t *)0)->mc_fpstate) == 512U, "native FXSAVE payload");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "gate publication must be lock-free");

static void copy_bytes(void *destination, const void *source, size_t bytes) {
    /* Volatile destination prevents loop-idiom libc calls/IFUNC resolution in
     * this bounded handler path. No allocator, loader or TLS helper is used. */
    volatile unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0U; i < bytes; ++i) out[i] = in[i];
}

static bool contains(uintptr_t low, uintptr_t high, uintptr_t address, size_t bytes) {
    return low < high && address >= low && address <= high && bytes <= high - address;
}

static uint64_t read64(const unsigned char *bytes) {
    uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i) value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}

static bool general(uint64_t pc, uint64_t sp, uint64_t flags) {
    /* FreeBSD preserves the hardware resume flag in native signal contexts;
     * PSL_RF is explicitly user-changeable by sigreturn (sys/x86/psl.h).
     * Keep rejecting tracing, privilege, task and other unsupported flags. */
    return pc != 0U && pc <= UINT64_C(0x00007fffffffffff) && sp != 0U &&
        sp <= UINT64_C(0x00007fffffffffff) && (sp & 7U) == 0U &&
        (flags & ~UINT64_C(0x10ad7)) == 0U && (flags & UINT64_C(0x202)) == UINT64_C(0x202);
}

static bool floating_owner(long owner) {
    return owner == _MC_FPOWNED_NONE || owner == _MC_FPOWNED_FPU || owner == _MC_FPOWNED_PCB;
}

static bool xstate(const unsigned char *extra, size_t bytes) {
    if (bytes == 0U) return true;
    if (bytes != 64U && bytes != LLAM_FREEBSD_X86_XSTATE_BYTES) return false;
    uint64_t bitmap = read64(extra);
    if ((bitmap & ~UINT64_C(7)) != 0U || (bytes == 64U && (bitmap & 4U) != 0U) || read64(extra + 8U) != 0U)
        return false;
    for (size_t i = 16U; i < 64U; ++i) if (extra[i] != 0U) return false;
    return true;
}

static int profile(const ucontext_t *frame, uintptr_t low, uintptr_t high, size_t *extra_bytes) {
    if (frame == NULL || !contains(low, high, (uintptr_t)frame, sizeof(*frame))) return EINVAL;
    const mcontext_t *m = &frame->uc_mcontext;
    /* HASBASES/HASTLSBASE permit nonstandard thread-base manipulation. A
     * normal signal frame carries HASSEGS and the optional FP extension only. */
    if (m->mc_len != (long)sizeof(*m) || m->mc_fpformat != _MC_FPFMT_XMM ||
        !floating_owner(m->mc_ownedfp) ||
        (m->mc_flags & ~(_MC_HASSEGS | _MC_HASFPXSTATE)) != 0U || (m->mc_flags & _MC_HASSEGS) == 0U ||
        (m->mc_cs & 3U) != 3U || (m->mc_ss & 3U) != 3U ||
        (uint64_t)m->mc_cs > UINT16_MAX || (uint64_t)m->mc_ss > UINT16_MAX ||
        !general(m->mc_rip, m->mc_rsp, m->mc_rflags)) return ENOTSUP;
    *extra_bytes = 0U;
    if ((m->mc_flags & _MC_HASFPXSTATE) == 0U) {
        return m->mc_xfpustate == 0U && m->mc_xfpustate_len == 0U ? 0 : ENOTSUP;
    }
    size_t bytes = (size_t)m->mc_xfpustate_len;
    uintptr_t address = (uintptr_t)m->mc_xfpustate;
    if ((bytes != 64U && bytes != LLAM_FREEBSD_X86_XSTATE_BYTES) || (address & 63U) != 0U ||
        !contains(low, high, address, bytes) ||
        (address < (uintptr_t)frame + sizeof(*frame) && (uintptr_t)frame < address + bytes)) return ENOTSUP;
    const unsigned char *extra = (const unsigned char *)address;
    if (!xstate(extra, bytes)) return ENOTSUP;
    *extra_bytes = bytes;
    return 0;
}

static void selectors(const mcontext_t *m, uint16_t out[6]) {
    out[0] = (uint16_t)m->mc_cs;
    out[1] = (uint16_t)m->mc_ss;
    out[2] = m->mc_ds;
    out[3] = m->mc_es;
    out[4] = m->mc_fs;
    out[5] = m->mc_gs;
}

int llam_freebsd_x86_async_read(const ucontext_t *frame, int entry_errno,
                               uintptr_t signal_low, uintptr_t signal_high,
                               llam_freebsd_x86_async_machine_t *out) {
    size_t extra_bytes;
    if (out == NULL) return EINVAL;
    int error = profile(frame, signal_low, signal_high, &extra_bytes);
    if (error != 0) return error;
    const mcontext_t *m = &frame->uc_mcontext;
    copy_bytes(out->gpr, &m->mc_rdi, sizeof(out->gpr));
    out->pc = m->mc_rip;
    out->sp = m->mc_rsp;
    out->flags = m->mc_rflags;
    out->fsbase = m->mc_fsbase;
    out->gsbase = m->mc_gsbase;
    selectors(m, out->selectors);
    copy_bytes(&out->signal_mask, &frame->uc_sigmask, sizeof(out->signal_mask));
    copy_bytes(out->floating, m->mc_fpstate, sizeof(out->floating));
    if (extra_bytes != 0U) copy_bytes(out->xstate, (const void *)(uintptr_t)m->mc_xfpustate, extra_bytes);
    out->xstate_bytes = extra_bytes;
    out->floating_owner = m->mc_ownedfp;
    out->entry_errno = entry_errno;
    out->valid = true;
    return 0;
}

int llam_freebsd_x86_async_redirect(ucontext_t *frame, const llam_ctx_t *scheduler,
                                   const sigset_t *scheduler_mask,
                                   uintptr_t signal_low, uintptr_t signal_high) {
    size_t extra_bytes;
    if (scheduler == NULL || scheduler_mask == NULL || scheduler->rsp == 0U ||
        (scheduler->rsp & 7U) != 0U || scheduler->xsave_area != NULL) return EINVAL;
    int error = profile(frame, signal_low, signal_high, &extra_bytes);
    if (error != 0) return error;
    frame->uc_mcontext.mc_rip = (register_t)(uintptr_t)llam_freebsd_x86_async_restore_scheduler;
    frame->uc_mcontext.mc_rdi = (register_t)(uintptr_t)scheduler;
    copy_bytes(&frame->uc_sigmask, scheduler_mask, sizeof(frame->uc_sigmask));
    return 0;
}

int llam_freebsd_x86_async_restore(ucontext_t *frame, const llam_freebsd_x86_async_machine_t *machine,
                                  uintptr_t signal_low, uintptr_t signal_high) {
    size_t extra_bytes;
    if (machine == NULL || !machine->valid || !general(machine->pc, machine->sp, machine->flags) ||
        !floating_owner(machine->floating_owner) || !xstate(machine->xstate, machine->xstate_bytes)) return EINVAL;
    int error = profile(frame, signal_low, signal_high, &extra_bytes);
    if (error != 0) return error;
    mcontext_t *m = &frame->uc_mcontext;
    uint16_t fresh_selectors[6];
    selectors(m, fresh_selectors);
    if (extra_bytes != machine->xstate_bytes || (uint64_t)m->mc_fsbase != machine->fsbase ||
        (uint64_t)m->mc_gsbase != machine->gsbase) return ENOTSUP;
    for (unsigned i = 0U; i < 6U; ++i) if (fresh_selectors[i] != machine->selectors[i]) return ENOTSUP;
    /* All checks precede the first write. Keep this activation's bookkeeping,
     * TLS bases and extra-state pointer, copying only architectural payload. */
    copy_bytes(&m->mc_rdi, machine->gpr, sizeof(machine->gpr));
    m->mc_rip = (register_t)machine->pc;
    m->mc_rsp = (register_t)machine->sp;
    m->mc_rflags = (register_t)machine->flags;
    m->mc_ownedfp = machine->floating_owner;
    copy_bytes(m->mc_fpstate, machine->floating, sizeof(machine->floating));
    if (extra_bytes != 0U) copy_bytes((void *)(uintptr_t)m->mc_xfpustate, machine->xstate, extra_bytes);
    copy_bytes(&frame->uc_sigmask, &machine->signal_mask, sizeof(frame->uc_sigmask));
    return 0;
}
#endif
