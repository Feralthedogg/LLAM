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
#include "async_preempt_abi.h"

#include <stdio.h>

#if !defined(__linux__) || !defined(__x86_64__) || \
    !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)

int main(void) {
    puts("test_async_preempt_linux_x86_64: skipped (experimental Linux x86-64 backend disabled or unsupported)");
    return 77;
}

#else

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <cpuid.h>
#if defined(LLAM_TEST_WRAP_SIGACTION)
#include "async_signal_registry.h"
#include <sys/wait.h>
#endif

extern unsigned int llam_async_safe_leaf(atomic_uint *release);
extern void llam_async_unsafe_leaf(atomic_uint *release);
extern char llam_async_safe_leaf_begin[];
extern char llam_async_safe_leaf_end[];
extern int llam_async_avx_leaf(atomic_uint *release);
extern char llam_async_avx_leaf_begin[];
extern char llam_async_avx_leaf_end[];

/* Private experimental entry points implemented by the guarded backend. */
int llam_async_preempt_register_safe_range(llam_runtime_t *runtime,
                                           uintptr_t begin,
                                           uintptr_t end);
int llam_async_preempt_enable_runtime(llam_runtime_t *runtime);

static int async_test_legacy_fpu_frame(void) {
    _Alignas(64) unsigned char source[516] = {0};
    _Alignas(64) unsigned char destination[580] = {0};
    ucontext_t context = {0};
    llam_async_preempt_state_t state = {0};
    uint32_t magic = UINT32_C(0x46505853);
    uint32_t extended_size = 516U;
    uint32_t xstate_size = 512U;
    uint64_t features = 0U;

    memcpy(source + 464U, &magic, sizeof(magic));
    memcpy(source + 468U, &extended_size, sizeof(extended_size));
    memcpy(source + 472U, &features, sizeof(features));
    memcpy(source + 480U, &xstate_size, sizeof(xstate_size));
    state.xstate_area = destination;
    state.xstate_capacity = 512U;
    state.xsave_mask = UINT64_C(3);
    context.uc_flags = LLAM_ASYNC_PREEMPT_UC_SIGCONTEXT_SS |
                       LLAM_ASYNC_PREEMPT_UC_STRICT_RESTORE_SS;
    context.uc_mcontext.fpregs = (fpregset_t)source;
    if (!llam_async_preempt_test_capture_xstate(&state, &context) ||
        state.xstate_format != LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_LEGACY ||
        state.captured_xstate_size != 512U || memcmp(source, destination, 512U) != 0) {
        return 1;
    }
    {
        long page = sysconf(_SC_PAGESIZE);
        void *mapping;
        unsigned char *guarded;
        bool valid;
        if (page < 512L || (uint64_t)page > SIZE_MAX / 2U) return 1;
        mapping = mmap(NULL, 2U * (size_t)page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) return 1;
        if (mprotect(mapping, (size_t)page, PROT_READ | PROT_WRITE) != 0) {
            munmap(mapping, 2U * (size_t)page);
            return 1;
        }
        guarded = (unsigned char *)mapping + (size_t)page - 512U;
        memcpy(guarded, source, 512U);
        context.uc_mcontext.fpregs = (fpregset_t)guarded;
        valid = llam_async_preempt_test_capture_xstate(&state, &context);
        context.uc_mcontext.fpregs = (fpregset_t)source;
        munmap(mapping, 2U * (size_t)page);
        if (!valid || state.captured_xstate_size != 512U) return 1;
    }
    context.uc_flags |= LLAM_ASYNC_PREEMPT_UC_FP_XSTATE;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    context.uc_flags &= ~(unsigned long)LLAM_ASYNC_PREEMPT_UC_FP_XSTATE;
    context.uc_flags |= 8UL;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    context.uc_flags &= ~8UL;
    context.uc_flags &= ~(unsigned long)LLAM_ASYNC_PREEMPT_UC_SIGCONTEXT_SS;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    context.uc_flags |= LLAM_ASYNC_PREEMPT_UC_SIGCONTEXT_SS;
    features = UINT64_C(4);
    memcpy(source + 472U, &features, sizeof(features));
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    features = 0U;
    memcpy(source + 472U, &features, sizeof(features));
    extended_size = 515U;
    memcpy(source + 468U, &extended_size, sizeof(extended_size));
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    extended_size = 516U;
    memcpy(source + 468U, &extended_size, sizeof(extended_size));
    state.xstate_capacity = 576U;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    puts("test_async_preempt_linux_x86_64: native non-XSAVE FXSAVE format and malformed-frame refusal passed");
    return 0;
}

