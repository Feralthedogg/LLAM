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

#include "async_context_freebsd_x86_64.h"
#include <stdio.h>

#if !defined(__FreeBSD__) || !LLAM_ARCH_X86_64 || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) { puts("FreeBSD amd64 native signal transport unavailable"); return 77; }
#else
#include "async_context_phase.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(LLAM_ASYNC_CONTEXT_STANDALONE)
uint32_t g_llam_fp_control_context = 1U;
uint32_t g_llam_xsave_mask_lo;
uint32_t g_llam_xsave_mask_hi;
LLAM_NORETURN void llam_task_bootstrap(struct llam_task *task) { (void)task; abort(); }
LLAM_NORETURN void llam_fiber_alignment_violation(uint64_t rsp) { (void)rsp; abort(); }
#else
extern uint32_t g_llam_fp_control_context;
#endif

extern unsigned llam_async_safe_leaf(atomic_uint *release);
extern unsigned char llam_async_safe_leaf_begin[], llam_async_safe_leaf_end[];
extern void llam_async_freebsd_fixture_entry(void);

enum { REQUEST_IDLE, REQUEST_CAPTURE, REQUEST_RESTORE, REQUEST_WITNESS, REQUEST_BUSY };
typedef struct fixture {
    llam_ctx_t scheduler;
    llam_ctx_t fiber;
    llam_freebsd_x86_async_machine_t machine;
    ucontext_t frame_snapshot;
    atomic_uint request;
    atomic_uint captures;
    atomic_uint restores;
    atomic_uint witnesses;
    atomic_uint gate;
    atomic_uint started;
    atomic_uint done;
    atomic_uint release_leaf;
    atomic_int error;
    sigset_t scheduler_mask;
    int *errno_location;
    uintptr_t signal_low;
    uintptr_t signal_high;
    uint64_t refusal[16];
    unsigned result;
} fixture_t;

static fixture_t *g_fixture;
static _Alignas(16) unsigned char alternate_stack[65536];

static void snapshot_bytes(void *destination, const void *source, size_t size) {
    volatile unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0U; i < size; ++i) out[i] = in[i];
}

static void signal_handler(int number, siginfo_t *info, void *opaque) {
    fixture_t *f = g_fixture;
    if (number != SIGUSR2 || info == NULL || f == NULL || f->errno_location == NULL) return;
    int entry_errno = *f->errno_location;
    ucontext_t *frame = opaque;
    unsigned request = atomic_load_explicit(&f->request, memory_order_acquire);
    uintptr_t pc = (uintptr_t)frame->uc_mcontext.mc_rip;
    bool leaf = pc >= (uintptr_t)llam_async_safe_leaf_begin && pc < (uintptr_t)llam_async_safe_leaf_end;
    bool gate = pc >= (uintptr_t)llam_freebsd_x86_async_gate_begin && pc < (uintptr_t)llam_freebsd_x86_async_gate_end;
    if ((request == REQUEST_CAPTURE && leaf) || (request == REQUEST_RESTORE && gate) ||
        (request == REQUEST_WITNESS && leaf)) {
        if (!atomic_compare_exchange_strong_explicit(&f->request, &request, REQUEST_BUSY,
                                                     memory_order_acq_rel, memory_order_acquire)) return;
        int error = 0;
        if (request == REQUEST_CAPTURE) {
            snapshot_bytes(&f->frame_snapshot, frame, sizeof(*frame));
            error = llam_freebsd_x86_async_read(frame, entry_errno, f->signal_low, f->signal_high, &f->machine);
            if (error == 0) error = llam_freebsd_x86_async_redirect(frame, &f->scheduler, &f->scheduler_mask,
                                                                   f->signal_low, f->signal_high);
            if (error == 0) {
                *f->errno_location = EAGAIN;
                atomic_fetch_add_explicit(&f->captures, 1U, memory_order_release);
            }
        } else if (request == REQUEST_RESTORE) {
            error = llam_freebsd_x86_async_restore(frame, &f->machine, f->signal_low, f->signal_high);
            if (error == 0) {
                *f->errno_location = f->machine.entry_errno;
                atomic_fetch_add_explicit(&f->restores, 1U, memory_order_release);
            }
        } else {
            atomic_fetch_add_explicit(&f->witnesses, 1U, memory_order_release);
        }
        if (error != 0) {
            const mcontext_t *m = &frame->uc_mcontext;
            f->refusal[0] = request;
            f->refusal[1] = (uint64_t)m->mc_len;
            f->refusal[2] = (uint64_t)m->mc_fpformat;
            f->refusal[3] = (uint64_t)m->mc_ownedfp;
            f->refusal[4] = m->mc_flags;
            f->refusal[5] = (uint64_t)m->mc_rflags;
            f->refusal[6] = (uint64_t)m->mc_cs;
            f->refusal[7] = (uint64_t)m->mc_ss;
            f->refusal[8] = (uint64_t)m->mc_xfpustate;
            f->refusal[9] = (uint64_t)m->mc_xfpustate_len;
            f->refusal[10] = (uint64_t)m->mc_rsp;
            f->refusal[11] = (uintptr_t)frame;
            f->refusal[12] = f->signal_low;
            f->refusal[13] = f->signal_high;
            f->refusal[14] = (uint64_t)m->mc_fsbase;
            f->refusal[15] = (uint64_t)m->mc_gsbase;
            atomic_store_explicit(&f->error, error, memory_order_release);
            *f->errno_location = entry_errno;
        }
        atomic_store_explicit(&f->request, REQUEST_IDLE, memory_order_release);
    }
}

