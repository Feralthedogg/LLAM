/**
 * @file src/core/platform/signal.c
 * @brief Process-signal coordination and native-thread alternate-stack scopes.
 *
 * @details
 * Signal actions are process-global, so compatible runtimes share one
 * reference-counted configuration. Alternate stacks remain strictly
 * native-thread-local: a valid host stack is borrowed, otherwise the entering
 * thread owns a guarded mapping until it leaves the scheduler scope.
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

#include "runtime_internal.h"

typedef void (*llam_classic_signal_handler_fn)(int);
typedef void (*llam_siginfo_signal_handler_fn)(int, siginfo_t *, void *);

#if LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD
#define LLAM_RUNTIME_HANDLES_SIGBUS 1
#else
#define LLAM_RUNTIME_HANDLES_SIGBUS 0
#endif

typedef struct llam_saved_fault_action {
    _Atomic(llam_classic_signal_handler_fn) handler;
    _Atomic(llam_siginfo_signal_handler_fn) sigaction;
    atomic_uint flags;
} llam_saved_fault_action_t;

static pthread_mutex_t g_llam_process_signal_lock =
    PTHREAD_MUTEX_INITIALIZER;
static unsigned g_llam_process_signal_refs;
static unsigned g_llam_process_signal_flags;
static int g_llam_process_preempt_signal;
static bool g_llam_process_preempt_installed;
static bool g_llam_process_fault_installed;
#if LLAM_RUNTIME_HANDLES_SIGBUS
static bool g_llam_process_bus_installed;
#endif
static struct sigaction g_llam_process_previous_preempt_action;
static struct sigaction g_llam_process_previous_segv_action;
#if LLAM_RUNTIME_HANDLES_SIGBUS
static struct sigaction g_llam_process_previous_bus_action;
#endif
static llam_saved_fault_action_t g_llam_process_previous_segv_projection;
#if LLAM_RUNTIME_HANDLES_SIGBUS
static llam_saved_fault_action_t g_llam_process_previous_bus_projection;
#endif
static _Thread_local volatile sig_atomic_t g_llam_fault_chain_active;

/** @brief Return whether the configured preemption action is still ours. */
static bool llam_process_preempt_action_is_owned(int signo) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return g_llam_process_preempt_installed &&
           signo == g_llam_process_preempt_signal;
#else
    struct sigaction current;

    if (sigaction(signo, NULL, &current) != 0) {
        return false;
    }
    return (current.sa_flags & SA_SIGINFO) == 0 &&
           current.sa_handler == llam_preempt_signal_handler;
#endif
}

/** @brief Return whether one configured guard-fault action is still ours. */
static bool llam_process_fault_action_is_owned(int signo) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return signo == SIGSEGV && g_llam_process_fault_installed;
#else
    struct sigaction current;

    if (sigaction(signo, NULL, &current) != 0) {
        return false;
    }
    return (current.sa_flags & SA_SIGINFO) != 0 &&
           current.sa_sigaction == llam_fault_signal_handler;
#endif
}

/** @brief Select the signal-safe saved-action projection for a fault signal. */
static llam_saved_fault_action_t *llam_previous_fault_projection(int signo) {
    if (signo == SIGSEGV) {
        return &g_llam_process_previous_segv_projection;
    }
#if LLAM_RUNTIME_HANDLES_SIGBUS
    if (signo == SIGBUS) {
        return &g_llam_process_previous_bus_projection;
    }
#endif
    return NULL;
}

/** @brief Publish an immutable, signal-safe projection of a host action. */
static void llam_publish_previous_fault_action(
    int signo,
    const struct sigaction *previous) {
    llam_saved_fault_action_t *projection =
        llam_previous_fault_projection(signo);

    if (projection == NULL) {
        return;
    }
    atomic_store_explicit(&projection->handler,
                          previous->sa_handler,
                          memory_order_relaxed);
    atomic_store_explicit(&projection->sigaction,
                          previous->sa_sigaction,
                          memory_order_relaxed);
    atomic_store_explicit(&projection->flags,
                          (unsigned)previous->sa_flags,
                          memory_order_release);
}

/** @brief Restore default disposition and synchronously re-raise a signal. */
static void llam_raise_with_default_action(int signo) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(signo, &action, NULL);
    (void)raise(signo);
    _exit(128 + signo);
}

