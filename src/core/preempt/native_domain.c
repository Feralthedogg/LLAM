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
#include "llam/managed.h"
#include <limits.h>

#if !LLAM_PLATFORM_WINDOWS
#include <sys/syscall.h>
#endif

#define LLAM_NATIVE_DOMAIN_SLOTS 4096U
#define LLAM_NATIVE_DOMAIN_PROBES 64U
#define LLAM_NATIVE_DOMAIN_DEPTH_MAX 65535U
#define LLAM_NATIVE_DOMAIN_RESERVED UINT64_MAX

#if LLAM_PLATFORM_WINDOWS || \
    ((defined(__aarch64__) || defined(__x86_64__)) && (defined(__linux__) || defined(__APPLE__))) || \
    (defined(__x86_64__) && (defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)))
#define LLAM_NATIVE_DOMAIN_AVAILABLE 1
#else
#define LLAM_NATIVE_DOMAIN_AVAILABLE 0
#endif

typedef struct llam_native_domain_slot {
    _Atomic(uint64_t) native_id;
    _Atomic(llam_worker_t *) worker;
} llam_native_domain_slot_t;

static llam_native_domain_slot_t g_native_domain_slots[LLAM_NATIVE_DOMAIN_SLOTS];
atomic_uint g_llam_native_domains;

#if defined(_MSC_VER) && !defined(__clang__)
/* MSVC reports conditional support; binding probes the actual guard objects. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE >= 1 && ATOMIC_POINTER_LOCK_FREE >= 1 &&
               ATOMIC_INT_LOCK_FREE >= 1, "native guards need lock-free atomic support");
#else
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2 &&
               ATOMIC_INT_LOCK_FREE == 2, "native guards need lock-free atomics");
#endif
_Static_assert(LLAM_NATIVE_DOMAIN_SLOTS * (LLAM_NATIVE_DOMAIN_DEPTH_MAX + 1U) < UINT_MAX,
               "global guard count must not wrap, including in-flight entries");

/* No libc TLS or pthread/loader helper in a guard. These kernel operations have
 * no failure path for a live calling thread. An unsupported architecture cannot
 * acquire a managed native binding; ordinary legacy ports retain their path. */
