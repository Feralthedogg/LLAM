/**
 * @file src/internal/llam_internal.h
 * @brief Primary internal include that joins runtime types, state, platform, and subsystem prototypes.
 *
 * @details
 * This header defines the minimal ABI shared with hand-written context-switch
 * assembly before the larger runtime type graph is included. The architecture
 * specific ::llam_ctx_t layout must stay in lockstep with the assembly files named
 * in the static assertions.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LLAM_INTERNAL_H
#define LLAM_INTERNAL_H

#include "llam/runtime.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
/** @brief Mark a hot-path condition as expected true for branch layout. */
#define LLAM_LIKELY(expr) (__builtin_expect(!!(expr), 1))
/** @brief Mark a diagnostic/error condition as expected false. */
#define LLAM_UNLIKELY(expr) (__builtin_expect(!!(expr), 0))
#else
#define LLAM_LIKELY(expr) (!!(expr))
#define LLAM_UNLIKELY(expr) (!!(expr))
#endif

#ifndef LLAM_RUNTIME_DISABLE_OWNER_CHECKS
/*
 * Keep owner checks enabled by default because EXDEV is part of the public
 * 2.x owner-diagnostic contract. Release-fast embedders that build their own LLAM
 * binary may define this to 1 only when they intentionally trade cross-owner
 * diagnostics for a zero-cost single-runtime assumption.
 */
#define LLAM_RUNTIME_DISABLE_OWNER_CHECKS 0
#endif

/** @brief Internal task states used by tracing, diagnostics, and assembly-visible code. */
typedef enum llam_task_state_id {
    LLAM_TASK_STATE_NEW = 0,
    LLAM_TASK_STATE_RUNNABLE = 1,
    LLAM_TASK_STATE_RUNNING = 2,
    LLAM_TASK_STATE_PARKED = 3,
    LLAM_TASK_STATE_BLOCKED_OPAQUE = 4,
    LLAM_TASK_STATE_DEAD = 5,
} llam_task_state_id_t;

/** @brief Reason a task left running state or was woken. */
typedef enum llam_wait_reason {
    LLAM_WAIT_NONE = 0,
    LLAM_WAIT_YIELD = 1,
    LLAM_WAIT_JOIN = 2,
    LLAM_WAIT_SLEEP = 3,
    LLAM_WAIT_BLOCKING = 4,
    LLAM_WAIT_IO = 5,
    LLAM_WAIT_CANCEL = 6,
    LLAM_WAIT_MUTEX = 7,
    LLAM_WAIT_COND = 8,
    LLAM_WAIT_CHANNEL_SEND = 9,
    LLAM_WAIT_CHANNEL_RECV = 10,
    LLAM_WAIT_TIMEOUT = 11,
} llam_wait_reason_t;

/** @brief Trace event categories recorded by shard-local trace buffers. */
typedef enum llam_trace_kind {
    LLAM_TRACE_STATE = 1,
    LLAM_TRACE_WAKE = 2,
    LLAM_TRACE_BLOCK_SUBMIT = 3,
    LLAM_TRACE_BLOCK_COMPLETE = 4,
    LLAM_TRACE_IO_SUBMIT = 5,
    LLAM_TRACE_IO_COMPLETE = 6,
    LLAM_TRACE_IDLE = 7,
    LLAM_TRACE_STEAL = 8,
    LLAM_TRACE_WATCHDOG = 9,
} llam_trace_kind_t;

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
/**
 * @brief Windows x86-64 callee-saved context layout.
 *
 * Windows x64 preserves RSI, RDI, and XMM6-XMM15 in addition to the integer
 * registers saved by the SysV x86-64 path. Offsets are consumed directly by
 * @c asm/windows/x86_64/windows_context_x86_64.S.
 */
#define LLAM_CTX_SIMD_F_SKIP_SAVE 0x1U
#define LLAM_CTX_SIMD_F_SKIP_RESTORE 0x2U

typedef struct llam_ctx {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t simd_align_pad;
    uint64_t xmm6[2];
    uint64_t xmm7[2];
    uint64_t xmm8[2];
    uint64_t xmm9[2];
    uint64_t xmm10[2];
    uint64_t xmm11[2];
    uint64_t xmm12[2];
    uint64_t xmm13[2];
    uint64_t xmm14[2];
    uint64_t xmm15[2];
    uint32_t mxcsr;
    uint16_t x87_cw;
    uint16_t pad;
    void *xsave_area;
    uint32_t simd_valid;
    uint32_t simd_flags;
} llam_ctx_t;

_Static_assert(offsetof(llam_ctx_t, rsp) == 0, "llam_ctx_t.rsp offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rbx) == 8, "llam_ctx_t.rbx offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rbp) == 16, "llam_ctx_t.rbp offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rsi) == 24, "llam_ctx_t.rsi offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rdi) == 32, "llam_ctx_t.rdi offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r12) == 40, "llam_ctx_t.r12 offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r15) == 64, "llam_ctx_t.r15 offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, simd_align_pad) == 72, "llam_ctx_t.simd_align_pad offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, xmm6) == 80, "llam_ctx_t.xmm6 offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, xmm15) == 224, "llam_ctx_t.xmm15 offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, mxcsr) == 240, "llam_ctx_t.mxcsr offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, x87_cw) == 244, "llam_ctx_t.x87_cw offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, xsave_area) == 248, "llam_ctx_t.xsave_area offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, simd_valid) == 256, "llam_ctx_t.simd_valid offset must match asm/windows/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, simd_flags) == 260, "llam_ctx_t.simd_flags offset must match asm/windows/x86_64 context switch");
_Static_assert(sizeof(llam_ctx_t) == 264, "llam_ctx_t size must match asm/windows/x86_64 context switch");
#elif (LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) && LLAM_ARCH_X86_64
/**
 * @brief x86-64 callee-saved context layout.
 *
 * The offsets below are consumed directly by
 * @c asm/linux/x86_64/linux_context_x86_64.S and
 * @c asm/darwin/x86_64/darwin_context_x86_64.S. BSD x86-64 targets intentionally use
 * the Linux-labelled SysV assembly path because the saved callee-saved register
 * set and stack ABI are the same for this fiber switch contract.
 */