static int async_test_pkru_frame(void) {
    _Alignas(64) unsigned char source[644] = {0}, destination[644] = {0}, before[644];
    ucontext_t context = {0};
    llam_async_preempt_state_t state = {0};
    uint32_t magic = UINT32_C(0x46505853), trailer = UINT32_C(0x46505845);
    uint32_t bytes = 584U, extended = 588U, pkru = UINT32_C(0x55555554);
    uint64_t features = UINT64_C(515), active = features;
    memcpy(source + 464U, &magic, 4U);
    memcpy(source + 468U, &extended, 4U);
    memcpy(source + 472U, &features, 8U);
    memcpy(source + 480U, &bytes, 4U);
    memcpy(source + 512U, &active, 8U);
    memcpy(source + 576U, &pkru, 4U);
    memcpy(source + 584U, &trailer, 4U);
    state.xstate_area = destination;
    state.xstate_capacity = 640U;
    state.xsave_mask = features;
    state.pkru_offset = 576U;
    state.pkru_baseline = pkru;
    context.uc_mcontext.fpregs = (fpregset_t)source;
    if (!llam_async_preempt_test_capture_xstate(&state, &context) ||
        state.captured_xstate_size != extended || memcmp(source, destination, extended) != 0) return 1;
    memcpy(before, destination, sizeof(before));
    pkru ^= UINT32_C(0x40000000);
    memcpy(source + 576U, &pkru, 4U);
    if (llam_async_preempt_test_capture_xstate(&state, &context) ||
        memcmp(before, destination, sizeof(before)) != 0) return 1;
    active = 3U;
    memcpy(source + 512U, &active, 8U);
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    state.pkru_baseline = 0U;
    if (!llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    active = features;
    memcpy(source + 512U, &active, 8U);
    state.pkru_baseline = pkru;
    source[580] = 1U;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    source[580] = 0U;
    state.pkru_offset = 0U;
    if (llam_async_preempt_test_capture_xstate(&state, &context)) return 1;
    puts("test_async_preempt_linux_x86_64: PKRU baseline preservation and changed-policy refusal passed");
    return 0;
}

static int async_test_frame_refusal(void) {
    ucontext_t context = {0};

    if (llam_async_preempt_test_frame_supported(NULL) ||
        llam_async_preempt_test_frame_supported(&context)) {
        return 1;
    }
    context.uc_mcontext.gregs[REG_CSGSFS] = (greg_t)UINT64_C(0x33);
    if (llam_async_preempt_test_frame_supported(&context)) {
        return 1;
    }
    context.uc_mcontext.gregs[REG_CSGSFS] =
        (greg_t)((UINT64_C(0x2b) << 48U) | UINT64_C(0x33));
    if (!llam_async_preempt_test_frame_supported(&context)) {
        return 1;
    }
    puts("test_async_preempt_linux_x86_64: missing signal selectors refused (contract check passed)");
    return async_test_legacy_fpu_frame() || async_test_pkru_frame();
}

static void async_allocation_task(void *opaque) {
    atomic_uint *ran = opaque;

    atomic_fetch_add_explicit(ran, 1U, memory_order_release);
}

static int async_test_preparation_failures(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task;
    atomic_uint ran = ATOMIC_VAR_INIT(0U);
    unsigned point;
    int result = 1;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return 1;
    }
    opts.deterministic = 1U;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &runtime) != 0 ||
        llam_async_preempt_register_safe_range(runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0) {
        goto cleanup;
    }
    for (point = LLAM_ASYNC_TEST_FAIL_WORKER; point <= LLAM_ASYNC_TEST_FAIL_RESTORE; ++point) {
        llam_async_preempt_test_fail_allocation(point);
        errno = 0;
        if (llam_async_preempt_enable_runtime(runtime) != -1 || errno != ENOMEM ||
            atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) != 0U ||
            atomic_load_explicit(&runtime->shards[0].primary_worker.async_preempt_state,
                                 memory_order_acquire) != 0U) {
            fprintf(stderr, "async preparation failure did not unwind worker point %u: %d\n", point, errno);
            goto cleanup;
        }
    }
    if (llam_async_preempt_enable_runtime(runtime) != 0) {
        goto cleanup;
    }
    for (point = LLAM_ASYNC_TEST_FAIL_TASK; point <= LLAM_ASYNC_TEST_FAIL_BANK1; ++point) {
        llam_async_preempt_test_fail_allocation(point);
        errno = 0;
        task = llam_runtime_spawn_ex(runtime, async_allocation_task, &ran, NULL, 0U);
        if (task != NULL || errno != ENOMEM ||
            atomic_load_explicit(&runtime->shards[0].live_tasks, memory_order_acquire) != 0U) {
            fprintf(stderr, "async preparation failure did not unwind task point %u: %d\n", point, errno);
            goto cleanup;
        }
    }
    task = llam_runtime_spawn_ex(runtime, async_allocation_task, &ran, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0 ||
        atomic_load_explicit(&ran, memory_order_acquire) != 1U) {
        goto cleanup;
    }
    /* Destroy also owns capsule storage of tasks that never entered a worker. */
    if (llam_runtime_spawn_ex(runtime, async_allocation_task, &ran, NULL, 0U) == NULL) {
        goto cleanup;
    }
    result = 0;
    puts("test_async_preempt_linux_x86_64: seven preparation failures, retry and never-started cleanup passed");
cleanup:
    llam_async_preempt_test_fail_allocation(0U);
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    return result;
}

typedef struct async_preempt_test_state {
    llam_runtime_t *runtime;
    atomic_uint release_leaf;
    atomic_uint leaf_started;
    atomic_uint leaf_returned;
    atomic_uint leaf_registers_ok;
    atomic_uint companion_ran;
    atomic_uint foreign_blocking_rejected;
    atomic_uint stop_rescue;
    atomic_uint rescue_released;
    llam_task_t *leaf_task;
    llam_task_t *companion_task;
} async_preempt_test_state_t;

static void async_leaf_task(void *arg) {
    async_preempt_test_state_t *state = arg;
    unsigned a, b, c, d, pkru = 0U, restored_pkru = 0U;
    bool has_pkru = __get_cpuid_count(7U, 0U, &a, &b, &c, &d) && (c & (1U << 4U)) != 0U;
    if (has_pkru) __asm__ volatile("rdpkru" : "=a"(pkru), "=d"(d) : "c"(0U) : "memory");

    atomic_store_explicit(&state->leaf_started, 1U, memory_order_release);
    unsigned registers_ok = llam_async_safe_leaf(&state->release_leaf);
    if (has_pkru) __asm__ volatile("rdpkru" : "=a"(restored_pkru), "=d"(d) : "c"(0U) : "memory");
    atomic_store_explicit(&state->leaf_registers_ok, registers_ok && pkru == restored_pkru,
                          memory_order_release);
    atomic_store_explicit(&state->leaf_returned, 1U, memory_order_release);
}

