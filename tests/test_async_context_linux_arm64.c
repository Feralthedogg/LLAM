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

#include "llam/platform.h"

#include <stdio.h>

#if !LLAM_PLATFORM_LINUX || !LLAM_ARCH_AARCH64 || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) {
    puts("test_async_context_linux_arm64: skipped (native mechanism disabled)");
    return 77;
}
#else
#include "async_context_linux_arm64.h"
#include "async_context_phase.h"
#include "fixtures/async_darwin_leaf.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define FIXTURE_ITERATIONS UINT64_C(300000000)
#define FIXTURE_CAPTURES 8U
#define FIXTURE_CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Linux arm64 fixture failed at line %d: %s\n", __LINE__, #expr); abort(); \
} } while (0)

enum { FIXTURE_CAPTURE = 1U, FIXTURE_RESTORE = 2U, FIXTURE_WITNESS = 3U };

typedef struct fixture_policy {
    int value;
    int error;
} fixture_policy_t;

static fixture_policy_t fixture_read_policy(void) {
    int saved_errno = errno;
    fixture_policy_t policy;
    policy.value = prctl(PR_GET_SPECULATION_CTRL, (unsigned long)PR_SPEC_STORE_BYPASS,
                        0UL, 0UL, 0UL);
    policy.error = policy.value < 0 ? errno : 0;
    errno = saved_errno;
    return policy;
}

typedef struct fixture {
    llam_ctx_t scheduler;
    llam_ctx_t fiber;
    llam_linux_arm64_async_machine_t machine;
    llam_async_darwin_snapshot_t snapshot;
    sigset_t scheduler_mask;
    sigset_t task_mask;
    uintptr_t stack_low;
    uintptr_t stack_high;
    uint64_t result;
    atomic_uint request;
    atomic_uint response;
    atomic_uint phase;
    atomic_uint started;
    atomic_uint done;
    atomic_uint quit;
    atomic_uint errors;
    atomic_uint captures;
    atomic_uint restores;
    atomic_uint witnesses;
    atomic_uint peers;
    fixture_policy_t policy_before;
    fixture_policy_t policy_after;
    _Alignas(16) unsigned char altstack[65536];
} fixture_t;

static _Thread_local fixture_t *fixture_target;
extern void llam_async_linux_arm64_fixture_start(void);

/* The standalone context-switch object also contains its ordinary bootstrap.
 * These are unreachable in this mechanism fixture; do not discard undefined
 * symbols or link a different switch implementation just to build the test. */
LLAM_NORETURN void llam_task_bootstrap(struct llam_task *task) { (void)task; abort(); }
LLAM_NORETURN void llam_task_exit_internal(void) { abort(); }

static uint64_t fixture_now_ns(void) {
    struct timespec t;
    FIXTURE_CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void fixture_pause(void) {
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000L};
    (void)nanosleep(&delay, NULL);
}

static bool fixture_same_mask(const sigset_t *a, const sigset_t *b) {
    uint64_t left, right;
    memcpy(&left, a, sizeof(left));
    memcpy(&right, b, sizeof(right));
    return left == right;
}

static void fixture_fp(uint64_t *control, uint64_t *status) {
    __asm__ volatile("mrs %0, fpcr" : "=r"(*control));
    __asm__ volatile("mrs %0, fpsr" : "=r"(*status));
}

static bool fixture_leaf_pc(uint64_t pc) {
    return pc >= (uintptr_t)llam_async_darwin_leaf_begin &&
           pc < (uintptr_t)llam_async_darwin_leaf_end;
}

static bool fixture_stack(const fixture_t *f, uint64_t sp) {
    return sp >= f->stack_low && sp < f->stack_high && (sp & 15U) == 0U;
}