/** @brief Validate caller-sized signal options before runtime publication. */
bool llam_runtime_signal_options_valid(uint32_t flags, int32_t signo) {
    if ((flags &
         ~(LLAM_RUNTIME_SIGNAL_F_PREEMPT |
           LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT)) != 0U) {
        return false;
    }
    if ((flags & LLAM_RUNTIME_SIGNAL_F_PREEMPT) == 0U || signo == 0) {
        return true;
    }
    if (signo <= 0 || signo == SIGSEGV) {
        return false;
    }
#if LLAM_RUNTIME_HANDLES_SIGBUS
    if (signo == SIGBUS) {
        return false;
    }
#endif
#if LLAM_PLATFORM_POSIX
    if (signo == SIGKILL || signo == SIGSTOP) {
        return false;
    }
#if defined(NSIG)
    if (signo >= NSIG) {
        return false;
    }
#elif defined(_NSIG)
    if (signo >= _NSIG) {
        return false;
    }
#endif
#endif
    return true;
}

bool llam_runtime_preempt_signal_is_owned(const llam_runtime_t *rt) {
    return rt != NULL && rt->preempt_signal_installed &&
           llam_process_preempt_action_is_owned(rt->preempt_signal);
}

int llam_install_process_signal_handlers(llam_runtime_t *rt) {
    struct sigaction action;
    unsigned requested_flags;
    int saved_errno;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }
    requested_flags =
        rt->signal_flags &
        (LLAM_RUNTIME_SIGNAL_F_PREEMPT |
         LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT);
    if (requested_flags == 0U) {
        return 0;
    }

    pthread_mutex_lock(&g_llam_process_signal_lock);
    if (g_llam_process_signal_refs > 0U) {
        bool incompatible =
            requested_flags != g_llam_process_signal_flags ||
            (((requested_flags & LLAM_RUNTIME_SIGNAL_F_PREEMPT) != 0U) &&
             (rt->preempt_signal != g_llam_process_preempt_signal ||
              !llam_process_preempt_action_is_owned(
                  g_llam_process_preempt_signal))) ||
            (((requested_flags & LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT) != 0U) &&
             (!llam_process_fault_action_is_owned(SIGSEGV)
#if LLAM_RUNTIME_HANDLES_SIGBUS
              || !llam_process_fault_action_is_owned(SIGBUS)
#endif
             ));

        if (incompatible || g_llam_process_signal_refs == UINT_MAX) {
            pthread_mutex_unlock(&g_llam_process_signal_lock);
            errno = incompatible ? EBUSY : EOVERFLOW;
            return -1;
        }
        g_llam_process_signal_refs += 1U;
        rt->preempt_signal_installed =
            (requested_flags & LLAM_RUNTIME_SIGNAL_F_PREEMPT) != 0U;
        rt->fault_signal_installed =
            (requested_flags & LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT) != 0U;
        pthread_mutex_unlock(&g_llam_process_signal_lock);
        return 0;
    }

    if ((requested_flags & LLAM_RUNTIME_SIGNAL_F_PREEMPT) != 0U) {
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = llam_preempt_signal_handler;
        action.sa_flags = SA_RESTART | SA_ONSTACK;
        if (sigaction(rt->preempt_signal,
                      &action,
                      &g_llam_process_previous_preempt_action) != 0) {
            saved_errno = errno;
            pthread_mutex_unlock(&g_llam_process_signal_lock);
            errno = saved_errno;
            return -1;
        }
        g_llam_process_preempt_installed = true;
    }

    if ((requested_flags & LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT) != 0U) {
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = llam_fault_signal_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        if (sigaction(SIGSEGV,
                      &action,
                      &g_llam_process_previous_segv_action) != 0) {
            saved_errno = errno;
            if (g_llam_process_preempt_installed &&
                llam_process_preempt_action_is_owned(rt->preempt_signal)) {
                (void)sigaction(rt->preempt_signal,
                                &g_llam_process_previous_preempt_action,
                                NULL);
            }
            g_llam_process_preempt_installed = false;
            pthread_mutex_unlock(&g_llam_process_signal_lock);
            errno = saved_errno;
            return -1;
        }
        llam_publish_previous_fault_action(
            SIGSEGV,
            &g_llam_process_previous_segv_action);
        g_llam_process_fault_installed = true;
#if LLAM_RUNTIME_HANDLES_SIGBUS
        if (sigaction(SIGBUS,
                      &action,
                      &g_llam_process_previous_bus_action) != 0) {
            saved_errno = errno;
            if (llam_process_fault_action_is_owned(SIGSEGV)) {
                (void)sigaction(SIGSEGV,
                                &g_llam_process_previous_segv_action,
                                NULL);
            }
            g_llam_process_fault_installed = false;
            if (g_llam_process_preempt_installed &&
                llam_process_preempt_action_is_owned(rt->preempt_signal)) {
                (void)sigaction(rt->preempt_signal,
                                &g_llam_process_previous_preempt_action,
                                NULL);
            }
            g_llam_process_preempt_installed = false;
            pthread_mutex_unlock(&g_llam_process_signal_lock);
            errno = saved_errno;
            return -1;
        }
        llam_publish_previous_fault_action(
            SIGBUS,
            &g_llam_process_previous_bus_action);
        g_llam_process_bus_installed = true;
#endif
    }

    g_llam_process_signal_flags = requested_flags;
    g_llam_process_preempt_signal = rt->preempt_signal;
    g_llam_process_signal_refs = 1U;
    rt->preempt_signal_installed =
        (requested_flags & LLAM_RUNTIME_SIGNAL_F_PREEMPT) != 0U;
    rt->fault_signal_installed =
        (requested_flags & LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT) != 0U;
    pthread_mutex_unlock(&g_llam_process_signal_lock);
    return 0;
}