static void async_companion_task(void *arg) {
    async_preempt_test_state_t *state = arg;
    int enter_rc;
    int enter_errno;

    while (atomic_load_explicit(&state->leaf_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    errno = 0;
    enter_rc = llam_enter_blocking();
    enter_errno = errno;
    if (enter_rc == -1 && enter_errno == ENOTSUP) {
        atomic_store_explicit(&state->foreign_blocking_rejected,
                              1U,
                              memory_order_release);
    }
    atomic_store_explicit(&state->companion_ran, 1U, memory_order_release);
    atomic_store_explicit(&state->release_leaf, 1U, memory_order_release);
}

static void *async_rescue_thread(void *arg) {
    async_preempt_test_state_t *state = arg;
    const struct timespec wait = {.tv_sec = 0, .tv_nsec = 500000000L};

    (void)nanosleep(&wait, NULL);
    if (atomic_load_explicit(&state->stop_rescue, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state->companion_ran, memory_order_acquire) == 0U) {
        atomic_store_explicit(&state->rescue_released, 1U, memory_order_release);
        atomic_store_explicit(&state->release_leaf, 1U, memory_order_release);
    }
    return NULL;
}

static void async_unsafe_leaf_task(void *arg) {
    async_preempt_test_state_t *state = arg;

    atomic_store_explicit(&state->leaf_started, 1U, memory_order_release);
    llam_async_unsafe_leaf(&state->release_leaf);
    atomic_store_explicit(&state->leaf_returned, 1U, memory_order_release);
}

static void async_conflicting_signal_handler(int signo) {
    (void)signo;
}

typedef struct async_transition_state {
    async_preempt_test_state_t base;
    llam_async_preempt_state_t *worker_state;
    pthread_t owner;
    atomic_uint allow_witness;
    atomic_uint witnessed;
    bool stop_before_ready;
} async_transition_state_t;

static void async_transition_companion(void *opaque) {
    async_transition_state_t *state = opaque;

    while (atomic_load_explicit(&state->base.leaf_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (!state->stop_before_ready) {
        llam_shard_t *shard = &state->base.runtime->shards[0];

        /* Freeze new requests under the same lock pair as the watchdog sender. */
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        atomic_store_explicit(&state->worker_state->disabled, 1U, memory_order_release);
        atomic_store_explicit(&state->worker_state->status, LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                              memory_order_release);
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        atomic_store_explicit(&state->allow_witness, 1U, memory_order_release);
        /* Resume the captured busy leaf; only the witness injector can release it. */
        llam_yield();
    } else {
        atomic_store_explicit(&state->base.release_leaf, 1U, memory_order_release);
    }
    atomic_store_explicit(&state->base.companion_ran, 1U, memory_order_release);
}

static void *async_transition_witness(void *opaque) {
    async_transition_state_t *state = opaque;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    bool sent = false;

    while (llam_now_ns() < deadline &&
           atomic_load_explicit(&state->base.stop_rescue, memory_order_acquire) == 0U) {
        if (!sent && atomic_load_explicit(&state->allow_witness, memory_order_acquire) != 0U &&
            atomic_load_explicit(&state->worker_state->pending_restore, memory_order_acquire) != 0U) {
            /* The leaf cannot exit/unbind until this thread releases it below. */
            if (pthread_kill(state->owner, state->base.runtime->async_preempt_signal) != 0) {
                break;
            }
            sent = true;
        }
        if (sent && atomic_load_explicit(&state->worker_state->observed_restore_sequence,
                                         memory_order_acquire) != 0U) {
            atomic_store_explicit(&state->witnessed, 1U, memory_order_release);
            atomic_store_explicit(&state->base.release_leaf, 1U, memory_order_release);
            return NULL;
        }
        (void)nanosleep(&delay, NULL);
    }
    atomic_store_explicit(&state->base.rescue_released, 1U, memory_order_release);
    atomic_store_explicit(&state->base.release_leaf, 1U, memory_order_release);
    return NULL;
}

static int async_test_transition(bool stop_before_ready) {
    async_transition_state_t state = {0};
    llam_runtime_opts_t opts;
    pthread_t injector;
    bool injector_started = false;
    stack_t before_stack;
    stack_t after_stack;
    sigset_t before_mask;
    sigset_t after_mask;
    unsigned stop_hook_state;
    int result = 1;
    int signo;

    atomic_init(&state.base.release_leaf, 0U);
    atomic_init(&state.base.leaf_started, 0U);
    atomic_init(&state.base.leaf_returned, 0U);
    atomic_init(&state.base.leaf_registers_ok, 0U);
    atomic_init(&state.base.companion_ran, 0U);
    atomic_init(&state.base.stop_rescue, 0U);
    atomic_init(&state.base.rescue_released, 0U);
    atomic_init(&state.allow_witness, 0U);
    atomic_init(&state.witnessed, 0U);
    state.owner = pthread_self();
    state.stop_before_ready = stop_before_ready;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0 ||
        sigaltstack(NULL, &before_stack) != 0 ||
        pthread_sigmask(SIG_SETMASK, NULL, &before_mask) != 0) {
        return 1;
    }
    opts.deterministic = 1U;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create_compat(&opts, sizeof(opts), &state.base.runtime) != 0 ||
        llam_async_preempt_register_safe_range(state.base.runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0 ||
        llam_async_preempt_enable_runtime(state.base.runtime) != 0) {
        goto cleanup;
    }
    state.worker_state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &state.base.runtime->shards[0].primary_worker.async_preempt_state, memory_order_acquire);
    state.base.leaf_task = llam_runtime_spawn_ex(state.base.runtime, async_leaf_task,
                                                &state.base, NULL, 0U);
    state.base.companion_task = llam_runtime_spawn_ex(state.base.runtime, async_transition_companion,
                                                     &state, NULL, 0U);
    if (state.base.leaf_task == NULL || state.base.companion_task == NULL ||
        pthread_create(&injector, NULL,
                       stop_before_ready ? async_rescue_thread : async_transition_witness,
                       stop_before_ready ? (void *)&state.base : (void *)&state) != 0) {
        goto cleanup;
    }
    injector_started = true;
    (void)llam_async_preempt_test_stop_before_ready(stop_before_ready ? 1U : 0U);
    if (llam_runtime_run_handle(state.base.runtime) != 0) {
        goto cleanup;
    }
    atomic_store_explicit(&state.base.stop_rescue, 1U, memory_order_release);
    (void)pthread_join(injector, NULL);
    injector_started = false;
    stop_hook_state = llam_async_preempt_test_stop_before_ready(0U);
    if (atomic_load_explicit(&state.base.rescue_released, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.base.leaf_registers_ok, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.base.leaf_returned, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.base.companion_ran, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.worker_state->pending_restore, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.base.runtime->shards[0].metrics.async_preempt_captures,
                             memory_order_acquire) != 1U ||
        (stop_before_ready ? stop_hook_state != 2U :
            atomic_load_explicit(&state.witnessed, memory_order_acquire) != 1U) ||
        sigaltstack(NULL, &after_stack) != 0 ||
        pthread_sigmask(SIG_SETMASK, NULL, &after_mask) != 0 ||
        before_stack.ss_flags != after_stack.ss_flags ||
        ((before_stack.ss_flags & SS_DISABLE) == 0 &&
         (before_stack.ss_sp != after_stack.ss_sp || before_stack.ss_size != after_stack.ss_size)) ||
        llam_join(state.base.leaf_task) != 0 || llam_join(state.base.companion_task) != 0) {
        fprintf(stderr, "async transition failed: stop=%d witness=%u rescue=%u hook=%u captures=%llu registers=%u returned=%u companion=%u\n",
                stop_before_ready, atomic_load(&state.witnessed),
                atomic_load(&state.base.rescue_released), stop_hook_state,
                (unsigned long long)atomic_load(&state.base.runtime->shards[0].metrics.async_preempt_captures),
                atomic_load(&state.base.leaf_registers_ok), atomic_load(&state.base.leaf_returned),
                atomic_load(&state.base.companion_ran));
        goto cleanup;
    }
    for (signo = 1; signo <= 64; ++signo) {
        if (sigismember(&before_mask, signo) != sigismember(&after_mask, signo)) {
            goto cleanup;
        }
    }
    result = 0;
    puts(stop_before_ready
        ? "test_async_preempt_linux_x86_64: stop between active-clear and READY, restore and host state passed"
        : "test_async_preempt_linux_x86_64: late signal witnessed restore without a new capture passed");
cleanup:
    (void)llam_async_preempt_test_stop_before_ready(0U);
    atomic_store_explicit(&state.base.stop_rescue, 1U, memory_order_release);
    atomic_store_explicit(&state.base.release_leaf, 1U, memory_order_release);
    if (injector_started) {
        (void)pthread_join(injector, NULL);
    }
    if (state.base.runtime != NULL) {
        llam_runtime_destroy(state.base.runtime);
    }
    return result;
}

static int async_test_unsafe_pc_defers(void) {
    async_preempt_test_state_t state;
    llam_runtime_opts_t opts;
    pthread_t rescue_thread;
    bool rescue_started = false;
    bool backend_enabled;
    int run_rc = -1;
    int result = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.release_leaf, 0U);
    atomic_init(&state.leaf_started, 0U);
    atomic_init(&state.leaf_returned, 0U);
    atomic_init(&state.leaf_registers_ok, 0U);
    atomic_init(&state.companion_ran, 0U);
    atomic_init(&state.foreign_blocking_rejected, 0U);
    atomic_init(&state.stop_rescue, 0U);
    atomic_init(&state.rescue_released, 0U);
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return 1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_poll_period = 1U;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create_compat(&opts, sizeof(opts), &state.runtime) != 0 ||
        state.runtime == NULL) {
        return 1;
    }
    backend_enabled =
        llam_async_preempt_register_safe_range(state.runtime,
                                               (uintptr_t)llam_async_safe_leaf_begin,
                                               (uintptr_t)llam_async_safe_leaf_end) == 0 &&
        llam_async_preempt_enable_runtime(state.runtime) == 0;
    state.leaf_task = llam_runtime_spawn_ex(state.runtime,
                                             async_unsafe_leaf_task,
                                             &state,
                                             NULL,
                                             0U);
    state.companion_task = llam_runtime_spawn_ex(state.runtime,
                                                 async_companion_task,
                                                 &state,
                                                 NULL,
                                                 0U);
    if (!backend_enabled || state.leaf_task == NULL || state.companion_task == NULL ||
        pthread_create(&rescue_thread, NULL, async_rescue_thread, &state) != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: unsafe-range setup failed: %d\n",
                errno);
        goto cleanup;
    }
    rescue_started = true;
    run_rc = llam_runtime_run_handle(state.runtime);
    atomic_store_explicit(&state.stop_rescue, 1U, memory_order_release);
    (void)pthread_join(rescue_thread, NULL);
    rescue_started = false;
    if (run_rc != 0 ||
        atomic_load_explicit(&state.leaf_returned, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.rescue_released, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.foreign_blocking_rejected, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_captures,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_deferred_unsafe,
                             memory_order_acquire) == 0U) {
        fprintf(stderr,
                "test_async_preempt_linux_x86_64: unsafe PC was not deferred "
                "(rc=%d captures=%llu deferred=%llu rescue=%u)\n",
                run_rc,
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_captures,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_deferred_unsafe,
                    memory_order_acquire),
                atomic_load_explicit(&state.rescue_released, memory_order_acquire));
        goto cleanup;
    }
    if (llam_join(state.leaf_task) != 0 || llam_join(state.companion_task) != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: unsafe-range join failed: %d\n",
                errno);
        goto cleanup;
    }
    result = 0;

