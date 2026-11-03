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
#include "async_context_darwin.h"

#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <sys/sysctl.h>

static pthread_once_t g_darwin_async_profile_once = PTHREAD_ONCE_INIT;
static bool g_darwin_async_cpu_has_no_sme;

static void llam_darwin_async_prepare_profile(void) {
    int feature = -1;
    size_t size = sizeof(feature);

    /* Hardware capability is immutable for this process. Query outside any
     * owned suspension; an absent/unknown sysctl does not prove SME absence. */
    g_darwin_async_cpu_has_no_sme =
        sysctlbyname("hw.optional.arm.FEAT_SME", &feature, &size, NULL, 0U) == 0 &&
        size == sizeof(feature) && feature == 0;
}

/* No allocation, runtime locks or target-dependent waits are allowed here.
 * A partially committed native context must never be resumed as a rollback. */
static LLAM_NORETURN void llam_darwin_async_uncertain(void) {
    _Exit(125);
}

static int llam_darwin_async_error(int error) {
    errno = error;
    return -1;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_darwin_async_test_failure = ATOMIC_VAR_INIT(0U);

void llam_darwin_async_test_fail(unsigned point) {
    atomic_store_explicit(&g_darwin_async_test_failure, point, memory_order_release);
}

unsigned llam_darwin_async_test_pending_failure(void) {
    return atomic_load_explicit(&g_darwin_async_test_failure, memory_order_acquire);
}

static bool llam_darwin_async_take_failure(unsigned point) {
    return atomic_compare_exchange_strong_explicit(&g_darwin_async_test_failure,
        &point, 0U, memory_order_acq_rel, memory_order_acquire);
}
#else
#define llam_darwin_async_take_failure(point) false
#endif

static kern_return_t llam_darwin_async_set_state(
    thread_t thread, thread_state_flavor_t flavor,
    thread_state_t state, mach_msg_type_number_t count) {
    if (llam_darwin_async_take_failure(flavor == ARM_NEON_STATE64
            ? LLAM_DARWIN_ASYNC_TEST_FAIL_NEON
            : LLAM_DARWIN_ASYNC_TEST_FAIL_GENERAL)) {
        return KERN_FAILURE;
    }
    return thread_set_state(thread, flavor, state, count);
}

int llam_darwin_async_suspend(thread_t thread, llam_darwin_async_transaction_t *tx) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t rc;

    if (tx == NULL || tx->suspended || thread == MACH_PORT_NULL ||
        thread == pthread_mach_thread_np(pthread_self())) {
        return llam_darwin_async_error(EINVAL);
    }
    if (pthread_once(&g_darwin_async_profile_once, llam_darwin_async_prepare_profile) != 0) {
        return llam_darwin_async_error(ENOTSUP);
    }
    *tx = (llam_darwin_async_transaction_t){.thread = thread};
    rc = thread_suspend(thread);
    if (rc != KERN_SUCCESS) {
        return llam_darwin_async_error(ESRCH);
    }
    tx->suspended = true;
    rc = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    if (rc != KERN_SUCCESS || count != THREAD_BASIC_INFO_COUNT ||
        info.suspend_count != 1) {
        /* Do not consume a debugger/peer's pre-existing suspension increment. */
        llam_darwin_async_resume(tx);
        return llam_darwin_async_error(rc == KERN_SUCCESS ? EBUSY : EIO);
    }
    return 0;
}

uintptr_t llam_darwin_async_pc(const llam_darwin_async_machine_t *machine) {
    return machine != NULL && machine->valid
        ? (uintptr_t)arm_thread_state64_get_pc(machine->general) : 0U;
}

uintptr_t llam_darwin_async_sp(const llam_darwin_async_machine_t *machine) {
    return machine != NULL && machine->valid
        ? (uintptr_t)arm_thread_state64_get_sp(machine->general) : 0U;
}

static bool llam_darwin_async_baseline_state(thread_t thread) {
    if (llam_darwin_async_take_failure(LLAM_DARWIN_ASYNC_TEST_FAIL_SME_QUERY)) {
        return false;
    }
#ifdef ARM_SME_STATE
    arm_sme_state_t sme = {0};
    mach_msg_type_number_t count = ARM_SME_STATE_COUNT;
    kern_return_t rc = thread_get_state(thread, ARM_SME_STATE,
                                        (thread_state_t)&sme, &count);

    /* XNU documents INVALID_ARGUMENT for a CPU without SME. All other unknown
     * states fail closed; live streaming/ZA or ABI state is not truncated. */
    if (rc == KERN_INVALID_ARGUMENT) {
        return g_darwin_async_cpu_has_no_sme;
    }
    return rc == KERN_SUCCESS && count == ARM_SME_STATE_COUNT &&
           sme.__svcr == 0U && sme.__tpidr2_el0 == 0U;
#else
    (void)thread;
    return false; /* An older SDK cannot qualify newer machine state. */
#endif
}