static void fixture_handler(int signal_number, siginfo_t *info, void *opaque) {
    int entry_errno = errno;
    fixture_t *f = fixture_target;
    ucontext_t *frame = opaque;
    unsigned request;
    int error = EAGAIN;
    (void)signal_number;

    if (f == NULL || info == NULL || info->si_code != SI_TKILL) {
        errno = entry_errno;
        return;
    }
    request = atomic_load_explicit(&f->request, memory_order_acquire);
    if (request == 0U) {
        errno = entry_errno;
        return;
    }
    if (request == FIXTURE_CAPTURE &&
        atomic_load_explicit(&f->phase, memory_order_acquire) == LLAM_ASYNC_BANK_FREE &&
        fixture_leaf_pc(frame->uc_mcontext.pc) && fixture_stack(f, frame->uc_mcontext.sp)) {
        error = llam_linux_arm64_async_read(frame, entry_errno, &f->machine);
        if (error == 0) {
            error = llam_linux_arm64_async_redirect(frame, &f->scheduler, &f->scheduler_mask);
        }
        if (error == 0) {
            atomic_fetch_add_explicit(&f->captures, 1U, memory_order_relaxed);
            atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_CAPTURED, memory_order_release);
            entry_errno = ERANGE; /* M's independently selected errno. */
        }
    } else if (request == FIXTURE_RESTORE &&
               atomic_load_explicit(&f->phase, memory_order_acquire) == LLAM_ASYNC_BANK_GATE &&
               frame->uc_mcontext.pc >= (uintptr_t)llam_linux_arm64_async_gate_begin &&
               frame->uc_mcontext.pc < (uintptr_t)llam_linux_arm64_async_gate_end &&
               frame->uc_mcontext.sp == f->scheduler.sp) {
        error = llam_linux_arm64_async_restore(frame, &f->machine);
        if (error == 0) {
            atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_RESTORING, memory_order_release);
            atomic_fetch_add_explicit(&f->restores, 1U, memory_order_relaxed);
            entry_errno = f->machine.entry_errno;
        }
    } else if (request == FIXTURE_WITNESS) {
        unsigned phase = atomic_load_explicit(&f->phase, memory_order_acquire);
        if (phase == LLAM_ASYNC_BANK_RESTORING && fixture_leaf_pc(frame->uc_mcontext.pc) &&
            fixture_stack(f, frame->uc_mcontext.sp)) {
            /* This is a later genuine signal delivery on G, not the restore
             * handler's acknowledgement or a synthesized completion event. */
            atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_OBSERVED, memory_order_release);
            atomic_fetch_add_explicit(&f->witnesses, 1U, memory_order_relaxed);
            error = 0;
        } else if (phase == LLAM_ASYNC_BANK_OBSERVED) {
            error = 0; /* Actual G return already supplied the witness. */
        }
    }
    if (error != 0 && error != EAGAIN) {
        atomic_fetch_add_explicit(&f->errors, 1U, memory_order_relaxed);
    }
    atomic_store_explicit(&f->request, 0U, memory_order_release);
    atomic_store_explicit(&f->response, (unsigned)error + 1U, memory_order_release);
    errno = entry_errno;
    /* Return normally through the OS restorer with this delivery's frame. */
}

void llam_async_linux_arm64_fixture_body(void *opaque) {
    fixture_t *f = opaque;
    sigset_t mask;
    FIXTURE_CHECK(pthread_sigmask(SIG_SETMASK, &f->task_mask, NULL) == 0);
    errno = EDOM;
    atomic_store_explicit(&f->started, 1U, memory_order_release);
    f->result = llam_async_darwin_leaf(FIXTURE_ITERATIONS, &f->snapshot);
    if (errno != EDOM || pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
        !fixture_same_mask(&mask, &f->task_mask)) {
        atomic_fetch_add_explicit(&f->errors, 1U, memory_order_relaxed);
    }
    if (atomic_load_explicit(&f->phase, memory_order_acquire) == LLAM_ASYNC_BANK_RESTORING) {
        atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_OBSERVED, memory_order_release);
        atomic_fetch_add_explicit(&f->witnesses, 1U, memory_order_relaxed);
    }
    FIXTURE_CHECK(pthread_sigmask(SIG_SETMASK, &f->scheduler_mask, NULL) == 0);
    errno = ERANGE;
    atomic_store_explicit(&f->done, 1U, memory_order_release);
    llam_ctx_switch(&f->fiber, &f->scheduler);
    abort();
}