cleanup:
    atomic_store_explicit(&state.stop_rescue, 1U, memory_order_release);
    atomic_store_explicit(&state.release_leaf, 1U, memory_order_release);
    if (rescue_started) {
        (void)pthread_join(rescue_thread, NULL);
    }
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    return result;
}

#define ASYNC_COHORT_TASKS 6U

typedef struct async_cohort async_cohort_t;
typedef struct async_cohort_item {
    async_cohort_t *cohort;
    unsigned index;
    atomic_uint release;
} async_cohort_item_t;

struct async_cohort {
    async_cohort_item_t item[ASYNC_COHORT_TASKS];
    atomic_uint started;
    atomic_uint finished;
    atomic_uint failed;
    atomic_uint controller_done;
    atomic_uint joined;
    atomic_uint rescue_stop;
    atomic_uint rescued;
    llam_runtime_t *runtime;
    uint64_t captures_target;
    llam_task_t *join_target;
    bool avx;
};

static void async_cohort_leaf(void *opaque) {
    async_cohort_item_t *item = opaque;
    async_cohort_t *cohort = item->cohort;
    uint32_t old_mxcsr;
    uint32_t wanted_mxcsr;
    uint32_t observed_mxcsr;
    uint16_t old_cw;
    uint16_t wanted_cw;
    uint16_t observed_cw;
    sigset_t set;
    sigset_t old_mask;
    sigset_t observed_mask;
    int expected_errno = EDOM + (int)item->index;
    int observed_errno;
    unsigned registers_ok;
    unsigned mode = (item->index & 1U) != 0U ? 2U : 1U;

    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(old_mxcsr), "=m"(old_cw));
    wanted_mxcsr = (old_mxcsr & ~UINT32_C(0x6000)) | (mode << 13U);
    wanted_cw = (uint16_t)((old_cw & ~UINT16_C(0x0c00)) | (mode << 10U));
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(wanted_mxcsr), "m"(wanted_cw));
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    if (pthread_sigmask((item->index & 1U) != 0U ? SIG_UNBLOCK : SIG_BLOCK,
                        &set, &old_mask) != 0) {
        atomic_fetch_add_explicit(&cohort->failed, 1U, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&cohort->started, 1U, memory_order_release);
    errno = expected_errno;
    registers_ok = cohort->avx && (item->index & 1U) != 0U
                       ? (unsigned)llam_async_avx_leaf(&item->release)
                       : llam_async_safe_leaf(&item->release);
    observed_errno = errno;
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(observed_mxcsr), "=m"(observed_cw));
    if (registers_ok != 1U || observed_errno != expected_errno ||
        (observed_mxcsr & UINT32_C(0x6000)) != (wanted_mxcsr & UINT32_C(0x6000)) ||
        (observed_cw & UINT16_C(0x0c00)) != (wanted_cw & UINT16_C(0x0c00)) ||
        pthread_sigmask(SIG_SETMASK, NULL, &observed_mask) != 0 ||
        sigismember(&observed_mask, SIGUSR2) != ((item->index & 1U) == 0U ? 1 : 0)) {
        atomic_fetch_add_explicit(&cohort->failed, 1U, memory_order_relaxed);
    }
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(old_mxcsr), "m"(old_cw));
    (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    atomic_fetch_add_explicit(&cohort->finished, 1U, memory_order_release);
}