int llam_darwin_async_read(llam_darwin_async_transaction_t *tx,
                            llam_darwin_async_machine_t *out) {
    if (tx == NULL || !tx->suspended || out == NULL) {
        return llam_darwin_async_error(EINVAL);
    }
    out->valid = false;
    out->origin_thread = MACH_PORT_NULL;
#if defined(__arm64e__) || __DARWIN_OPAQUE_ARM_THREAD_STATE64
    /* No qualification yet for the additional authenticated native ABI. */
    return llam_darwin_async_error(ENOTSUP);
#else
    mach_msg_type_number_t general_count = ARM_THREAD_STATE64_COUNT;
    mach_msg_type_number_t neon_count = ARM_NEON_STATE64_COUNT;

    if (llam_darwin_async_take_failure(LLAM_DARWIN_ASYNC_TEST_FAIL_READ) ||
        !llam_darwin_async_baseline_state(tx->thread) ||
        thread_get_state(tx->thread, ARM_THREAD_STATE64,
                         (thread_state_t)&out->general, &general_count) != KERN_SUCCESS ||
        general_count != ARM_THREAD_STATE64_COUNT ||
        thread_get_state(tx->thread, ARM_NEON_STATE64,
                         (thread_state_t)&out->neon, &neon_count) != KERN_SUCCESS ||
        neon_count != ARM_NEON_STATE64_COUNT) {
        return llam_darwin_async_error(ENOTSUP);
    }
    /* Only EL0 baseline NZCV is admitted. Preserve, never strip, pointer state. */
    if ((out->general.__cpsr & ~UINT32_C(0xf0000000)) != 0U ||
        (out->general.__pad & ~UINT32_C(1)) != 0U) {
        return llam_darwin_async_error(ENOTSUP);
    }
    out->origin_thread = tx->thread;
    out->valid = true;
    return 0;
#endif
}

