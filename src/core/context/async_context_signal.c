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

#if LLAM_ASYNC_CONTEXT_SIGNAL
#include "async_context_signal.h"
#include "async_signal_registry.h"

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 &&
               ATOMIC_POINTER_LOCK_FREE == 2, "signal mailbox atomics must be lock-free");

static pthread_mutex_t g_llam_signal_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_llam_signal_owner_refs;
static int g_llam_signal_reserved;
static struct sigaction g_llam_signal_previous;
/* The frontend requires static TLS routing. No dynamic TLS resolver or
 * pthread_getspecific may run from the handler. */
static _Thread_local llam_async_signal_binding_t *g_llam_signal_binding
    __attribute__((tls_model("initial-exec")));
#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_llam_signal_fail_probe;
void llam_async_signal_test_fail_probe(void) {
    atomic_store_explicit(&g_llam_signal_fail_probe, 1U, memory_order_release);
}
#endif

static LLAM_NORETURN void llam_signal_uncertain(void) {
    _Exit(125);
}

static void llam_signal_handler(int signal_number, siginfo_t *info, void *opaque) {
    llam_async_signal_binding_t *binding = g_llam_signal_binding;
    int *error_location;
    int entry_errno;
    int return_errno;
    if (binding == NULL) return; /* No libc/TLS helper even on a foreign thread. */
    error_location = binding->errno_location; /* Prewarmed before unblocking. */
    entry_errno = *error_location;
    if (signal_number != binding->signal_number || info == NULL ||
        info->si_code != LLAM_ASYNC_SIGNAL_SENDER_CODE || opaque == NULL) return;
    return_errno = llam_async_shared_signal_handle(binding, opaque, entry_errno);
    /* No request/G/bank access after the entry's terminal result publication.
     * This M's TLS/altstack/epilogue outlive it until ordinary target close. */
    *error_location = return_errno;
}

static bool llam_signal_action_is_ours(const struct sigaction *action) {
    return (action->sa_flags & SA_SIGINFO) != 0 &&
           action->sa_sigaction == llam_signal_handler;
}

int llam_async_signal_process_acquire(int *signal_number) {
    struct sigaction action = {0}, current, previous;
    int error = 0;
    if (signal_number == NULL) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&g_llam_signal_owner_lock);
    if (g_llam_signal_owner_refs == UINT_MAX) {
        error = EOVERFLOW;
    } else if (g_llam_signal_owner_refs != 0U) {
        if (sigaction(g_llam_signal_reserved, NULL, &current) != 0 ||
            !llam_signal_action_is_ours(&current)) error = EBUSY;
    } else {
        action.sa_sigaction = llam_signal_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigfillset(&action.sa_mask);
        for (int number = SIGRTMIN + 2; number <= SIGRTMAX; ++number) {
            if ((unsigned)number > LLAM_ASYNC_SIGNAL_MASK_WORDS * 64U || sigaction(number, NULL, &current) != 0 ||
                current.sa_handler != SIG_DFL) continue;
            /* Owner mutex -> existing signal registry. Wait-set creation never
             * takes the owner mutex; no handler takes either ordinary lock. */
            if (llam_async_signal_claim_number(number) != 0) continue;
            if (sigaction(number, &action, &previous) != 0) {
                llam_async_signal_release_number(number);
                continue;
            }
            if (previous.sa_handler != SIG_DFL) {
                /* Concurrent foreign disposition change is not ownership. */
                if (sigaction(number, NULL, &current) == 0 && llam_signal_action_is_ours(&current)) {
                    if (sigaction(number, &previous, NULL) != 0) llam_signal_uncertain();
                }
                llam_async_signal_release_number(number);
                continue;
            }
            g_llam_signal_reserved = number;
            g_llam_signal_previous = previous;
            break;
        }
        if (g_llam_signal_reserved == 0) error = EBUSY;
    }
    if (error == 0) {
        ++g_llam_signal_owner_refs;
        *signal_number = g_llam_signal_reserved;
    }
    pthread_mutex_unlock(&g_llam_signal_owner_lock);
    if (error != 0) errno = error;
    return error == 0 ? 0 : -1;
}