static void async_cohort_controller(void *opaque) {
    async_cohort_t *cohort = opaque;
    sigset_t set;
    unsigned i;

    while (atomic_load_explicit(&cohort->started, memory_order_acquire) != ASYNC_COHORT_TASKS) {
        llam_yield();
    }
    /* Reaching this G requires all earlier no-poll Gs to be suspended together. */
    if (atomic_load_explicit(&cohort->started, memory_order_acquire) != ASYNC_COHORT_TASKS ||
        atomic_load_explicit(&cohort->finished, memory_order_acquire) != 0U) {
        atomic_fetch_add_explicit(&cohort->failed, 1U, memory_order_relaxed);
    }
    /* Repeated captures force restore observation plus use of the other bank. */
    while (atomic_load_explicit(&cohort->runtime->shards[0].metrics.async_preempt_captures,
                                memory_order_acquire) < cohort->captures_target &&
           atomic_load_explicit(&cohort->rescued, memory_order_acquire) == 0U) {
        llam_yield();
    }
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    (void)pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    for (i = 0U; i < ASYNC_COHORT_TASKS; ++i) {
        atomic_store_explicit(&cohort->item[i].release, 1U, memory_order_release);
    }
    /* Target is still suspended: managed join must not restore its stale ctx. */
    if (llam_join(cohort->join_target) != 0) {
        atomic_fetch_add_explicit(&cohort->failed, 1U, memory_order_relaxed);
    } else {
        atomic_store_explicit(&cohort->joined, 1U, memory_order_release);
    }
    atomic_store_explicit(&cohort->controller_done, 1U, memory_order_release);
}

static void *async_cohort_rescue(void *opaque) {
    async_cohort_t *cohort = opaque;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
    unsigned i;

    while (atomic_load_explicit(&cohort->rescue_stop, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        (void)nanosleep(&delay, NULL);
    }
    if (atomic_load_explicit(&cohort->rescue_stop, memory_order_acquire) == 0U) {
        atomic_store_explicit(&cohort->rescued, 1U, memory_order_release);
        for (i = 0U; i < ASYNC_COHORT_TASKS; ++i) {
            atomic_store_explicit(&cohort->item[i].release, 1U, memory_order_release);
        }
    }
    return NULL;
}

static int async_test_multiple_capsules(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_async_preempt_state_t *worker_state;
    sigset_t initial_mask;
    unsigned round;
    int result = 1;

    (void)pthread_sigmask(SIG_SETMASK, NULL, &initial_mask);
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return 1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create_compat(&opts, sizeof(opts), &runtime) != 0 ||
        llam_async_preempt_register_safe_range(runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0 ||
        llam_async_preempt_register_safe_range(runtime,
            (uintptr_t)llam_async_avx_leaf_begin, (uintptr_t)llam_async_avx_leaf_end) != 0 ||
        llam_async_preempt_enable_runtime(runtime) != 0) {
        goto cleanup;
    }
    worker_state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &runtime->shards[0].primary_worker.async_preempt_state, memory_order_acquire);
    for (round = 0U; round < 3U; ++round) {
        async_cohort_t cohort = {0};
        llam_task_t *tasks[ASYNC_COHORT_TASKS + 1U] = {0};
        pthread_t rescue;
        bool rescue_started = false;
        uint64_t before = atomic_load_explicit(&runtime->shards[0].metrics.async_preempt_captures,
                                               memory_order_acquire);
        unsigned i;
        int run_result = -1;

        atomic_init(&cohort.started, 0U);
        atomic_init(&cohort.finished, 0U);
        atomic_init(&cohort.failed, 0U);
        atomic_init(&cohort.controller_done, 0U);
        atomic_init(&cohort.joined, 0U);
        atomic_init(&cohort.rescue_stop, 0U);
        atomic_init(&cohort.rescued, 0U);
        cohort.avx = (worker_state->xsave_mask & UINT64_C(4)) != 0U;
        cohort.runtime = runtime;
        cohort.captures_target = before + 4U * ASYNC_COHORT_TASKS;
        for (i = 0U; i < ASYNC_COHORT_TASKS; ++i) {
            cohort.item[i].cohort = &cohort;
            cohort.item[i].index = i;
            atomic_init(&cohort.item[i].release, 0U);
            tasks[i] = llam_runtime_spawn_ex(runtime, async_cohort_leaf, &cohort.item[i], NULL, 0U);
            if (tasks[i] == NULL) {
                goto round_cleanup;
            }
        }
        cohort.join_target = tasks[0];
        tasks[ASYNC_COHORT_TASKS] = llam_runtime_spawn_ex(
            runtime, async_cohort_controller, &cohort, NULL, 0U);
        if (tasks[ASYNC_COHORT_TASKS] == NULL ||
            pthread_create(&rescue, NULL, async_cohort_rescue, &cohort) != 0) {
            goto round_cleanup;
        }
        rescue_started = true;
        run_result = llam_runtime_run_handle(runtime);
round_cleanup:
        atomic_store_explicit(&cohort.rescue_stop, 1U, memory_order_release);
        if (rescue_started) {
            pthread_join(rescue, NULL);
        }
        if (run_result != 0 ||
            atomic_load_explicit(&cohort.failed, memory_order_acquire) != 0U ||
            atomic_load_explicit(&cohort.rescued, memory_order_acquire) != 0U ||
            atomic_load_explicit(&cohort.finished, memory_order_acquire) != ASYNC_COHORT_TASKS ||
            atomic_load_explicit(&cohort.controller_done, memory_order_acquire) == 0U ||
            atomic_load_explicit(&cohort.joined, memory_order_acquire) == 0U ||
            atomic_load_explicit(&worker_state->pending_restore, memory_order_acquire) != 0U ||
            atomic_load_explicit(&runtime->shards[0].metrics.async_preempt_captures,
                                 memory_order_acquire) < before ||
            atomic_load_explicit(&runtime->shards[0].metrics.async_preempt_captures,
                                 memory_order_acquire) - before < 4U * ASYNC_COHORT_TASKS) {
            fprintf(stderr, "async cohort failed: round=%u run=%d started=%u finished=%u failed=%u rescued=%u\n",
                    round, run_result, atomic_load(&cohort.started), atomic_load(&cohort.finished),
                    atomic_load(&cohort.failed), atomic_load(&cohort.rescued));
            goto cleanup;
        }
        for (i = 1U; i <= ASYNC_COHORT_TASKS; ++i) {
            if (llam_join(tasks[i]) != 0) {
                goto cleanup;
            }
        }
        (void)pthread_sigmask(SIG_SETMASK, &initial_mask, NULL);
    }
    puts("test_async_preempt_linux_x86_64: 3 x 6 simultaneous capsules, >=72 captures, FP/errno/mask and supported AVX passed");
    result = 0;
cleanup:
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    (void)pthread_sigmask(SIG_SETMASK, &initial_mask, NULL);
    return result;
}