static uint64_t llam_native_domain_id(void) {
#if LLAM_PLATFORM_WINDOWS
    return (uint64_t)GetCurrentThreadId();
#elif defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
    register uint64_t result __asm__("x0");
#if defined(__APPLE__)
    register uint64_t number __asm__("x16") = SYS_thread_selfid;
    __asm__ volatile("svc #0x80" : "=r"(result), "+r"(number) : : "x1", "memory", "cc");
#else
    register uint64_t number __asm__("x8") = SYS_gettid;
    __asm__ volatile("svc #0" : "=r"(result), "+r"(number) : : "memory", "cc");
#endif
    return result;
#elif defined(__x86_64__)
    uint64_t number;
    uint64_t result;
#if defined(__APPLE__)
    number = UINT64_C(0x2000000) | SYS_thread_selfid;
#elif defined(__linux__)
    number = SYS_gettid;
#elif defined(__FreeBSD__)
    long native_id = 0;
    uintptr_t argument = (uintptr_t)&native_id;
    number = SYS_thr_self;
    /* FreeBSD's fast syscall return zeros r8/r9/r10. Model the complete
     * caller-saved set so inlined guards cannot retain M/slot pointers there. */
    __asm__ volatile("syscall" : "=a"(result), "+D"(argument) : "a"(number)
                     : "rcx", "r11", "rsi", "rdx", "r8", "r9", "r10", "memory", "cc");
    return result == 0U && native_id > 0 ? (uint64_t)native_id : 0U;
#elif defined(__NetBSD__)
    number = SYS__lwp_self;
#elif defined(__OpenBSD__)
    number = SYS_getthrid;
#elif defined(__DragonFly__)
    number = SYS_lwp_gettid;
#else
    return 0U;
#endif
#if defined(__linux__)
    __asm__ volatile("syscall" : "=a"(result) : "a"(number) : "rcx", "r11", "memory", "cc");
#else
    __asm__ volatile("syscall" : "=a"(result) : "a"(number)
                     : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory", "cc");
#endif
    return result;
#else
    return 0U;
#endif
}

static unsigned llam_native_domain_hash(uint64_t native_id) {
    return (unsigned)((native_id * UINT64_C(0x9e3779b97f4a7c15)) >> 52U);
}

static llam_worker_t *llam_native_domain_lookup(uint64_t native_id) {
    if (native_id == 0U || native_id == LLAM_NATIVE_DOMAIN_RESERVED) return NULL;
    unsigned first = llam_native_domain_hash(native_id);
    for (unsigned i = 0U; i < LLAM_NATIVE_DOMAIN_PROBES; ++i) {
        llam_native_domain_slot_t *slot = &g_native_domain_slots[(first + i) & (LLAM_NATIVE_DOMAIN_SLOTS - 1U)];
        if (atomic_load_explicit(&slot->native_id, memory_order_acquire) == native_id) {
            return atomic_load_explicit(&slot->worker, memory_order_relaxed);
        }
        /* Empty buckets cannot terminate a lookup after same-thread retirement. */
    }
    return NULL;
}

void llam_native_domain_worker_init(llam_worker_t *worker) {
    atomic_init(&worker->native_domain_depth, 0U);
    worker->native_domain_slot = 0U;
}

int llam_native_domain_bind(llam_worker_t *worker) {
    uint64_t native_id = llam_native_domain_id();
    if (!LLAM_NATIVE_DOMAIN_AVAILABLE) return 0; /* Legacy-only port. */
#if defined(_MSC_VER) && !defined(__clang__)
    /* Probe before a slot can publish this M to signal/native guards. Array
     * elements share the same atomic types and natural alignment. */
    if (!atomic_is_lock_free(&g_native_domain_slots[0].native_id) ||
        !atomic_is_lock_free(&g_native_domain_slots[0].worker) ||
        !atomic_is_lock_free(&g_llam_native_domains) ||
        !atomic_is_lock_free(&worker->native_domain_depth)) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    if (native_id == 0U || native_id > INT64_MAX) {
        errno = ENOTSUP;
        return -1;
    }
#if defined(__APPLE__)
    uint64_t expected_id = 0U;
    if (pthread_threadid_np(NULL, &expected_id) != 0 || expected_id != native_id) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    llam_worker_t *existing = llam_native_domain_lookup(native_id);
    if (existing != NULL) {
        if (existing == worker && worker->native_domain_slot != 0U) return 0;
        errno = EBUSY;
        return -1;
    }
    unsigned first = llam_native_domain_hash(native_id);
    for (unsigned i = 0U; i < LLAM_NATIVE_DOMAIN_PROBES; ++i) {
        unsigned index = (first + i) & (LLAM_NATIVE_DOMAIN_SLOTS - 1U);
        llam_native_domain_slot_t *slot = &g_native_domain_slots[index];
        uint64_t empty = 0U;
        if (!atomic_compare_exchange_strong_explicit(&slot->native_id, &empty,
                LLAM_NATIVE_DOMAIN_RESERVED, memory_order_acq_rel, memory_order_acquire)) continue;
        worker->native_domain_slot = index + 1U;
        atomic_store_explicit(&slot->worker, worker, memory_order_relaxed);
        /* The kernel TID identifies exactly one live native thread. Its signal
         * callbacks finish before that same thread can withdraw/free this M. */
        atomic_store_explicit(&slot->native_id, native_id, memory_order_release);
        return 0;
    }
    errno = EAGAIN;
    return -1;
}

void llam_native_domain_unbind(llam_worker_t *worker) {
    if (worker == NULL || worker->native_domain_slot == 0U) return;
    unsigned index = worker->native_domain_slot - 1U;
    if (index >= LLAM_NATIVE_DOMAIN_SLOTS ||
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U) abort();
    llam_native_domain_slot_t *slot = &g_native_domain_slots[index];
    if (atomic_load_explicit(&slot->native_id, memory_order_acquire) != llam_native_domain_id() ||
        atomic_load_explicit(&slot->worker, memory_order_relaxed) != worker) abort();
    /* Caller cleared scheduler TLS and holds no P/registry lock. A
     * callback after withdrawal therefore cannot inherit scheduler authority. */
    atomic_store_explicit(&slot->worker, NULL, memory_order_relaxed);
    atomic_store_explicit(&slot->native_id, 0U, memory_order_release);
    worker->native_domain_slot = 0U;
}

llam_worker_t *llam_native_domain_guarded_worker(void) {
    llam_worker_t *worker = llam_native_domain_lookup(llam_native_domain_id());
    return worker != NULL &&
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U ? worker : NULL;
}

void llam_managed_native_enter(void) {
    llam_worker_t *worker = llam_native_domain_lookup(llam_native_domain_id());
    if (worker == NULL) return;
    unsigned depth = atomic_load_explicit(&worker->native_domain_depth, memory_order_relaxed);
    if (depth >= LLAM_NATIVE_DOMAIN_DEPTH_MAX) _Exit(125);
    /* Publish the global slow-path hint before depth. Only this native thread
     * and balanced nested signals write its depth, so it cannot change between
     * this load and RMW except for a nested guard that also returns to this depth. */
    atomic_fetch_add_explicit(&g_llam_native_domains, 1U, memory_order_acq_rel);
    atomic_fetch_add_explicit(&worker->native_domain_depth, 1U, memory_order_release);
}

void llam_managed_native_leave(void) {
    llam_worker_t *worker = llam_native_domain_lookup(llam_native_domain_id());
    if (worker == NULL) return;
    unsigned depth = atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire);
    if (depth == 0U) _Exit(125);
    atomic_fetch_sub_explicit(&worker->native_domain_depth, 1U, memory_order_acq_rel);
    atomic_fetch_sub_explicit(&g_llam_native_domains, 1U, memory_order_release);
}

#if defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))
/* Keep admission identity local even when a host interposes public symbols or
 * loads a second runtime copy. The guard slot must use this registry instance. */
void llam_native_domain_enter_identity(void)
    __attribute__((alias("llam_managed_native_enter"), visibility("hidden")));
void llam_native_domain_leave_identity(void)
    __attribute__((alias("llam_managed_native_leave"), visibility("hidden")));
#else
#define llam_native_domain_enter_identity llam_managed_native_enter
#define llam_native_domain_leave_identity llam_managed_native_leave
#endif

void llam_native_domain_guard_identities(uintptr_t out[2]) {
    void (*enter)(void) = llam_native_domain_enter_identity;
    void (*leave)(void) = llam_native_domain_leave_identity;
    _Static_assert(sizeof(enter) == sizeof(out[0]), "compiler ABI function pointer width");
    memcpy(&out[0], &enter, sizeof(enter));
    memcpy(&out[1], &leave, sizeof(leave));
}
