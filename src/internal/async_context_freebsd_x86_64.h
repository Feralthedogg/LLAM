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

#ifndef LLAM_ASYNC_CONTEXT_FREEBSD_X86_64_H
#define LLAM_ASYNC_CONTEXT_FREEBSD_X86_64_H

#include "llam_internal.h"

#if defined(__FreeBSD__) && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <ucontext.h>

enum { LLAM_FREEBSD_X86_XSTATE_BYTES = 320U };

/* Native payload only: no old ucontext, altstack, trap/error fields, xstate
 * pointer or restorer activation survives a capture. TLS bases/selectors are
 * identity constraints on the original M, never a per-G thread-base restore. */
typedef struct llam_freebsd_x86_async_machine {
    uint64_t gpr[15]; /* Native mc_rdi..mc_r15 order, statically checked in C. */
    uint64_t pc;
    uint64_t sp;
    uint64_t flags;
    uint64_t fsbase;
    uint64_t gsbase;
    uint16_t selectors[6]; /* CS, SS, DS, ES, FS, GS. */
    sigset_t signal_mask; /* All 128 FreeBSD signal bits. */
    _Alignas(16) unsigned char floating[512];
    _Alignas(64) unsigned char xstate[LLAM_FREEBSD_X86_XSTATE_BYTES];
    size_t xstate_bytes;
    long floating_owner;
    int entry_errno;
    bool valid;
} llam_freebsd_x86_async_machine_t;

/* Signal-handler transport only. Bounds name the owning M's independently
 * installed altstack, not uc_stack supplied by a caller. Every refusal leaves
 * frame/output unchanged. The caller owns/pins disjoint storage and checks the
 * exact request tuple; valid alone never authorizes restoration. Return error
 * numbers without changing errno; successful callers return normally through
 * FreeBSD's fresh signal trampoline. Unknown XSTATE/TLS profiles are refused. */
int llam_freebsd_x86_async_read(const ucontext_t *frame, int entry_errno,
                               uintptr_t signal_low, uintptr_t signal_high,
                               llam_freebsd_x86_async_machine_t *out);
int llam_freebsd_x86_async_redirect(ucontext_t *frame, const llam_ctx_t *scheduler,
                                   const sigset_t *scheduler_mask,
                                   uintptr_t signal_low, uintptr_t signal_high);
int llam_freebsd_x86_async_restore(ucontext_t *frame, const llam_freebsd_x86_async_machine_t *machine,
                                  uintptr_t signal_low, uintptr_t signal_high);

void llam_freebsd_x86_async_restore_scheduler(void);
void llam_freebsd_x86_async_save_gate(llam_ctx_t *scheduler, atomic_uint *phase);
extern const unsigned char llam_freebsd_x86_async_gate_begin[];
extern const unsigned char llam_freebsd_x86_async_gate_end[];
#endif

#endif /* LLAM_ASYNC_CONTEXT_FREEBSD_X86_64_H */