typedef struct async_signal_namespace_call {
    int signal_number;
    int error;
    llam_signal_set_t *set;
} async_signal_namespace_call_t;

static void async_namespace_create_waiter(void *opaque) {
    async_signal_namespace_call_t *call = opaque;
    if (llam_signal_set_create_ex(&call->signal_number, 1U, NULL, 0U, &call->set) != 0) {
        call->error = errno;
    }
}

static int async_namespace_run_waiter(llam_runtime_t *runtime,
                                      async_signal_namespace_call_t *call) {
    llam_task_t *task = llam_runtime_spawn_ex(runtime, async_namespace_create_waiter,
                                             call, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) return -1;
    if (call->error != 0) {
        errno = call->error;
        return -1;
    }
    return 0;
}

static int async_test_signal_namespace(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *waiter_runtime = NULL;
    llam_runtime_t *async_runtime = NULL;
    async_signal_namespace_call_t call = {.signal_number = SIGRTMIN + 2};
    llam_signal_set_t *refused = NULL;
    struct sigaction action;
    sigset_t initial_mask;
    sigset_t before_mask;
    sigset_t after_mask;
    int private_signal = 0;
    int result = 1;

    if (pthread_sigmask(SIG_SETMASK, NULL, &initial_mask) != 0) return 1;
    if (sigaction(call.signal_number, NULL, &action) != 0 || action.sa_handler != SIG_DFL) {
        puts("async namespace: skipped (first candidate has a foreign action)");
        return 77;
    }
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) goto cleanup;
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &waiter_runtime) != 0 ||
        async_namespace_run_waiter(waiter_runtime, &call) != 0 || call.set == NULL ||
        llam_runtime_create_compat(&opts, sizeof(opts), &async_runtime) != 0 ||
        llam_async_preempt_register_safe_range(async_runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0) {
        goto cleanup;
    }
    if (llam_async_preempt_enable_runtime(async_runtime) != 0) {
        if (errno == ENOTSUP) result = 77;
        goto cleanup;
    }
    private_signal = async_runtime->async_preempt_signal;
    if (private_signal == call.signal_number) {
        fprintf(stderr, "async namespace: stolen waiter signal %d during preparation\n", private_signal);
        goto cleanup;
    }
    if (private_signal <= 0 || pthread_sigmask(SIG_SETMASK, NULL, &before_mask) != 0 ||
        sigismember(&before_mask, private_signal) != 0) goto cleanup;
    errno = 0;
    if (llam_signal_set_create_ex(&private_signal, 1U, NULL, 0U, &refused) != -1 ||
        errno != EBUSY || refused != NULL ||
        pthread_sigmask(SIG_SETMASK, NULL, &after_mask) != 0 ||
        sigismember(&after_mask, private_signal) != 0) {
        fprintf(stderr, "async namespace: private signal waiter not refused without mask changes\n");
        goto cleanup;
    }
    llam_runtime_destroy(async_runtime);
    async_runtime = NULL;
    if (sigaction(private_signal, NULL, &action) != 0 || action.sa_handler != SIG_DFL ||
        llam_signal_set_destroy(call.set) != 0) goto cleanup;
    call.set = NULL;
    call.signal_number = private_signal;
    call.error = 0;
    if (async_namespace_run_waiter(waiter_runtime, &call) != 0 || call.set == NULL) goto cleanup;
    puts("async namespace: waiter-first exclusion, async-first EBUSY/no mask change, post-release reuse passed (preparation only)");
    result = 0;
cleanup:
    if (async_runtime != NULL) llam_runtime_destroy(async_runtime);
    if (refused != NULL && llam_signal_set_destroy(refused) != 0) result = 1;
    if (call.set != NULL && llam_signal_set_destroy(call.set) != 0) result = 1;
    if (waiter_runtime != NULL) llam_runtime_destroy(waiter_runtime);
    if (pthread_sigmask(SIG_SETMASK, &initial_mask, NULL) != 0) result = 1;
    return result;
}

#if defined(LLAM_TEST_WRAP_SIGACTION)
enum async_signal_fault {
    ASYNC_SIGNAL_INSTALL_EIO = 1,
    ASYNC_SIGNAL_INSTALL_EINVAL,
    ASYNC_SIGNAL_PREVIOUS_FOREIGN,
    ASYNC_SIGNAL_ROLLBACK_QUERY,
    ASYNC_SIGNAL_ROLLBACK_RESTORE,
    ASYNC_SIGNAL_SHUTDOWN_QUERY,
    ASYNC_SIGNAL_SHUTDOWN_RESTORE
};
static unsigned async_signal_fault;
static int async_fault_signal;
static bool async_fault_fired;
static bool async_fault_installed;
static bool async_fault_shutdown;

int __real_sigaction(int signo, const struct sigaction *action, struct sigaction *previous);
int __wrap_sigaction(int signo, const struct sigaction *action, struct sigaction *previous);

static void async_namespace_foreign_handler(int signo) { (void)signo; }

/* Link-time test interception only. Production calls the real sigaction; no
 * runtime hook, handler lock or changed native context path is introduced. */
