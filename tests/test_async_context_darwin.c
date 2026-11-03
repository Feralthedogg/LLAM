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

#include "runtime_internal.h"

#include <stdio.h>
#include <inttypes.h>

#if !LLAM_PLATFORM_DARWIN || !LLAM_ARCH_AARCH64 || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) {
    puts("test_async_context_darwin: skipped (native arm64 mechanism disabled)");
    return 77;
}
#else
#include "async_context_darwin.h"
#include <sys/sysctl.h>
#include <sys/wait.h>

#define FIXTURE_ITERATIONS UINT64_C(300000000)
#define FIXTURE_CAPTURES 8U

#include "fixtures/async_darwin_leaf.h"
typedef llam_async_darwin_snapshot_t fixture_snapshot_t;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "gate publication must be lock-free");

typedef struct fixture {
    llam_ctx_t scheduler;
    llam_ctx_t fiber;
    atomic_uint port;
    atomic_uint phase;
    atomic_uint started;
    atomic_uint done;
    atomic_uint peers;
    atomic_uint errors;
    fixture_snapshot_t snapshot;
    sigset_t mask;
    uintptr_t stack_low;
    uintptr_t stack_high;
    uint64_t result;
    uint64_t scheduler_fpcr;
    uint64_t scheduler_fpsr;
} fixture_t;

extern void llam_async_darwin_fixture_start(void);

static uint64_t fixture_now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void fixture_pause(void) {
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000L};
    (void)nanosleep(&delay, NULL);
}

static void fixture_fp(uint64_t *control, uint64_t *status) {
    __asm__ volatile("mrs %0, fpcr" : "=r"(*control));
    __asm__ volatile("mrs %0, fpsr" : "=r"(*status));
}

static bool fixture_same_mask(const sigset_t *a, const sigset_t *b) {
    for (int i = 1; i < NSIG; ++i) {
        if (sigismember(a, i) != sigismember(b, i)) return false;
    }
    return true;
}

void llam_async_darwin_fixture_body(void *opaque) {
    fixture_t *f = opaque;
    atomic_store_explicit(&f->started, 1U, memory_order_release);
    errno = EDOM;
    f->result = llam_async_darwin_leaf(FIXTURE_ITERATIONS, &f->snapshot);
    if (errno != EDOM) atomic_fetch_add_explicit(&f->errors, 1U, memory_order_relaxed);
    atomic_store_explicit(&f->done, 1U, memory_order_release);
    llam_ctx_switch(&f->fiber, &f->scheduler);
    abort();
}

static void *fixture_worker(void *opaque) {
    fixture_t *f = opaque;
    sigset_t original;
    uint64_t control;
    uint64_t status;
    thread_t port = mach_thread_self();

    if (pthread_sigmask(SIG_SETMASK, NULL, &original) != 0) abort();
    f->mask = original;
    sigaddset(&f->mask, SIGUSR2);
    if (pthread_sigmask(SIG_SETMASK, &f->mask, NULL) != 0) abort();
    fixture_fp(&f->scheduler_fpcr, &f->scheduler_fpsr);
    atomic_store_explicit(&f->port, port, memory_order_release);
    f->fiber.sp = f->stack_high;
    f->fiber.lr = (uint64_t)(uintptr_t)llam_async_darwin_fixture_start;
    f->fiber.x19 = (uint64_t)(uintptr_t)f;
    errno = EDOM;
    llam_ctx_switch(&f->scheduler, &f->fiber);
    for (;;) {
        sigset_t mask;
        fixture_fp(&control, &status);
        if (control != f->scheduler_fpcr || status != f->scheduler_fpsr ||
            errno != EDOM || pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
            !fixture_same_mask(&mask, &f->mask)) {
            atomic_fetch_add_explicit(&f->errors, 1U, memory_order_relaxed);
        }
        if (atomic_load_explicit(&f->done, memory_order_acquire) != 0U) break;
        /* A native-continuation witness and independent peer work, not a call
         * from the interrupted leaf or from any signal handler. */
        atomic_fetch_add_explicit(&f->peers, 1U, memory_order_release);
        errno = EDOM;
        llam_darwin_async_save_gate(&f->scheduler, &f->phase);
    }
    if (pthread_sigmask(SIG_SETMASK, &original, NULL) != 0) abort();
    return NULL; /* The controller retains and releases the published send right. */
}