static void *fixture_worker(void *opaque) {
    fixture_t *f = opaque;
    stack_t stack = {.ss_sp = f->altstack, .ss_size = sizeof(f->altstack)}, previous_stack;
    sigset_t previous_mask;
    uint64_t previous_control, previous_status;
    uint64_t control, status;

    fixture_target = f;
    f->policy_before = fixture_read_policy(); /* Actual target M, not controller. */
    fixture_fp(&previous_control, &previous_status);
    FIXTURE_CHECK(pthread_sigmask(SIG_SETMASK, &f->scheduler_mask, &previous_mask) == 0);
    FIXTURE_CHECK(sigaltstack(&stack, &previous_stack) == 0);
    __asm__ volatile("msr fpcr, %0" : : "r"(UINT64_C(0x400000)));
    __asm__ volatile("msr fpsr, %0" : : "r"(UINT64_C(1)));
    f->fiber.sp = f->stack_high;
    f->fiber.lr = (uint64_t)(uintptr_t)llam_async_linux_arm64_fixture_start;
    f->fiber.x19 = (uint64_t)(uintptr_t)f;
    errno = ERANGE;
    llam_ctx_switch(&f->scheduler, &f->fiber);
    for (;;) {
        sigset_t mask;
        fixture_fp(&control, &status);
        if (control != UINT64_C(0x400000) || status != UINT64_C(1) || errno != ERANGE ||
            pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
            !fixture_same_mask(&mask, &f->scheduler_mask)) {
            atomic_fetch_add_explicit(&f->errors, 1U, memory_order_relaxed);
        }
        if (atomic_load_explicit(&f->done, memory_order_acquire) != 0U) break;
        FIXTURE_CHECK(atomic_load_explicit(&f->phase, memory_order_acquire) == LLAM_ASYNC_BANK_CAPTURED);
        /* Off-G native arrival and independent peer work. No handler queueing. */
        atomic_fetch_add_explicit(&f->peers, 1U, memory_order_relaxed);
        atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_READY, memory_order_release);
        errno = ERANGE;
        llam_linux_arm64_async_save_gate(&f->scheduler, &f->phase);
    }
    f->policy_after = fixture_read_policy();
    while (atomic_load_explicit(&f->quit, memory_order_acquire) == 0U) fixture_pause();
    __asm__ volatile("msr fpcr, %0" : : "r"(previous_control));
    __asm__ volatile("msr fpsr, %0" : : "r"(previous_status));
    FIXTURE_CHECK(sigaltstack(&previous_stack, NULL) == 0);
    FIXTURE_CHECK(pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) == 0);
    fixture_target = NULL;
    return NULL;
}

static int fixture_request(fixture_t *f, pthread_t worker, unsigned request, uint64_t deadline) {
    unsigned response;
    FIXTURE_CHECK(atomic_load_explicit(&f->request, memory_order_acquire) == 0U);
    atomic_store_explicit(&f->response, 0U, memory_order_relaxed);
    /* The fixture and native continuation are pre-owned until pthread_join.
     * Release/acquire publishes the immutable request to this target only. */
    atomic_store_explicit(&f->request, request, memory_order_release);
    FIXTURE_CHECK(pthread_kill(worker, SIGUSR1) == 0);
    while ((response = atomic_load_explicit(&f->response, memory_order_acquire)) == 0U) {
        FIXTURE_CHECK(fixture_now_ns() < deadline);
        fixture_pause();
    }
    FIXTURE_CHECK(atomic_load_explicit(&f->errors, memory_order_relaxed) == 0U);
    return (int)response - 1;
}

static void fixture_record(ucontext_t *frame, size_t offset, uint32_t magic, uint32_t bytes) {
    uint32_t header[2] = {magic, bytes};
    memcpy(frame->uc_mcontext.__reserved + offset, header, sizeof(header));
}

static void fixture_negative(ucontext_t *bad) {
    ucontext_t before = *bad;
    llam_linux_arm64_async_machine_t machine, original;
    llam_ctx_t scheduler = {.sp = 0x3000U, .lr = 0x4000U};
    sigset_t mask;
    memset(&machine, 0xa5, sizeof(machine));
    original = machine;
    sigemptyset(&mask);
    FIXTURE_CHECK(llam_linux_arm64_async_read(bad, EDOM, &machine) == ENOTSUP);
    FIXTURE_CHECK(memcmp(&machine, &original, sizeof(machine)) == 0);
    FIXTURE_CHECK(llam_linux_arm64_async_redirect(bad, &scheduler, &mask) == ENOTSUP);
    machine.valid = true;
    machine.sp = 0x2000U;
    machine.pc = 0x1000U;
    machine.pstate = 0U;
    FIXTURE_CHECK(llam_linux_arm64_async_restore(bad, &machine) == ENOTSUP);
    FIXTURE_CHECK(memcmp(bad, &before, sizeof(before)) == 0);
}