int __wrap_sigaction(int signo, const struct sigaction *action, struct sigaction *previous) {
    if (async_signal_fault != 0U && signo == async_fault_signal) {
        if (!async_fault_fired && action != NULL && previous != NULL) {
            async_fault_fired = true;
            if (async_signal_fault == ASYNC_SIGNAL_INSTALL_EIO ||
                async_signal_fault == ASYNC_SIGNAL_INSTALL_EINVAL) {
                errno = async_signal_fault == ASYNC_SIGNAL_INSTALL_EIO ? EIO : EINVAL;
                return -1;
            }
            if (async_signal_fault >= ASYNC_SIGNAL_PREVIOUS_FOREIGN &&
                async_signal_fault <= ASYNC_SIGNAL_ROLLBACK_RESTORE) {
                struct sigaction foreign = {.sa_handler = async_namespace_foreign_handler};
                sigemptyset(&foreign.sa_mask);
                if (__real_sigaction(signo, &foreign, NULL) != 0) _Exit(11);
            }
            int rc = __real_sigaction(signo, action, previous);
            async_fault_installed = rc == 0;
            return rc;
        }
        if (async_fault_installed &&
            ((action == NULL &&
              (async_signal_fault == ASYNC_SIGNAL_ROLLBACK_QUERY ||
               (async_fault_shutdown && async_signal_fault == ASYNC_SIGNAL_SHUTDOWN_QUERY))) ||
             (action != NULL && previous == NULL &&
              (async_signal_fault == ASYNC_SIGNAL_ROLLBACK_RESTORE ||
               (async_fault_shutdown && async_signal_fault == ASYNC_SIGNAL_SHUTDOWN_RESTORE))))) {
            errno = EIO;
            return -1;
        }
    }
    return __real_sigaction(signo, action, previous);
}

static int async_signal_fault_child(unsigned fault) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    struct sigaction original;
    struct sigaction observed;
    int result = 1;
    int rc;
    int error;

    async_fault_signal = SIGRTMIN + 2;
    if (__real_sigaction(async_fault_signal, NULL, &original) != 0 ||
        original.sa_handler != SIG_DFL) return 77;
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0 ||
        llam_async_preempt_register_safe_range(runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0) goto cleanup;
    async_signal_fault = fault;
    errno = 0;
    rc = llam_async_preempt_enable_runtime(runtime);
    error = errno;
    if (!async_fault_fired) {
        if (rc == -1 && error == ENOTSUP) result = 77;
        goto cleanup;
    }
    if (fault == ASYNC_SIGNAL_INSTALL_EIO) {
        if (rc != -1 || error != EIO) goto cleanup;
    } else {
        if (rc != 0) goto cleanup;
        if (fault == ASYNC_SIGNAL_INSTALL_EINVAL || fault == ASYNC_SIGNAL_PREVIOUS_FOREIGN) {
            if (runtime->async_preempt_signal == async_fault_signal) goto cleanup;
        }
    }
    if (fault >= ASYNC_SIGNAL_SHUTDOWN_QUERY) {
        async_fault_shutdown = true;
        llam_runtime_destroy(runtime);
        return 1; /* Must have terminated inside production teardown. */
    }
    if (__real_sigaction(async_fault_signal, NULL, &observed) != 0 ||
        (fault == ASYNC_SIGNAL_PREVIOUS_FOREIGN
             ? observed.sa_handler != async_namespace_foreign_handler
             : observed.sa_handler != SIG_DFL)) goto cleanup;
    if (llam_async_signal_claim_number(async_fault_signal) != 0) goto cleanup;
    llam_async_signal_release_number(async_fault_signal);
    result = 0;
cleanup:
    async_signal_fault = 0U;
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (__real_sigaction(async_fault_signal, &original, NULL) != 0) result = 1;
    return result;
}

static int async_test_signal_faults(void) {
    /* Fork before creating a runtime or any LLAM native worker. Each child owns
     * its process-global dispositions, so intentional fail-stop cannot escape. */
    for (unsigned fault = ASYNC_SIGNAL_INSTALL_EIO; fault <= ASYNC_SIGNAL_SHUTDOWN_RESTORE; ++fault) {
        pid_t child = fork();
        int status;
        pid_t waited;
        int expected = fault >= ASYNC_SIGNAL_ROLLBACK_QUERY ? 125 : 0;
        if (child < 0) return 1;
        if (child == 0) _Exit(async_signal_fault_child(fault));
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status)) return 1;
        int actual = WEXITSTATUS(status);
        printf("async namespace fault=%u expectedChildExit=%d actualChildExit=%d\n", fault, expected, actual);
        if (actual == 77) return 77;
        if (actual != expected) return 1;
    }
    puts("async namespace: install errors/errno/reusable claims, foreign rollback and required fail-stop passed");
    return 0;
}
#endif

#if defined(LLAM_TSAN_BUILD)
static int async_test_intercepted_context_refusal(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    int result = 1;
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0) return 1;
    if (llam_async_preempt_register_safe_range(runtime,
            (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) == 0 &&
        llam_async_preempt_enable_runtime(runtime) == -1 && errno == ENOTSUP &&
        runtime->async_preempt_control == NULL &&
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U &&
        atomic_load_explicit(&runtime->preempt_capability.failure_reason, memory_order_acquire) ==
            LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED) {
        result = 0;
        puts("test_async_preempt_linux_x86_64: intercepted kernel context refused before native publication");
    }
    llam_runtime_destroy(runtime);
    return result;
}
#endif