typedef struct llam_ctx {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint32_t mxcsr;
    uint16_t x87_cw;
    uint16_t pad;
    void *xsave_area;
} llam_ctx_t;

_Static_assert(offsetof(llam_ctx_t, rsp) == 0, "llam_ctx_t.rsp offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rbx) == 8, "llam_ctx_t.rbx offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, rbp) == 16, "llam_ctx_t.rbp offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r12) == 24, "llam_ctx_t.r12 offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r13) == 32, "llam_ctx_t.r13 offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r14) == 40, "llam_ctx_t.r14 offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, r15) == 48, "llam_ctx_t.r15 offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, mxcsr) == 56, "llam_ctx_t.mxcsr offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, x87_cw) == 60, "llam_ctx_t.x87_cw offset must match asm/x86_64 context switch");
_Static_assert(offsetof(llam_ctx_t, xsave_area) == 64, "llam_ctx_t.xsave_area offset must match asm/x86_64 context switch");
_Static_assert(sizeof(llam_ctx_t) == 72, "llam_ctx_t size must match asm/x86_64 context switch");
#elif LLAM_ARCH_AARCH64
/**
 * @brief AArch64 callee-saved context layout.
 *
 * Integer callee-saved registers, FP/LR, SIMD d8-d15, and FP control/status are
 * stored in the exact order expected by the AArch64 context switch assembly.
 */
#define LLAM_CTX_SIMD_F_SKIP_SAVE 0x1U
#define LLAM_CTX_SIMD_F_SKIP_RESTORE 0x2U

typedef struct llam_ctx {
    uint64_t sp;
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t fp;
    uint64_t lr;
    uint64_t d8_bits;
    uint64_t d9_bits;
    uint64_t d10_bits;
    uint64_t d11_bits;
    uint64_t d12_bits;
    uint64_t d13_bits;
    uint64_t d14_bits;
    uint64_t d15_bits;
    uint64_t fpcr;
    uint64_t fpsr;
    uint64_t simd_flags;
} llam_ctx_t;

_Static_assert(offsetof(llam_ctx_t, sp) == 0, "llam_ctx_t.sp offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, x19) == 8, "llam_ctx_t.x19 offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, x28) == 80, "llam_ctx_t.x28 offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, fp) == 88, "llam_ctx_t.fp offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, lr) == 96, "llam_ctx_t.lr offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, d8_bits) == 104, "llam_ctx_t.d8_bits offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, d15_bits) == 160, "llam_ctx_t.d15_bits offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, fpcr) == 168, "llam_ctx_t.fpcr offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, fpsr) == 176, "llam_ctx_t.fpsr offset must match asm/arm64 context switch");
_Static_assert(offsetof(llam_ctx_t, simd_flags) == 184, "llam_ctx_t.simd_flags offset must match asm/arm64 context switch");
_Static_assert(sizeof(llam_ctx_t) == 192, "llam_ctx_t size must match asm/arm64 context switch");
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <fenv.h>
#include <ucontext.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/**
 * @brief Portable ucontext-backed context layout.
 *
 * Used only when no architecture-specific assembly context switch is selected.
 */
typedef struct llam_ctx {
    ucontext_t uc;
    fenv_t fenv;
    int fenv_valid;
} llam_ctx_t;
#endif

/*
 * Context-switch and fiber bootstrap ABI.
 *
 * These functions are implemented either by assembly or the portable ucontext
 * backend. The noreturn functions terminate a task stack back into scheduler
 * code and must never return to their caller.
 */
#if defined(_MSC_VER)
#define LLAM_NORETURN __declspec(noreturn)
#else
#define LLAM_NORETURN __attribute__((noreturn))
#endif

/*
 * Internal helper visibility.
 *
 * LLAM_API is reserved for the supported C ABI. Internal test hooks and broker
 * helpers may still need cross-translation-unit linkage inside tests, but they
 * must not become dynamic-library entry points on ELF/Mach-O builds.
 */
#if defined(_WIN32)
#define LLAM_INTERNAL_API
#elif defined(__GNUC__) || defined(__clang__)
#define LLAM_INTERNAL_API __attribute__((visibility("hidden")))
#else
#define LLAM_INTERNAL_API
#endif

void llam_ctx_switch(llam_ctx_t *from, const llam_ctx_t *to);
void llam_fiber_bootstrap(void);
LLAM_NORETURN void llam_fiber_alignment_violation(uint64_t rsp);
LLAM_NORETURN void llam_task_bootstrap(struct llam_task *task);
LLAM_NORETURN void llam_task_exit_internal(void);
int llam_ctx_make_task(llam_ctx_t *ctx, void *stack_base, size_t stack_size, struct llam_task *task);

/*
 * Diagnostic formatting helpers shared by dumps, traces, and examples.
 */
const char *llam_state_name_from_id(llam_task_state_id_t state);
const char *llam_wait_reason_name(llam_wait_reason_t reason);
size_t llam_stack_bytes(llam_stack_class_t stack_class);

#endif