void llam_async_freebsd_fixture_body(void *opaque) {
    fixture_t *f = opaque;
    sigset_t mask = f->scheduler_mask;
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGRTMIN + 1);
    sigdelset(&mask, SIGRTMAX);
    if (pthread_sigmask(SIG_SETMASK, &mask, NULL) != 0) abort();
    const uint16_t cw = UINT16_C(0x077f);
    const uint32_t mxcsr = UINT32_C(0x3f80);
    __asm__ volatile("fldcw %0; ldmxcsr %1" : : "m"(cw), "m"(mxcsr) : "memory");
    errno = EDOM;
    atomic_store_explicit(&f->started, 1U, memory_order_release);
    f->result = llam_async_safe_leaf(&f->release_leaf);
    if (errno != EDOM || pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
        sigismember(&mask, SIGUSR1) != 1 || sigismember(&mask, SIGRTMIN + 1) != 1 ||
        sigismember(&mask, SIGRTMAX) != 0 || sigismember(&mask, SIGUSR2) != 0)
        atomic_store_explicit(&f->error, EIO, memory_order_release);
    uint16_t restored_cw;
    uint32_t restored_mxcsr;
    __asm__ volatile("fnstcw %0; stmxcsr %1" : "=m"(restored_cw), "=m"(restored_mxcsr));
    if (restored_cw != cw || restored_mxcsr != mxcsr) atomic_store(&f->error, EIO);
    atomic_store_explicit(&f->done, 1U, memory_order_release);
    llam_ctx_switch(&f->fiber, &f->scheduler);
    abort();
}

static void *worker(void *opaque) {
    fixture_t *f = opaque;
    stack_t previous_stack;
    stack_t stack = {.ss_sp = alternate_stack, .ss_size = sizeof(alternate_stack)};
    sigset_t previous_mask;
    if (sigaltstack(&stack, &previous_stack) != 0 || pthread_sigmask(SIG_SETMASK, NULL, &previous_mask) != 0) abort();
    f->scheduler_mask = previous_mask;
    sigdelset(&f->scheduler_mask, SIGUSR1);
    sigdelset(&f->scheduler_mask, SIGUSR2);
    sigdelset(&f->scheduler_mask, SIGRTMIN + 1);
    sigaddset(&f->scheduler_mask, SIGRTMAX);
    if (pthread_sigmask(SIG_SETMASK, &f->scheduler_mask, NULL) != 0) abort();
    f->errno_location = &errno;
    const uint16_t cw = UINT16_C(0x037f);
    const uint32_t mxcsr = UINT32_C(0x1f80);
    __asm__ volatile("fldcw %0; ldmxcsr %1" : : "m"(cw), "m"(mxcsr) : "memory");
    errno = EAGAIN;
    llam_ctx_switch(&f->scheduler, &f->fiber);
    while (atomic_load_explicit(&f->done, memory_order_acquire) == 0U) {
        sigset_t mask;
        uint16_t got_cw;
        uint32_t got_mxcsr;
        __asm__ volatile("fnstcw %0; stmxcsr %1" : "=m"(got_cw), "=m"(got_mxcsr));
        if (errno != EAGAIN || got_cw != cw || got_mxcsr != mxcsr ||
            pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 || sigismember(&mask, SIGRTMAX) != 1 ||
            sigismember(&mask, SIGUSR1) != 0 || sigismember(&mask, SIGRTMIN + 1) != 0) abort();
        llam_freebsd_x86_async_save_gate(&f->scheduler, &f->gate);
    }
    if (sigaltstack(&previous_stack, NULL) != 0 || pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) != 0) abort();
    return NULL;
}