void llam_async_signal_process_release(int signal_number) {
    struct sigaction current;
    pthread_mutex_lock(&g_llam_signal_owner_lock);
    if (g_llam_signal_owner_refs == 0U || signal_number != g_llam_signal_reserved) {
        llam_signal_uncertain();
    }
    if (--g_llam_signal_owner_refs == 0U) {
        if (sigaction(signal_number, NULL, &current) != 0) llam_signal_uncertain();
        if (llam_signal_action_is_ours(&current) &&
            sigaction(signal_number, &g_llam_signal_previous, NULL) != 0) {
            llam_signal_uncertain();
        }
        llam_async_signal_release_number(signal_number);
        g_llam_signal_reserved = 0;
    }
    pthread_mutex_unlock(&g_llam_signal_owner_lock);
}

void llam_async_signal_prepare(llam_async_signal_binding_t *binding, llam_worker_t *worker,
                               void *probe, int signal_number) {
    binding->worker = worker;
    binding->probe = probe;
    binding->signal_number = signal_number;
    atomic_init(&binding->stamp, 0U);
    atomic_init(&binding->scheduler_run, 0U);
    for (size_t i = 0U; i < LLAM_ASYNC_SIGNAL_MASK_WORDS; ++i)
        atomic_init(&binding->scheduler_mask[i], 0U);
    atomic_init(&binding->scheduler_errno, 0);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    atomic_init(&binding->cancellations, 0U);
    for (unsigned i = 0U; i < LLAM_ASYNC_SIGNAL_COMMAND_COUNT; ++i) {
        atomic_init(&binding->tail_observed[i], NULL);
        atomic_init(&binding->tail_release[i], NULL);
    }
#endif
}

void llam_async_signal_save_scheduler(llam_async_signal_binding_t *binding, uint64_t run,
                                      int error, const sigset_t *mask) {
    uint64_t bits[LLAM_ASYNC_SIGNAL_MASK_WORDS];
    memcpy(bits, mask, sizeof(bits));
    if (binding != g_llam_signal_binding || run == 0U ||
        llam_signal_mask_blocks(mask, binding->signal_number)) llam_signal_uncertain();
    for (size_t i = 0U; i < LLAM_ASYNC_SIGNAL_MASK_WORDS; ++i)
        atomic_store_explicit(&binding->scheduler_mask[i], bits[i], memory_order_relaxed);
    atomic_store_explicit(&binding->scheduler_errno, error, memory_order_relaxed);
    atomic_store_explicit(&binding->scheduler_run, run, memory_order_release);
}

bool llam_async_signal_idle(const llam_async_signal_binding_t *binding) {
    return (atomic_load_explicit(&binding->stamp, memory_order_acquire) &
            LLAM_ASYNC_SIGNAL_STATE_MASK) == LLAM_ASYNC_SIGNAL_IDLE;
}

int llam_async_signal_arm(llam_async_signal_binding_t *binding,
                         const llam_async_signal_request_t *request) {
    uint64_t stamp;
    if (!binding->bound || !llam_async_signal_idle(binding)) return EBUSY;
    if (request->kind >= LLAM_ASYNC_SIGNAL_COMMAND_COUNT ||
        request->binding_incarnation != binding->incarnation) return EINVAL;
    if (binding->next_sequence == (UINT64_MAX >> LLAM_ASYNC_SIGNAL_SEQUENCE_SHIFT)) return EOVERFLOW;
    stamp = ++binding->next_sequence << LLAM_ASYNC_SIGNAL_SEQUENCE_SHIFT;
    binding->request = *request;
    binding->result_error = 0;
    /* Ordinary controller owns the already-pinned descriptor before ARMED.
     * Exact stamped CAS in the handler prevents cancellation/reuse ABA. */
    atomic_store_explicit(&binding->stamp, stamp | LLAM_ASYNC_SIGNAL_ARMED, memory_order_release);
    return 0;
}