static void fixture_ssbs_tests(void) {
    const uint64_t ssbs = UINT64_C(0x1000);
    ucontext_t saved = {0}, fresh, expected;
    llam_linux_arm64_async_machine_t machine;
    llam_ctx_t scheduler = {.sp = 0x3000U, .lr = 0x4000U};
    sigset_t mask;

    saved.uc_mcontext.pc = 0x1000U;
    saved.uc_mcontext.sp = 0x2000U;
    fixture_record(&saved, 0U, 0x46508001U, 528U);
    sigemptyset(&mask);
    /* Entirely synthetic storage. None of these combinations reach sigreturn. */
    for (unsigned saved_bit = 0U; saved_bit < 2U; ++saved_bit) {
        saved.uc_mcontext.pstate = UINT64_C(0xa0000000) | (saved_bit ? ssbs : 0U);
        FIXTURE_CHECK(llam_linux_arm64_async_read(&saved, EDOM, &machine) == 0);
        FIXTURE_CHECK(machine.pstate == saved.uc_mcontext.pstate);
        for (unsigned fresh_bit = 0U; fresh_bit < 2U; ++fresh_bit) {
            fresh = saved;
            fresh.uc_mcontext.pstate = UINT64_C(0x60000000) | (fresh_bit ? ssbs : 0U);
            expected = saved;
            FIXTURE_CHECK(llam_linux_arm64_async_restore(&fresh, &machine) == 0);
            FIXTURE_CHECK(memcmp(&fresh, &expected, sizeof(fresh)) == 0);
        }
        fresh = saved;
        expected = saved;
        expected.uc_mcontext.regs[0] = (uint64_t)(uintptr_t)&scheduler;
        expected.uc_mcontext.pc = (uint64_t)(uintptr_t)llam_linux_arm64_async_restore_scheduler;
        expected.uc_mcontext.sp = scheduler.sp;
        expected.uc_mcontext.pstate = saved_bit ? ssbs : 0U;
        memcpy(&expected.uc_sigmask, &mask, sizeof(uint64_t));
        FIXTURE_CHECK(llam_linux_arm64_async_redirect(&fresh, &scheduler, &mask) == 0);
        FIXTURE_CHECK(memcmp(&fresh, &expected, sizeof(fresh)) == 0);
    }
    /* SSBS does not authorize execution mode, DAIF, SS, BTYPE, DIT, TCO or
     * unknown high bits. Each still rejects without changing frame or output. */
    const uint64_t unsupported[] = {
        UINT64_C(1), UINT64_C(0x3c0), UINT64_C(0x100000), UINT64_C(0xc00),
        UINT64_C(0x1000000), UINT64_C(0x2000000), UINT64_C(0x100000000)
    };
    for (size_t i = 0U; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        fresh = saved;
        fresh.uc_mcontext.pstate |= unsupported[i];
        fixture_negative(&fresh);
    }
    puts("test_async_context_linux_arm64: synthetic four-way SSBS exact restore/redirect and unknown-state no-write passed");
}