void llam_restore_process_signal_handlers(llam_runtime_t *rt) {
    if (rt == NULL ||
        (!rt->fault_signal_installed &&
         !rt->preempt_signal_installed)) {
        return;
    }

    pthread_mutex_lock(&g_llam_process_signal_lock);
    if (g_llam_process_signal_refs > 0U) {
        g_llam_process_signal_refs -= 1U;
    }
    rt->fault_signal_installed = false;
    rt->preempt_signal_installed = false;

    if (g_llam_process_signal_refs == 0U) {
        if (g_llam_process_fault_installed) {
#if LLAM_RUNTIME_HANDLES_SIGBUS
            if (g_llam_process_bus_installed) {
                if (llam_process_fault_action_is_owned(SIGBUS)) {
                    (void)sigaction(SIGBUS,
                                    &g_llam_process_previous_bus_action,
                                    NULL);
                }
                g_llam_process_bus_installed = false;
            }
#endif
            if (llam_process_fault_action_is_owned(SIGSEGV)) {
                (void)sigaction(SIGSEGV,
                                &g_llam_process_previous_segv_action,
                                NULL);
            }
            g_llam_process_fault_installed = false;
        }
        if (g_llam_process_preempt_installed) {
            if (llam_process_preempt_action_is_owned(
                    g_llam_process_preempt_signal)) {
                (void)sigaction(g_llam_process_preempt_signal,
                                &g_llam_process_previous_preempt_action,
                                NULL);
            }
            g_llam_process_preempt_installed = false;
        }
        g_llam_process_signal_flags = 0U;
        g_llam_process_preempt_signal = 0;
    }
    pthread_mutex_unlock(&g_llam_process_signal_lock);
}

void llam_chain_previous_fault_signal(int signo,
                                      siginfo_t *info,
                                      void *ucontext) {
    unsigned flags;
    llam_classic_signal_handler_fn classic;
    llam_siginfo_signal_handler_fn siginfo;
    llam_saved_fault_action_t *projection =
        llam_previous_fault_projection(signo);

    if (g_llam_fault_chain_active != 0) {
        llam_raise_with_default_action(signo);
    }
    if (projection == NULL) {
        llam_raise_with_default_action(signo);
    }
    g_llam_fault_chain_active = 1;
    flags = atomic_load_explicit(
        &projection->flags, memory_order_acquire);
    classic = atomic_load_explicit(
        &projection->handler, memory_order_relaxed);
    siginfo = atomic_load_explicit(
        &projection->sigaction, memory_order_relaxed);

    if (classic == SIG_IGN) {
        g_llam_fault_chain_active = 0;
        return;
    }
    if (classic == SIG_DFL ||
        ((flags & SA_SIGINFO) != 0U &&
         (siginfo == NULL ||
          siginfo == llam_fault_signal_handler))) {
        llam_raise_with_default_action(signo);
    }
    if ((flags & SA_SIGINFO) != 0U) {
        siginfo(signo, info, ucontext);
    } else {
        classic(signo);
    }
    g_llam_fault_chain_active = 0;
}

#if !LLAM_RUNTIME_BACKEND_WINDOWS
/** @brief Return the minimum host alternate stack that LLAM can borrow. */
static size_t llam_signal_stack_minimum(void) {
#if defined(MINSIGSTKSZ)
    return (size_t)MINSIGSTKSZ;
#else
    return (size_t)SIGSTKSZ;
#endif
}
#endif