static void wait_value(const atomic_uint *value, unsigned expected) {
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
    while (atomic_load_explicit(value, memory_order_acquire) != expected) {
        if (atomic_load_explicit(&g_fixture->error, memory_order_acquire) != 0) {
            fprintf(stderr, "native transport refused: %d\n", atomic_load(&g_fixture->error));
            for (unsigned i = 0U; i < 16U; ++i)
                fprintf(stderr, "refusal[%u]=%#llx\n", i, (unsigned long long)g_fixture->refusal[i]);
            _Exit(1);
        }
        nanosleep(&pause, NULL);
    }
}

static void request(pthread_t thread, fixture_t *f, unsigned kind, const atomic_uint *counter, unsigned expected) {
    wait_value(&f->request, REQUEST_IDLE);
    atomic_store_explicit(&f->request, kind, memory_order_release);
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
    while (atomic_load_explicit(counter, memory_order_acquire) != expected) {
        if (pthread_kill(thread, SIGUSR2) != 0) abort();
        if (atomic_load_explicit(&f->error, memory_order_acquire) != 0) {
            fprintf(stderr, "native transport refused: %d\n", atomic_load(&f->error));
            for (unsigned i = 0U; i < 16U; ++i)
                fprintf(stderr, "refusal[%u]=%#llx\n", i, (unsigned long long)f->refusal[i]);
            _Exit(1);
        }
        nanosleep(&pause, NULL);
    }
    wait_value(&f->request, REQUEST_IDLE);
}

static bool payload_matches(const llam_freebsd_x86_async_machine_t *m) {
    return m->valid && (m->flags & UINT64_C(0x41)) == UINT64_C(0x41) &&
        m->gpr[6] == UINT64_C(0x10203040) && m->gpr[7] == UINT64_C(0x20304050) &&
        m->gpr[14] == UINT64_C(0x45670123) && m->entry_errno == EDOM &&
        sigismember(&m->signal_mask, SIGRTMIN + 1) == 1 && sigismember(&m->signal_mask, SIGRTMAX) == 0;
}

int main(void) {
    fixture_t f = {0};
    pthread_t thread;
    struct sigaction previous;
    struct sigaction action = {.sa_sigaction = signal_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t stack_bytes = 65536U;
    void *mapping = mmap(NULL, stack_bytes + 2U * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED || mprotect((unsigned char *)mapping + page, stack_bytes, PROT_READ | PROT_WRITE) != 0) return 1;
    uint64_t *sp = (uint64_t *)((uintptr_t)mapping + page + stack_bytes);
    *--sp = 0U;
    *--sp = (uint64_t)(uintptr_t)llam_async_freebsd_fixture_entry;
    f.fiber.rsp = (uint64_t)(uintptr_t)sp;
    f.fiber.r12 = (uint64_t)(uintptr_t)&f;
    f.fiber.mxcsr = UINT32_C(0x1f80);
    f.fiber.x87_cw = UINT16_C(0x037f);
    f.signal_low = (uintptr_t)alternate_stack;
    f.signal_high = f.signal_low + sizeof(alternate_stack);
    g_llam_fp_control_context = 1U;
    g_fixture = &f;
    sigfillset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, &previous) != 0 || pthread_create(&thread, NULL, worker, &f) != 0) return 1;
    alarm(15U);
    wait_value(&f.started, 1U);
    for (unsigned i = 1U; i <= 4U; ++i) {
        request(thread, &f, REQUEST_CAPTURE, &f.captures, i);
        wait_value(&f.gate, LLAM_ASYNC_BANK_GATE);
        if (!payload_matches(&f.machine)) return 1;
        llam_freebsd_x86_async_machine_t invalid = f.machine;
        invalid.flags |= UINT64_C(0x3000); /* IOPL must remain unrepresentable. */
        ucontext_t unchanged = f.frame_snapshot;
        if (llam_freebsd_x86_async_restore(&f.frame_snapshot, &invalid,
                (uintptr_t)&f.frame_snapshot, (uintptr_t)(&f.frame_snapshot + 1)) != EINVAL ||
            memcmp(&unchanged, &f.frame_snapshot, sizeof(unchanged)) != 0) return 1;
        atomic_store_explicit(&f.gate, 0U, memory_order_release);
        request(thread, &f, REQUEST_RESTORE, &f.restores, i);
        request(thread, &f, REQUEST_WITNESS, &f.witnesses, i);
    }
    atomic_store_explicit(&f.release_leaf, 1U, memory_order_release);
    if (pthread_join(thread, NULL) != 0 || f.result != 1U || atomic_load(&f.error) != 0 ||
        sigaction(SIGUSR2, &previous, NULL) != 0 || munmap(mapping, stack_bytes + 2U * page) != 0) return 1;
    alarm(0U);
    printf("PASS: FreeBSD signal capture/redirect/fresh restore/witness=4; GPR/flags/red-zone/FP/128-bit mask/errno; xstate=%zu\n",
           f.machine.xstate_bytes);
    return 0;
}
#endif