int main(int argc, char **argv) {
    async_preempt_test_state_t state;
    llam_runtime_opts_t opts;
    pthread_t rescue_thread;
    struct sigaction prior_conflict_action;
    struct sigaction conflict_action;
    struct sigaction observed_conflict_action;
    bool rescue_started = false;
    bool backend_enabled;
    bool conflict_handler_installed = false;
    int conflict_signal = SIGRTMIN + 2;
    llam_async_preempt_state_t *capture_state;
    int run_rc;
    int result = 1;

    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--frame-only") == 0) return async_test_frame_refusal();
    if (argc == 2 && strcmp(argv[1], "--signal-namespace") == 0) {
        return async_test_signal_namespace();
    }
#if defined(LLAM_TEST_WRAP_SIGACTION)
    if (argc == 2 && strcmp(argv[1], "--signal-namespace-faults") == 0) {
        return async_test_signal_faults();
    }
#endif
    if (argc != 1) return 2;
    if (async_test_frame_refusal() != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: signal selector contract failed\n");
        return 1;
    }
#if defined(LLAM_TSAN_BUILD)
    return async_test_intercepted_context_refusal();
#endif
    memset(&state, 0, sizeof(state));
    atomic_init(&state.release_leaf, 0U);
    atomic_init(&state.leaf_started, 0U);
    atomic_init(&state.leaf_returned, 0U);
    atomic_init(&state.leaf_registers_ok, 0U);
    atomic_init(&state.companion_ran, 0U);
    atomic_init(&state.foreign_blocking_rejected, 0U);
    atomic_init(&state.stop_rescue, 0U);
    atomic_init(&state.rescue_released, 0U);
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: runtime opts init failed\n");
        return 1;
    }
    if (sigaction(conflict_signal, NULL, &observed_conflict_action) == 0 &&
        observed_conflict_action.sa_handler == SIG_DFL) {
        memset(&conflict_action, 0, sizeof(conflict_action));
        sigemptyset(&conflict_action.sa_mask);
        conflict_action.sa_handler = async_conflicting_signal_handler;
        if (sigaction(conflict_signal, &conflict_action, &prior_conflict_action) == 0) {
            conflict_handler_installed = true;
        }
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_poll_period = 1U;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create_compat(&opts, sizeof(opts), &state.runtime) != 0 ||
        state.runtime == NULL) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: runtime create failed: %d\n", errno);
        goto cleanup;
    }
    backend_enabled =
        llam_async_preempt_register_safe_range(state.runtime,
                                               (uintptr_t)llam_async_safe_leaf_begin,
                                               (uintptr_t)llam_async_safe_leaf_end) == 0 &&
        llam_async_preempt_register_safe_range(state.runtime,
                                               (uintptr_t)llam_async_safe_leaf_begin,
                                               (uintptr_t)llam_async_safe_leaf_end) == -1 &&
        errno == EEXIST &&
        llam_async_preempt_enable_runtime(state.runtime) == 0;
    if (backend_enabled && conflict_handler_installed &&
        state.runtime->async_preempt_signal == conflict_signal) {
        backend_enabled = false;
        errno = EBUSY;
    }
    if (!backend_enabled) {
        unsigned reason = atomic_load_explicit(
            &state.runtime->preempt_capability.failure_reason, memory_order_acquire);

        if (reason == LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED) {
            puts("test_async_preempt_linux_x86_64: skipped (CPU context unsupported)");
            result = 77;
            goto cleanup;
        }
        fprintf(stderr,
                "test_async_preempt_linux_x86_64: exercising cooperative no-capture baseline: %d\n",
                errno);
    }
    state.leaf_task = llam_runtime_spawn_ex(state.runtime,
                                             async_leaf_task,
                                             &state,
                                             NULL,
                                             0U);
    state.companion_task = llam_runtime_spawn_ex(state.runtime,
                                                 async_companion_task,
                                                 &state,
                                                 NULL,
                                                 0U);
    if (state.leaf_task == NULL || state.companion_task == NULL ||
        pthread_create(&rescue_thread, NULL, async_rescue_thread, &state) != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: task/rescue setup failed\n");
        goto cleanup;
    }
    rescue_started = true;
    run_rc = llam_runtime_run_handle(state.runtime);
    atomic_store_explicit(&state.stop_rescue, 1U, memory_order_release);
    (void)pthread_join(rescue_thread, NULL);
    rescue_started = false;
    capture_state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &state.runtime->shards[0].primary_worker.async_preempt_state,
        memory_order_acquire);
    if (run_rc == -1 && capture_state != NULL &&
        atomic_load_explicit(&state.runtime->preempt_capability.failure_reason,
                             memory_order_acquire) == LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED &&
        atomic_load_explicit(&capture_state->frame_probe_status, memory_order_acquire) == 3U &&
        atomic_load_explicit(&state.leaf_started, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state.companion_ran, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_captures,
                             memory_order_acquire) == 0U) {
        puts("test_async_preempt_linux_x86_64: skipped (signal ABI rejected before user dispatch)");
        result = 77;
        goto cleanup;
    }
    if (!backend_enabled || run_rc != 0 ||
        atomic_load_explicit(&state.companion_ran, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.leaf_returned, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.leaf_registers_ok, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.foreign_blocking_rejected, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_captures,
                             memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_resumes,
                             memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_context_bytes,
                             memory_order_acquire) < sizeof(llam_async_preempt_context_t) ||
        atomic_load_explicit(&state.runtime->shards[0].metrics.async_preempt_max_ack_latency_ns,
                             memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.rescue_released, memory_order_acquire) != 0U) {
        fprintf(stderr,
                "test_async_preempt_linux_x86_64: no-poll leaf did not resume after the companion "
                "(rc=%d enabled=%u started=%u companion=%u returned=%u registers=%u rescue=%u "
                "requests=%llu captures=%llu resumes=%llu context_bytes=%llu ack_max_ns=%llu "
                "unsafe=%llu stale=%llu defer_reason=%u)\n",
                run_rc,
                backend_enabled ? 1U : 0U,
                atomic_load_explicit(&state.leaf_started, memory_order_acquire),
                atomic_load_explicit(&state.companion_ran, memory_order_acquire),
                atomic_load_explicit(&state.leaf_returned, memory_order_acquire),
                atomic_load_explicit(&state.leaf_registers_ok, memory_order_acquire),
                atomic_load_explicit(&state.rescue_released, memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_requests,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_captures,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_resumes,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_context_bytes,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_max_ack_latency_ns,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_deferred_unsafe,
                    memory_order_acquire),
                (unsigned long long)atomic_load_explicit(
                    &state.runtime->shards[0].metrics.async_preempt_deferred_stale,
                    memory_order_acquire),
                capture_state != NULL
                    ? atomic_load_explicit(&capture_state->last_defer_reason,
                                           memory_order_acquire)
                    : 0U);
        goto cleanup;
    }
    if (llam_join(state.leaf_task) != 0 || llam_join(state.companion_task) != 0) {
        fprintf(stderr, "test_async_preempt_linux_x86_64: task join failed: %d\n", errno);
        goto cleanup;
    }
    result = 0;

cleanup:
    atomic_store_explicit(&state.stop_rescue, 1U, memory_order_release);
    atomic_store_explicit(&state.release_leaf, 1U, memory_order_release);
    if (rescue_started) {
        (void)pthread_join(rescue_thread, NULL);
    }
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    if (conflict_handler_installed) {
        (void)sigaction(conflict_signal, &prior_conflict_action, NULL);
    }
    if (result == 0) {
        result = async_test_unsafe_pc_defers();
    }
    if (result == 0) {
        result = async_test_multiple_capsules();
    }
    if (result == 0) {
        result = async_test_preparation_failures();
    }
    if (result == 0) {
        result = async_test_transition(false);
    }
    if (result == 0) {
        result = async_test_transition(true);
    }
    if (result == 0) {
        puts("test_async_preempt_linux_x86_64: safe capture/register restore and unsafe deferral passed");
    }
    return result;
}

#endif