int llam_install_thread_signal_stack(
    llam_runtime_t *rt,
    llam_thread_signal_stack_t *scope) {
    if (rt == NULL || scope == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(scope, 0, sizeof(*scope));
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return 0;
#else
    stack_t current;
    stack_t stack;
    void *mapping;
    void *stack_sp;
    void *upper_guard;
    size_t page_size;
    size_t guard_bytes;
    size_t stack_size = LLAM_ALTSTACK_BYTES;
    size_t mapping_size;

    if (!rt->fault_signal_installed) {
        return 0;
    }
    memset(&current, 0, sizeof(current));
    if (sigaltstack(NULL, &current) != 0) {
        return -1;
    }
    if ((current.ss_flags & SS_DISABLE) == 0 &&
        current.ss_sp != NULL &&
        current.ss_size >= llam_signal_stack_minimum()) {
        scope->previous = current;
        scope->stack_sp = current.ss_sp;
        scope->stack_size = current.ss_size;
        scope->installed = true;
        scope->borrowed = true;
        return 0;
    }

    if (stack_size < (size_t)SIGSTKSZ) {
        stack_size = (size_t)SIGSTKSZ;
    }
    page_size = (size_t)llam_page_size();
    if (page_size == 0U || page_size > SIZE_MAX / 2U ||
        stack_size > SIZE_MAX - (page_size - 1U)) {
        errno = EOVERFLOW;
        return -1;
    }
    stack_size =
        ((stack_size + page_size - 1U) / page_size) * page_size;
    guard_bytes = page_size * 2U;
    if (stack_size > SIZE_MAX - guard_bytes) {
        errno = EOVERFLOW;
        return -1;
    }
    mapping_size = stack_size + guard_bytes;
    mapping = mmap(NULL,
                   mapping_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return -1;
    }
    stack_sp = (unsigned char *)mapping + page_size;
    upper_guard = (unsigned char *)stack_sp + stack_size;
    /*
     * Establish guards by removing rights from the original mapping.
     * NetBSD treats mmap() protections as an upper bound for later
     * mprotect() calls, so mapping as PROT_NONE and upgrading the stack
     * interior fails with EACCES there.
     */
    if (mprotect(mapping, page_size, PROT_NONE) != 0 ||
        mprotect(upper_guard, page_size, PROT_NONE) != 0) {
        int saved_errno = errno;

        (void)munmap(mapping, mapping_size);
        errno = saved_errno;
        return -1;
    }
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = stack_sp;
    stack.ss_size = stack_size;
    if (sigaltstack(&stack, &scope->previous) != 0) {
        int saved_errno = errno;

        (void)munmap(mapping, mapping_size);
        errno = saved_errno;
        return -1;
    }

    scope->mapping = mapping;
    scope->mapping_size = mapping_size;
    scope->stack_sp = stack_sp;
    scope->stack_size = stack_size;
    scope->installed = true;
    return 0;
#endif
}

#if !LLAM_RUNTIME_BACKEND_WINDOWS
/** @brief Return whether a host replacement still references our mapping. */
static bool llam_signal_stack_mapping_overlaps(
    const llam_thread_signal_stack_t *scope,
    const stack_t *current) {
    uintptr_t mapping_lo = (uintptr_t)scope->mapping;
    uintptr_t mapping_hi;
    uintptr_t current_lo = (uintptr_t)current->ss_sp;
    uintptr_t current_hi;

    if (scope->mapping == NULL ||
        scope->mapping_size > UINTPTR_MAX - mapping_lo ||
        current->ss_sp == NULL ||
        current->ss_size > UINTPTR_MAX - current_lo) {
        return false;
    }
    mapping_hi = mapping_lo + scope->mapping_size;
    current_hi = current_lo + current->ss_size;
    return current_lo < mapping_hi && mapping_lo < current_hi;
}
#endif

void llam_uninstall_thread_signal_stack(
    llam_thread_signal_stack_t *scope) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (scope != NULL) {
        memset(scope, 0, sizeof(*scope));
    }
#else
    stack_t current;
    bool current_is_owned;

    if (scope == NULL || !scope->installed) {
        return;
    }
    if (scope->borrowed) {
        memset(scope, 0, sizeof(*scope));
        return;
    }
    memset(&current, 0, sizeof(current));
    if (sigaltstack(NULL, &current) != 0) {
        return;
    }
    current_is_owned =
        (current.ss_flags & SS_DISABLE) == 0 &&
        current.ss_sp == scope->stack_sp &&
        current.ss_size == scope->stack_size;
    if (current_is_owned &&
        sigaltstack(&scope->previous, NULL) != 0) {
        return;
    }
    if (!current_is_owned &&
        (current.ss_flags & SS_DISABLE) == 0 &&
        llam_signal_stack_mapping_overlaps(scope, &current)) {
        return;
    }
    if (scope->mapping != NULL) {
        (void)munmap(scope->mapping, scope->mapping_size);
    }
    memset(scope, 0, sizeof(*scope));
#endif
}