int llam_darwin_async_redirect(llam_darwin_async_transaction_t *tx,
                                const llam_darwin_async_machine_t *captured,
                                const llam_ctx_t *scheduler,
                                uintptr_t pc_begin, uintptr_t pc_end,
                                uintptr_t stack_low, uintptr_t stack_high) {
    arm_thread_state64_t redirect;
    uintptr_t pc = llam_darwin_async_pc(captured);
    uintptr_t sp = llam_darwin_async_sp(captured);

    if (tx == NULL || !tx->suspended || tx->mutated || scheduler == NULL ||
        captured == NULL || !captured->valid || pc_begin == 0U || pc_end <= pc_begin ||
        captured->origin_thread != tx->thread ||
        pc < pc_begin || pc >= pc_end || stack_low >= stack_high ||
        sp < stack_low || sp >= stack_high || (sp & 15U) != 0U ||
        scheduler->sp == 0U || scheduler->lr == 0U) {
        return llam_darwin_async_error(EINVAL);
    }
    redirect = captured->general;
    arm_thread_state64_set_pc_fptr(redirect, llam_darwin_async_restore_scheduler);
    redirect.__x[0] = (uint64_t)(uintptr_t)scheduler;
    /* No store on the interrupted G stack. The bridge reloads the live M ABI
     * state before returning into the exact scheduler continuation. */
    if (llam_darwin_async_set_state(tx->thread, ARM_THREAD_STATE64,
                         (thread_state_t)&redirect, ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS) {
        llam_darwin_async_uncertain();
    }
    tx->mutated = true;
    return 0;
}

int llam_darwin_async_write(llam_darwin_async_transaction_t *tx,
                             const llam_darwin_async_machine_t *captured) {
    if (tx == NULL || !tx->suspended || tx->mutated ||
        captured == NULL || !captured->valid || captured->origin_thread != tx->thread) {
        return llam_darwin_async_error(EINVAL);
    }
    /* Verify the destination is still in the same qualified baseline profile. */
    if (!llam_darwin_async_baseline_state(tx->thread)) {
        return llam_darwin_async_error(ENOTSUP);
    }
    if (llam_darwin_async_set_state(tx->thread, ARM_NEON_STATE64,
                         (thread_state_t)&captured->neon, ARM_NEON_STATE64_COUNT) != KERN_SUCCESS) {
        llam_darwin_async_uncertain();
    }
    tx->mutated = true;
    if (llam_darwin_async_set_state(tx->thread, ARM_THREAD_STATE64,
                         (thread_state_t)&captured->general, ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS) {
        llam_darwin_async_uncertain();
    }
    return 0;
}

void llam_darwin_async_resume(llam_darwin_async_transaction_t *tx) {
    if (tx == NULL || !tx->suspended ||
        llam_darwin_async_take_failure(LLAM_DARWIN_ASYNC_TEST_FAIL_RESUME) ||
        thread_resume(tx->thread) != KERN_SUCCESS) {
        llam_darwin_async_uncertain();
    }
    tx->suspended = false;
}

/* The new protocol owns this send right for one live run. The mechanism API
 * borrows it only while its counted binding operation excludes close. */
typedef struct llam_darwin_runtime_binding {
    thread_t thread;
    llam_darwin_async_transaction_t transaction;
    sigset_t previous_mask;
    bool mask_saved;
    bool restore_mask;
} llam_darwin_runtime_binding_t;

static int llam_darwin_runtime_bind(void *opaque) {
    llam_darwin_runtime_binding_t *binding = opaque;
#if defined(__arm64e__) || __DARWIN_OPAQUE_ARM_THREAD_STATE64
    (void)binding;
    errno = ENOTSUP;
    return -1;
#else
    if (binding->thread != MACH_PORT_NULL) {
        errno = EBUSY;
        return -1;
    }
    if (llam_async_shared_block_native_signals(g_llam_tls_worker, &binding->previous_mask) != 0) return -1;
    binding->mask_saved = true;
    binding->restore_mask = g_llam_tls_worker == NULL ||
        g_llam_tls_worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE;
    binding->thread = mach_thread_self();
    if (binding->thread == MACH_PORT_NULL) {
        (void)pthread_sigmask(SIG_SETMASK, &binding->previous_mask, NULL);
        binding->mask_saved = false;
        errno = EIO;
        return -1;
    }
    binding->transaction = (llam_darwin_async_transaction_t){0};
    return 0;
#endif
}

static void llam_darwin_runtime_close(void *opaque) {
    llam_darwin_runtime_binding_t *binding = opaque;
    if (binding->transaction.suspended || binding->thread == MACH_PORT_NULL ||
        mach_port_deallocate(mach_task_self(), binding->thread) != KERN_SUCCESS) {
        llam_darwin_async_uncertain();
    }
    if (binding->mask_saved && binding->restore_mask &&
        pthread_sigmask(SIG_SETMASK, &binding->previous_mask, NULL) != 0) llam_darwin_async_uncertain();
    memset(binding, 0, sizeof(*binding));
}

static int llam_darwin_runtime_suspend(void *opaque) {
    llam_darwin_runtime_binding_t *binding = opaque;
    return llam_darwin_async_suspend(binding->thread, &binding->transaction);
}

static int llam_darwin_runtime_read(void *opaque, void *context) {
    llam_darwin_runtime_binding_t *binding = opaque;
    return llam_darwin_async_read(&binding->transaction, context);
}

static int llam_darwin_runtime_observe_sp(void *opaque, uintptr_t *sp) {
    llam_darwin_runtime_binding_t *binding = opaque;
    arm_thread_state64_t general;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (binding == NULL || sp == NULL || !binding->transaction.suspended || binding->transaction.mutated)
        return llam_darwin_async_error(EINVAL);
    if (llam_darwin_async_take_failure(LLAM_DARWIN_ASYNC_TEST_FAIL_OBSERVE) ||
        thread_get_state(binding->thread, ARM_THREAD_STATE64, (thread_state_t)&general, &count) != KERN_SUCCESS ||
        count != ARM_THREAD_STATE64_COUNT) return llam_darwin_async_error(ENOTSUP);
    /* Current code may be a native call after restored G progress. Its SIMD,
     * flags and signed LR do not authorize another capture and are not saved.
     * An unrecognized SP representation simply fails the caller's G bounds. */
    *sp = (uintptr_t)arm_thread_state64_get_sp(general);
    return 0;
}

static int llam_darwin_runtime_redirect(void *opaque, const void *context,
                                        const llam_ctx_t *scheduler,
                                        uintptr_t begin, uintptr_t end,
                                        uintptr_t low, uintptr_t high) {
    llam_darwin_runtime_binding_t *binding = opaque;
    return llam_darwin_async_redirect(&binding->transaction, context, scheduler,
                                       begin, end, low, high);
}

static int llam_darwin_runtime_restore(void *opaque, const void *context) {
    llam_darwin_runtime_binding_t *binding = opaque;
    return llam_darwin_async_write(&binding->transaction, context);
}

static void llam_darwin_runtime_resume(void *opaque) {
    llam_darwin_runtime_binding_t *binding = opaque;
    llam_darwin_async_resume(&binding->transaction);
}

static uintptr_t llam_darwin_runtime_pc(const void *context) {
    return llam_darwin_async_pc(context);
}

static uintptr_t llam_darwin_runtime_sp(const void *context) {
    return llam_darwin_async_sp(context);
}

static bool llam_darwin_runtime_at_gate(const void *context) {
    uintptr_t pc = llam_darwin_async_pc(context);
    return pc >= (uintptr_t)llam_darwin_async_gate_begin &&
           pc < (uintptr_t)llam_darwin_async_gate_end;
}

const llam_async_native_ops_t g_llam_darwin_async_ops = {
    .binding_size = sizeof(llam_darwin_runtime_binding_t),
    .context_size = sizeof(llam_darwin_async_machine_t),
    .bind_current = llam_darwin_runtime_bind,
    .close = llam_darwin_runtime_close,
    .suspend = llam_darwin_runtime_suspend,
    .read = llam_darwin_runtime_read,
    .redirect = llam_darwin_runtime_redirect,
    .restore = llam_darwin_runtime_restore,
    .resume = llam_darwin_runtime_resume,
    .pc = llam_darwin_runtime_pc,
    .sp = llam_darwin_runtime_sp,
    .at_gate = llam_darwin_runtime_at_gate,
    .save_gate = llam_darwin_async_save_gate,
    .observe_sp = llam_darwin_runtime_observe_sp
};

#endif
