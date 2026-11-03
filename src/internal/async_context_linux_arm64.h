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

#ifndef LLAM_ASYNC_CONTEXT_LINUX_ARM64_H
#define LLAM_ASYNC_CONTEXT_LINUX_ARM64_H

#include "llam_internal.h"

#if LLAM_PLATFORM_LINUX && LLAM_ARCH_AARCH64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <stdbool.h>
#include <signal.h>
#include <stdatomic.h>
#include <ucontext.h>

/* A native payload, not an old signal frame. In particular, uc_stack, uc_link,
 * frame cookies/pointers, record headers and restorer state are never retained.
 * The owner pins this storage, the G stack, M continuation and request before
 * signal publication, and enforces origin-M identity outside this transport.
 * PSTATE admits NZCV|SSBS only. The WHOLE native-thread profile (LLAM, Gs, peers,
 * callbacks, signal handlers and external agents) must not change speculation
 * policy or directly manage SSBS while continuations exist. This preserves
 * architectural context; it does not virtualize per-G mitigation policy. */
typedef struct llam_linux_arm64_async_machine {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t signal_mask;
    _Alignas(16) unsigned char q[32][16];
    uint32_t fpcr;
    uint32_t fpsr;
    int entry_errno;
    bool valid;
} llam_linux_arm64_async_machine_t;

/* Bounded async-signal-safe transport. Return an error NUMBER, without changing
 * errno or publishing ownership. Failure never changes the input frame or out.
 * Only the initial FPSIMD profile is supported; SVE/SME/unknown state defers.
 * The caller captures entry errno before any handler work and returns normally
 * from the handler after success. Do not call a restorer or replay an old frame.
 * All arguments must be disjoint, owned, prevalidated readable/writable storage. */
/* An unchanged output after refusal can still contain an OLD valid payload.
 * Always check the error and exact owning request; valid is not authorization. */
int llam_linux_arm64_async_read(const ucontext_t *frame,
                              int entry_errno,
                              llam_linux_arm64_async_machine_t *out);
int llam_linux_arm64_async_redirect(ucontext_t *frame,
                                  const llam_ctx_t *scheduler,
                                  const sigset_t *scheduler_mask);
int llam_linux_arm64_async_restore(ucontext_t *fresh_frame,
                                 const llam_linux_arm64_async_machine_t *machine);

extern void llam_linux_arm64_async_restore_scheduler(void);
extern void llam_linux_arm64_async_save_gate(llam_ctx_t *scheduler, atomic_uint *phase);
extern const unsigned char llam_linux_arm64_async_gate_begin[];
extern const unsigned char llam_linux_arm64_async_gate_end[];
#endif
#endif