int llam_async_signal_send(const llam_async_signal_binding_t *binding) {
    return pthread_kill(binding->thread, binding->signal_number);
}

bool llam_async_signal_cancel(llam_async_signal_binding_t *binding) {
    uint64_t stamp = atomic_load_explicit(&binding->stamp, memory_order_acquire);
    if ((stamp & LLAM_ASYNC_SIGNAL_STATE_MASK) != LLAM_ASYNC_SIGNAL_ARMED) return false;
    bool canceled = atomic_compare_exchange_strong_explicit(&binding->stamp, &stamp,
        stamp & ~(uint64_t)LLAM_ASYNC_SIGNAL_STATE_MASK, memory_order_acq_rel, memory_order_acquire);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (canceled) atomic_fetch_add_explicit(&binding->cancellations, 1U, memory_order_relaxed);
#endif
    return canceled;
}

bool llam_async_signal_take_result(llam_async_signal_binding_t *binding,
                                  llam_async_signal_result_t *out) {
    uint64_t stamp = atomic_load_explicit(&binding->stamp, memory_order_acquire);
    if ((stamp & LLAM_ASYNC_SIGNAL_STATE_MASK) != LLAM_ASYNC_SIGNAL_RESULT) return false;
    *out = (llam_async_signal_result_t){.request = binding->request, .error = binding->result_error};
    /* Handler's terminal release is its LAST request/G/bank access. This does
     * not close its signal activation or permit alternate-stack teardown. */
    atomic_store_explicit(&binding->stamp, stamp & ~(uint64_t)LLAM_ASYNC_SIGNAL_STATE_MASK,
                          memory_order_release);
    return true;
}

static void llam_signal_close_current(void *opaque) {
    llam_async_signal_binding_t *binding = opaque;
    sigset_t reserved;
    const struct timespec zero = {0};
    if (!binding->bound) return;
    if (g_llam_signal_binding != binding || pthread_equal(binding->thread, pthread_self()) == 0) {
        llam_signal_uncertain();
    }
    sigemptyset(&reserved);
    sigaddset(&reserved, binding->signal_number);
    if (pthread_sigmask(SIG_BLOCK, &reserved, NULL) != 0 || !llam_async_signal_idle(binding)) {
        llam_signal_uncertain();
    }
    /* Binding refs are zero and ordinary shared admission is closed. This M
     * cannot execute close while its signal handler/epilogue is still active. */
    for (;;) {
        int number = sigtimedwait(&reserved, NULL, &zero);
        if (number == binding->signal_number) continue;
        if (number < 0 && errno == EINTR) continue;
        if (number < 0 && errno == EAGAIN) break;
        llam_signal_uncertain();
    }
    g_llam_signal_binding = NULL;
    binding->bound = false;
    if (binding->owns_signal_stack) {
        llam_uninstall_thread_signal_stack(binding->worker);
        binding->owns_signal_stack = false;
    }
    binding->incarnation = 0U;
    if (pthread_sigmask(SIG_SETMASK, &binding->original_mask, NULL) != 0) llam_signal_uncertain();
}