static bool fixture_machine_ok(const llam_darwin_async_machine_t *m, uint32_t native_policy) {
    if (!m->valid || m->general.__cpsr != (UINT32_C(0xa0000000) | native_policy) ||
        m->neon.__fpcr != UINT32_C(0x800000) ||
        m->neon.__fpsr != UINT32_C(0x8000000)) return false;
    for (unsigned i = 2U; i < 29U; ++i) {
        if (i != 18U && m->general.__x[i] != UINT64_C(0x5100) + i) return false;
    }
    for (unsigned i = 0U; i < 32U; ++i) {
        const unsigned char *q = (const unsigned char *)&m->neon.__v[i];
        for (unsigned j = 0U; j < 16U; ++j) {
            if (q[j] != (unsigned char)(i + 1U)) return false;
        }
    }
    return true;
}

static void fixture_report_read_refusal(llam_darwin_async_transaction_t *tx) {
    arm_thread_state64_t general = {0};
    mach_msg_type_number_t general_count = ARM_THREAD_STATE64_COUNT;
    kern_return_t general_rc = thread_get_state(tx->thread, ARM_THREAD_STATE64,
        (thread_state_t)&general, &general_count);
    int sme_rc = -1;
    unsigned sme_count = 0U;
    uint64_t svcr = 0U, tpidr2 = 0U;
#ifdef ARM_SME_STATE
    arm_sme_state_t sme = {0};
    mach_msg_type_number_t count = ARM_SME_STATE_COUNT;
    sme_rc = thread_get_state(tx->thread, ARM_SME_STATE, (thread_state_t)&sme, &count);
    sme_count = count;
    svcr = sme.__svcr;
    tpidr2 = sme.__tpidr2_el0;
#endif
    /* Resume before sysctl or stdio can acquire host-library locks. */
    llam_darwin_async_resume(tx);
    int feature = -1;
    size_t size = sizeof(feature);
    int feature_rc = sysctlbyname("hw.optional.arm.FEAT_SME", &feature, &size, NULL, 0U);
    fprintf(stderr, "Darwin native refusal: opaque=%d general_rc=%d count=%u "
        "cpsr=%#" PRIx32 " SME_rc=%d count=%u svcr=%#" PRIx64
        " tpidr2=%#" PRIx64 " feature_rc=%d feature=%d\n",
        __DARWIN_OPAQUE_ARM_THREAD_STATE64, general_rc, general_count,
        (uint32_t)general.__cpsr, sme_rc, sme_count, svcr, tpidr2, feature_rc, feature);
}