static void fixture_parser_tests(void) {
    ucontext_t good = {0}, frame, expected;
    llam_linux_arm64_async_machine_t machine;
    uint64_t signal_bits = UINT64_C(0x800);
    good.uc_mcontext.pc = 0x1000U;
    good.uc_mcontext.sp = 0x2000U;
    good.uc_mcontext.pstate = UINT64_C(0xa0000000);
    fixture_record(&good, 0U, 0x46508001U, 528U);
    memcpy(&good.uc_sigmask, &signal_bits, sizeof(signal_bits));
    FIXTURE_CHECK(llam_linux_arm64_async_read(&good, EDOM, &machine) == 0);
    FIXTURE_CHECK(machine.entry_errno == EDOM && machine.signal_mask == signal_bits && machine.valid);

    /* A different fresh record position plus sentinel OS-owned metadata proves
     * restoration is selective, not an old-ucontext or old-record-layout copy. */
    memset(&frame, 0xb6, sizeof(frame));
    frame.uc_mcontext.pc = 0x3000U;
    frame.uc_mcontext.sp = 0x4000U;
    frame.uc_mcontext.pstate = 0U;
    fixture_record(&frame, 0U, 0x45535201U, 16U);
    fixture_record(&frame, 16U, 0x46508001U, 528U);
    fixture_record(&frame, 544U, 0U, 0U);
    expected = frame;
    memcpy(expected.uc_mcontext.regs, machine.x, sizeof(machine.x));
    expected.uc_mcontext.sp = machine.sp;
    expected.uc_mcontext.pc = machine.pc;
    expected.uc_mcontext.pstate = machine.pstate;
    memcpy(&expected.uc_sigmask, &machine.signal_mask, sizeof(machine.signal_mask));
    memcpy(expected.uc_mcontext.__reserved + 24U, &machine.fpsr, sizeof(machine.fpsr));
    memcpy(expected.uc_mcontext.__reserved + 28U, &machine.fpcr, sizeof(machine.fpcr));
    memcpy(expected.uc_mcontext.__reserved + 32U, machine.q, sizeof(machine.q));
    FIXTURE_CHECK(llam_linux_arm64_async_restore(&frame, &machine) == 0);
    FIXTURE_CHECK(memcmp(&frame, &expected, sizeof(frame)) == 0);

    {
        llam_ctx_t scheduler = {.sp = 0x3000U, .lr = 0x4000U, .simd_flags = 1U};
        llam_linux_arm64_async_machine_t invalid = machine;
        sigset_t mask;
        frame = good;
        sigemptyset(&mask);
        FIXTURE_CHECK(llam_linux_arm64_async_read(NULL, EDOM, &machine) == EINVAL);
        FIXTURE_CHECK(llam_linux_arm64_async_read(&frame, EDOM, NULL) == EINVAL);
        FIXTURE_CHECK(llam_linux_arm64_async_redirect(&frame, NULL, &mask) == EINVAL);
        FIXTURE_CHECK(llam_linux_arm64_async_redirect(&frame, &scheduler, NULL) == EINVAL);
        FIXTURE_CHECK(llam_linux_arm64_async_redirect(&frame, &scheduler, &mask) == ENOTSUP);
        invalid.valid = false;
        FIXTURE_CHECK(llam_linux_arm64_async_restore(&frame, &invalid) == EINVAL);
        invalid.valid = true;
        invalid.pstate |= 1U;
        FIXTURE_CHECK(llam_linux_arm64_async_restore(&frame, &invalid) == ENOTSUP);
        FIXTURE_CHECK(memcmp(&frame, &good, sizeof(frame)) == 0);
    }

    frame = good; frame.uc_mcontext.pstate |= 1U; fixture_negative(&frame);
    frame = good; frame.uc_mcontext.pc |= 2U; fixture_negative(&frame);
    frame = good; frame.uc_mcontext.sp |= 8U; fixture_negative(&frame);
    frame = good; fixture_record(&frame, 0U, 0U, 16U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 0U, 0x46508001U, 8U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 0U, 0x46508001U, 4096U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0x46508001U, 528U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0x45535201U, 16U);
    fixture_record(&frame, 544U, 0x45535201U, 16U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0x53564501U, 16U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0x45585401U, 32U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0x54504902U, 16U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 528U, 0xdeadbeefU, 16U); fixture_negative(&frame);
    frame = good; fixture_record(&frame, 0U, 0U, 0U); fixture_negative(&frame);

    /* Guard-backed malformed storage: duplicate records/no terminator must be
     * refused without escaping the inline record area. This narrow profile
     * rejects the second ESR; it does not claim a successful 4KiB record walk. */
    {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t bytes = (sizeof(ucontext_t) + page - 1U) / page * page;
        unsigned char *mapping = mmap(NULL, bytes + page, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ucontext_t *edge;
        FIXTURE_CHECK(mapping != MAP_FAILED && mprotect(mapping + bytes, page, PROT_NONE) == 0);
        edge = (ucontext_t *)(mapping + bytes - sizeof(ucontext_t));
        *edge = good;
        for (size_t offset = 0U; offset < sizeof(edge->uc_mcontext.__reserved); offset += 16U) {
            fixture_record(edge, offset, 0x45535201U, 16U);
        }
        fixture_negative(edge);
        FIXTURE_CHECK(munmap(mapping, bytes + page) == 0);
    }
}

int main(void) {
    fixture_t *f = calloc(1U, sizeof(*f));
    struct sigaction action = {0}, previous_action;
    pthread_t worker;
    size_t page = (size_t)sysconf(_SC_PAGESIZE), stack_bytes = 64U * 1024U;
    unsigned char *mapping;
    uint64_t deadline = fixture_now_ns() + UINT64_C(8000000000);
    unsigned completed = 0U;
    uint64_t expected = FIXTURE_ITERATIONS * (FIXTURE_ITERATIONS + 1U) / 2U;

    fixture_ssbs_tests();
    fixture_parser_tests();
    FIXTURE_CHECK(f != NULL && page != 0U);
    mapping = mmap(NULL, stack_bytes + 2U * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FIXTURE_CHECK(mapping != MAP_FAILED && mprotect(mapping + page, stack_bytes, PROT_READ | PROT_WRITE) == 0);
    f->stack_low = (uintptr_t)mapping + page;
    f->stack_high = f->stack_low + stack_bytes;
    FIXTURE_CHECK(pthread_sigmask(SIG_SETMASK, NULL, &f->scheduler_mask) == 0);
    sigdelset(&f->scheduler_mask, SIGUSR1);
    sigdelset(&f->scheduler_mask, SIGUSR2);
    f->task_mask = f->scheduler_mask;
    sigaddset(&f->task_mask, SIGUSR2);
    action.sa_sigaction = fixture_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    FIXTURE_CHECK(sigaction(SIGUSR1, &action, &previous_action) == 0);
    FIXTURE_CHECK(pthread_create(&worker, NULL, fixture_worker, f) == 0);
    while (atomic_load_explicit(&f->started, memory_order_acquire) == 0U) {
        FIXTURE_CHECK(fixture_now_ns() < deadline);
        fixture_pause();
    }
    while (atomic_load_explicit(&f->done, memory_order_acquire) == 0U) {
        unsigned phase = atomic_load_explicit(&f->phase, memory_order_acquire);
        FIXTURE_CHECK(fixture_now_ns() < deadline);
        if (phase == LLAM_ASYNC_BANK_FREE && completed < FIXTURE_CAPTURES) {
            int error = fixture_request(f, worker, FIXTURE_CAPTURE, deadline);
            FIXTURE_CHECK(error == 0 || error == EAGAIN);
        } else if (phase == LLAM_ASYNC_BANK_GATE) {
            FIXTURE_CHECK(fixture_request(f, worker, FIXTURE_RESTORE, deadline) == 0);
        } else if (phase == LLAM_ASYNC_BANK_RESTORING) {
            int error = fixture_request(f, worker, FIXTURE_WITNESS, deadline);
            FIXTURE_CHECK(error == 0 || error == EAGAIN);
        } else if (phase == LLAM_ASYNC_BANK_OBSERVED) {
            ++completed;
            /* The genuine witness and the handler's last response access both
             * precede reuse; this fixture owns the storage until thread join. */
            atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_FREE, memory_order_release);
        } else {
            fixture_pause();
        }
    }
    if (atomic_load_explicit(&f->phase, memory_order_acquire) == LLAM_ASYNC_BANK_OBSERVED) {
        ++completed;
        atomic_store_explicit(&f->phase, LLAM_ASYNC_BANK_FREE, memory_order_release);
    }
    FIXTURE_CHECK(atomic_load(&f->request) == 0U);
    atomic_store_explicit(&f->quit, 1U, memory_order_release);
    FIXTURE_CHECK(pthread_join(worker, NULL) == 0);
    if (f->policy_before.value >= 0 || f->policy_after.value >= 0) {
        FIXTURE_CHECK(f->policy_before.value >= 0 &&
                      f->policy_before.value == f->policy_after.value);
    }
    printf("target-M speculation policy: before=%d error=%d after=%d error=%d status=%s\n",
           f->policy_before.value, f->policy_before.error, f->policy_after.value,
           f->policy_after.error, f->policy_before.value >= 0 ? "unchanged" : "unavailable");
    FIXTURE_CHECK(sigaction(SIGUSR1, &previous_action, NULL) == 0);
    FIXTURE_CHECK(completed == FIXTURE_CAPTURES && atomic_load(&f->errors) == 0U &&
                  atomic_load(&f->captures) == completed && atomic_load(&f->restores) == completed &&
                  atomic_load(&f->witnesses) == completed && atomic_load(&f->peers) == completed &&
                  atomic_load(&f->phase) == LLAM_ASYNC_BANK_FREE && f->result == expected &&
                  llam_async_darwin_snapshot_valid(&f->snapshot, expected, f->stack_low, f->stack_high));
    printf("test_async_context_linux_arm64: PASS captures=%u restores=%u witnesses=%u peers=%u "
           "full_gpr=31 full_q=32 masks+errno=isolated parser_no_write=pass result=%llu\n",
           completed, atomic_load(&f->restores), atomic_load(&f->witnesses), atomic_load(&f->peers),
           (unsigned long long)f->result);
    FIXTURE_CHECK(munmap(mapping, stack_bytes + 2U * page) == 0);
    free(f);
    return 0;
}
#endif