static int llam_signal_bind_current(void *opaque) {
    llam_async_signal_binding_t *binding = opaque;
    llam_async_signal_result_t result;
    llam_async_signal_request_t request = {.kind = LLAM_ASYNC_SIGNAL_PROBE};
    struct sigaction action;
    sigset_t reserved;
    uint64_t deadline;
    int error;

    if (binding->bound || g_llam_signal_binding != NULL ||
        binding->worker != g_llam_tls_worker || !llam_async_signal_idle(binding) ||
        !atomic_is_lock_free(&binding->stamp) || !atomic_is_lock_free(&binding->scheduler_mask[0])) {
        errno = EBUSY;
        return -1;
    }
    binding->incarnation = atomic_load_explicit(&binding->worker->binding_incarnation,
                                                memory_order_acquire);
    if (!llam_worker_binding_live(binding->worker, binding->incarnation) ||
        sigaction(binding->signal_number, NULL, &action) != 0 || !llam_signal_action_is_ours(&action)) {
        errno = ENOTSUP;
        return -1;
    }
    sigemptyset(&reserved);
    sigaddset(&reserved, binding->signal_number);
    error = pthread_sigmask(SIG_BLOCK, &reserved, &binding->original_mask);
    if (error != 0) { errno = error; return -1; }
    if (sigismember(&binding->original_mask, binding->signal_number) != 0 ||
        binding->worker->sigaltstack_installed) {
        (void)pthread_sigmask(SIG_SETMASK, &binding->original_mask, NULL);
        errno = EBUSY;
        return -1;
    }
    if (llam_install_thread_signal_stack(binding->worker) != 0) {
        error = errno;
        (void)pthread_sigmask(SIG_SETMASK, &binding->original_mask, NULL);
        errno = error;
        return -1;
    }
    binding->owns_signal_stack = true;
    binding->errno_location = &errno; /* Force libc/TLS resolution outside handler. */
    binding->thread = pthread_self();
    binding->bound = true;
    atomic_store_explicit(&binding->scheduler_run, 0U, memory_order_relaxed);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    binding->refuse_probe = atomic_exchange_explicit(&g_llam_signal_fail_probe, 0U,
                                                     memory_order_acq_rel) != 0U;
#endif
    g_llam_signal_binding = binding;
    error = pthread_sigmask(SIG_SETMASK, &binding->original_mask, NULL);
    if (error != 0) goto failed;
    request.binding_incarnation = binding->incarnation;
    error = llam_async_signal_arm(binding, &request);
    if (error != 0) goto failed;
    error = llam_async_signal_send(binding);
    if (error != 0) {
        if (!llam_async_signal_cancel(binding)) llam_signal_uncertain();
        goto failed;
    }
    deadline = llam_now_ns() + UINT64_C(1000000000);
    while (!llam_async_signal_take_result(binding, &result)) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        if (llam_now_ns() >= deadline) {
            if (!llam_async_signal_cancel(binding)) llam_signal_uncertain();
            error = ETIMEDOUT;
            goto failed;
        }
        (void)nanosleep(&delay, NULL);
    }
    if (result.request.kind != LLAM_ASYNC_SIGNAL_PROBE ||
        result.request.binding_incarnation != binding->incarnation) llam_signal_uncertain();
    if (result.error != 0) { error = result.error; goto failed; }
    return 0;
failed:
    llam_signal_close_current(binding);
    errno = error;
    return -1;
}

static uintptr_t llam_signal_pc(const void *opaque) {
    return (uintptr_t)((const llam_async_signal_machine_t *)opaque)->pc;
}
static uintptr_t llam_signal_sp(const void *opaque) {
    return (uintptr_t)((const llam_async_signal_machine_t *)opaque)->sp;
}
static bool llam_signal_at_gate(const void *opaque) {
    uintptr_t pc = llam_signal_pc(opaque);
    return pc >= (uintptr_t)llam_signal_gate_begin && pc < (uintptr_t)llam_signal_gate_end;
}

const llam_async_native_ops_t g_llam_signal_async_ops = {
    .binding_size = sizeof(llam_async_signal_binding_t),
    .context_size = sizeof(llam_async_signal_machine_t),
    .bind_current = llam_signal_bind_current,
    .close = llam_signal_close_current,
    .pc = llam_signal_pc,
    .sp = llam_signal_sp,
    .at_gate = llam_signal_at_gate,
    .save_gate = llam_signal_save_gate,
    .signal_mailbox = true
};
#endif