static int fixture_run(unsigned fail_point) {
    fixture_t f = {0};
    pthread_t worker;
    llam_darwin_async_transaction_t tx = {0};
    llam_darwin_async_machine_t captured = {0};
    llam_darwin_async_machine_t gate = {0};
    uint64_t deadline = fixture_now_ns() + UINT64_C(5000000000);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t bytes = 64U * 1024U;
    void *mapping = mmap(NULL, bytes + 2U * page, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned captures = 0U;
    bool negatives = false;
    bool read_refused = false;
    bool foreign_suspend_refused;
    int error = 0;
    thread_t port;
    uint32_t native_policy;

    if (mapping == MAP_FAILED) return 1;
    f.stack_low = (uintptr_t)mapping + page;
    f.stack_high = f.stack_low + bytes;
    if (mprotect((void *)f.stack_low, bytes, PROT_READ | PROT_WRITE) != 0 ||
        pthread_create(&worker, NULL, fixture_worker, &f) != 0) {
        munmap(mapping, bytes + 2U * page);
        return 1;
    }
    while (atomic_load_explicit(&f.port, memory_order_acquire) == 0U ||
           atomic_load_explicit(&f.started, memory_order_acquire) == 0U) {
        if (fixture_now_ns() > deadline) abort();
        fixture_pause();
    }
    port = atomic_load_explicit(&f.port, memory_order_acquire);
    {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;

        if (thread_suspend(port) != KERN_SUCCESS) abort();
        arm_thread_state64_t general = {0};
        mach_msg_type_number_t general_count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&general,
                             &general_count) != KERN_SUCCESS || general_count != ARM_THREAD_STATE64_COUNT) abort();
        native_policy = general.__cpsr & ~LLAM_DARWIN_ASYNC_NZCV_MASK;
        foreign_suspend_refused = llam_darwin_async_suspend(port, &tx) == -1 &&
                                  errno == EBUSY && !tx.suspended;
        foreign_suspend_refused = foreign_suspend_refused &&
            thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS &&
            info.suspend_count == 1;
        if (thread_resume(port) != KERN_SUCCESS) abort();
    }
    while (captures < FIXTURE_CAPTURES &&
           atomic_load_explicit(&f.done, memory_order_acquire) == 0U) {
        uintptr_t pc;
        if (fixture_now_ns() > deadline) abort();
        if (llam_darwin_async_suspend(port, &tx) != 0) { error = errno; break; }
        if (!read_refused) {
            llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_READ);
            read_refused = llam_darwin_async_read(&tx, &captured) == -1 &&
                           errno == ENOTSUP && !captured.valid && !tx.mutated;
            if (!read_refused) abort();
        }
        if (llam_darwin_async_read(&tx, &captured) != 0) {
            error = errno;
            fixture_report_read_refusal(&tx);
            break;
        }
        pc = llam_darwin_async_pc(&captured);
        if (pc < (uintptr_t)llam_async_darwin_leaf_begin ||
            pc >= (uintptr_t)llam_async_darwin_leaf_end) {
            llam_darwin_async_resume(&tx);
            fixture_pause();
            continue;
        }
        if (!fixture_machine_ok(&captured, native_policy)) {
            error = EILSEQ;
            llam_darwin_async_resume(&tx);
            break;
        }
        if (!negatives) {
            llam_darwin_async_machine_t wrong_origin = captured;
            llam_darwin_async_machine_t refused = {0};
            bool rejected =
                llam_darwin_async_redirect(&tx, &captured, &f.scheduler,
                    pc + 4U, pc + 8U, f.stack_low, f.stack_high) == -1 && !tx.mutated;
            rejected = rejected &&
                llam_darwin_async_redirect(&tx, &captured, &f.scheduler,
                    (uintptr_t)llam_async_darwin_leaf_begin,
                    (uintptr_t)llam_async_darwin_leaf_end, 1U, 2U) == -1 && !tx.mutated;
            wrong_origin.origin_thread = MACH_PORT_NULL;
            rejected = rejected &&
                llam_darwin_async_write(&tx, &wrong_origin) == -1 && !tx.mutated;
            llam_darwin_async_machine_t wrong_policy = captured;
            wrong_policy.general.__cpsr ^= LLAM_DARWIN_ASYNC_SSBS_MASK;
            rejected = rejected && llam_darwin_async_write(&tx, &wrong_policy) == -1 &&
                       errno == ENOTSUP && !tx.mutated;
            llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_SME_QUERY);
            rejected = rejected &&
                llam_darwin_async_read(&tx, &refused) == -1 && errno == ENOTSUP &&
                !refused.valid && !tx.mutated;
            if (!rejected) { error = EINVAL; llam_darwin_async_resume(&tx); break; }
            negatives = true;
        }
        atomic_store_explicit(&f.phase, LLAM_ASYNC_BANK_CAPTURED, memory_order_release);
        if (llam_darwin_async_redirect(&tx, &captured, &f.scheduler,
                (uintptr_t)llam_async_darwin_leaf_begin,
                (uintptr_t)llam_async_darwin_leaf_end,
                f.stack_low, f.stack_high) != 0) abort();
        llam_darwin_async_resume(&tx);
        while (atomic_load_explicit(&f.phase, memory_order_acquire) != LLAM_ASYNC_BANK_GATE) {
            if (fixture_now_ns() > deadline) abort();
            fixture_pause();
        }
        if (llam_darwin_async_suspend(port, &tx) != 0 ||
            llam_darwin_async_read(&tx, &gate) != 0) abort();
        pc = llam_darwin_async_pc(&gate);
        if (pc < (uintptr_t)llam_darwin_async_gate_begin ||
            pc >= (uintptr_t)llam_darwin_async_gate_end) abort();
        atomic_store_explicit(&f.phase, LLAM_ASYNC_BANK_RESTORING, memory_order_release);
        if (fail_point != LLAM_DARWIN_ASYNC_TEST_FAIL_RESUME) {
            llam_darwin_async_test_fail(fail_point);
        }
        if (llam_darwin_async_write(&tx, &captured) != 0) abort();
        arm_thread_state64_t restored = {0};
        mach_msg_type_number_t restored_count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&restored,
                             &restored_count) != KERN_SUCCESS || restored_count != ARM_THREAD_STATE64_COUNT ||
            restored.__cpsr != captured.general.__cpsr) abort();
        if (fail_point == LLAM_DARWIN_ASYNC_TEST_FAIL_RESUME) {
            llam_darwin_async_test_fail(fail_point);
        }
        llam_darwin_async_resume(&tx);
        ++captures;
    }
    if (pthread_join(worker, NULL) != 0) abort();
    /* A dead native target is refused without owning an increment. */
    bool dead_refused = llam_darwin_async_suspend(port, &tx) == -1 && !tx.suspended;
    if (mach_port_deallocate(mach_task_self(), port) != KERN_SUCCESS) abort();
    munmap(mapping, bytes + 2U * page);
    if (error != 0 || captures != FIXTURE_CAPTURES || !negatives || !dead_refused ||
        !read_refused || !foreign_suspend_refused ||
        atomic_load(&f.peers) != captures || atomic_load(&f.errors) != 0U ||
        f.result != FIXTURE_ITERATIONS * (FIXTURE_ITERATIONS + 1U) / 2U ||
        !llam_async_darwin_snapshot_valid(&f.snapshot, f.result, f.stack_low, f.stack_high)) {
        fprintf(stderr, "Darwin mechanism FAIL: errno=%d capture=%u peer=%u errors=%u result=%" PRIu64 "\n",
                error, captures, atomic_load(&f.peers), atomic_load(&f.errors), f.result);
        return 1;
    }
    printf("test_async_context_darwin: %u native capture/restore/return witnesses; full GPR/NEON/flags/FP/mask/errno and finite result passed\n", captures);
    return 0;
}

int main(int argc, char **argv) {
    unsigned point;
    pid_t child;
    int status;

    if (argc == 1) return fixture_run(0U);
    if (argc != 2) return 2;
    if (strcmp(argv[1], "--fail-neon") == 0) {
        point = LLAM_DARWIN_ASYNC_TEST_FAIL_NEON;
    } else if (strcmp(argv[1], "--fail-general") == 0) {
        point = LLAM_DARWIN_ASYNC_TEST_FAIL_GENERAL;
    } else if (strcmp(argv[1], "--fail-resume") == 0) {
        point = LLAM_DARWIN_ASYNC_TEST_FAIL_RESUME;
    } else {
        return 2;
    }
    /* Fork before creating fixture threads. Committed-mutation failures are
     * process-terminal and must not be tested inside the caller's process. */
    child = fork();
    if (child < 0) return 1;
    if (child == 0) _Exit(fixture_run(point));
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 125) return 1;
    printf("test_async_context_darwin: injected native failure %u terminated child with125\n", point);
    return 0;
}
#endif
