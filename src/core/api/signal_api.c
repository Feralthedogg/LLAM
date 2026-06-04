/**
 * @file src/core/api/signal_api.c
 * @brief Linux signal wait-set API.
 *
 * @details
 * Signals are process-global, so this API is intentionally opt-in and
 * conservative. LLAM exposes a runtime-owned wait set with hardened public
 * handles, but it does not install broad process-wide handlers. On Linux,
 * waiting uses sigtimedwait slices through the blocking path so managed tasks
 * park cooperatively and runtime stop can be observed. Platforms that need
 * kqueue EVFILT_SIGNAL or another backend return ENOTSUP until implemented.
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

#if LLAM_PLATFORM_POSIX
#include <signal.h>
#endif

#define LLAM_SIGNAL_PUBLIC_INITIAL_CAPACITY 16U
#define LLAM_SIGNAL_WAIT_SLICE_NS 50000000ULL
#define LLAM_SIGNAL_NO_DEADLINE UINT64_MAX

#if LLAM_PLATFORM_LINUX
#ifndef NSIG
#define LLAM_SIGNAL_MAX_SIGNO 128
#else
#define LLAM_SIGNAL_MAX_SIGNO NSIG
#endif
#define LLAM_SIGNAL_INLINE_SIGNOS 16U
#endif

struct llam_signal_set {
    llam_runtime_t *owner_runtime;
    _Atomic size_t active_ops;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    struct llam_signal_set *registry_next;
#if LLAM_PLATFORM_LINUX
    sigset_t mask;
    int signos[LLAM_SIGNAL_INLINE_SIGNOS];
    size_t signo_count;
    atomic_uint_fast64_t sequence;
#endif
};

#if LLAM_PLATFORM_LINUX
static pthread_mutex_t g_llam_signal_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_signal_set_t *g_llam_signal_registry;
static llam_public_slot_table_t g_llam_signal_public_slots;
static unsigned g_llam_signal_refs[LLAM_SIGNAL_MAX_SIGNO];

static llam_signal_set_t *llam_signal_public_handle(llam_signal_set_t *set) {
    if (set == NULL) {
        return NULL;
    }
    return (llam_signal_set_t *)llam_public_slot_encode_handle(set->public_handle_slot,
                                                               set->public_handle_generation,
                                                               LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
}

static int llam_signal_register_live_locked(llam_signal_set_t *set, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_signal_public_slots,
                                                  set,
                                                  LLAM_SIGNAL_PUBLIC_INITIAL_CAPACITY,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_SIGNAL_SET,
                                                  set->owner_runtime != NULL
                                                      ? set->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static void llam_signal_unregister_live_locked(llam_signal_set_t *set) {
    llam_signal_set_t **link = &g_llam_signal_registry;

    if (set->public_handle_slot < g_llam_signal_public_slots.count) {
        llam_public_slot_release(&g_llam_signal_public_slots,
                                 set->public_handle_slot,
                                 set,
                                 set->public_handle_generation);
    }
    while (*link != NULL) {
        if (*link == set) {
            *link = set->registry_next;
            set->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

static llam_signal_set_t *llam_signal_resolve_public_handle(const llam_signal_set_t *handle) {
    llam_signal_set_t *set;

    if (handle == NULL) {
        errno = EINVAL;
        return NULL;
    }
    pthread_mutex_lock(&g_llam_signal_registry_lock);
    set = llam_public_slot_resolve_encoded(&g_llam_signal_public_slots,
                                           (uintptr_t)handle,
                                           LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                           NULL,
                                           NULL);
    if (set == NULL) {
        errno = EINVAL;
    } else if (llam_public_active_op_try_begin(&set->active_ops) != 0) {
        set = NULL;
    }
    pthread_mutex_unlock(&g_llam_signal_registry_lock);
    return set;
}

static void llam_signal_end_public_op(llam_signal_set_t *set) {
    if (set != NULL) {
        llam_public_active_op_end(&set->active_ops);
    }
}

static int llam_signal_validate_signos(const int *signos, size_t signo_count) {
    size_t i;
    size_t j;

    if (signos == NULL || signo_count == 0U || signo_count > LLAM_SIGNAL_INLINE_SIGNOS) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < signo_count; ++i) {
        if (signos[i] <= 0 || signos[i] >= LLAM_SIGNAL_MAX_SIGNO ||
            signos[i] == SIGKILL || signos[i] == SIGSTOP) {
            errno = EINVAL;
            return -1;
        }
        for (j = i + 1U; j < signo_count; ++j) {
            if (signos[i] == signos[j]) {
                errno = EINVAL;
                return -1;
            }
        }
    }
    return 0;
}

static int llam_signal_reserve_signos_locked(const int *signos, size_t signo_count) {
    size_t i;

    for (i = 0U; i < signo_count; ++i) {
        if (g_llam_signal_refs[signos[i]] != 0U) {
            errno = EBUSY;
            return -1;
        }
    }
    for (i = 0U; i < signo_count; ++i) {
        g_llam_signal_refs[signos[i]] = 1U;
    }
    return 0;
}

static void llam_signal_release_signos_locked(const llam_signal_set_t *set) {
    size_t i;

    if (set == NULL) {
        return;
    }
    for (i = 0U; i < set->signo_count; ++i) {
        if (set->signos[i] > 0 && set->signos[i] < LLAM_SIGNAL_MAX_SIGNO) {
            g_llam_signal_refs[set->signos[i]] = 0U;
        }
    }
}

static void llam_signal_slice_timespec(uint64_t now_ns, uint64_t deadline_ns, struct timespec *out) {
    uint64_t slice_ns = LLAM_SIGNAL_WAIT_SLICE_NS;

    if (deadline_ns != LLAM_SIGNAL_NO_DEADLINE && deadline_ns > now_ns) {
        uint64_t remaining_ns = deadline_ns - now_ns;

        if (remaining_ns < slice_ns) {
            slice_ns = remaining_ns;
        }
    }
    out->tv_sec = (time_t)(slice_ns / 1000000000ULL);
    out->tv_nsec = (long)(slice_ns % 1000000000ULL);
}

typedef struct llam_signal_wait_call {
    llam_signal_set_t *set;
    uint64_t deadline_ns;
    int signo;
    int error_code;
} llam_signal_wait_call_t;

static void *llam_signal_blocking_wait_call(void *arg) {
    llam_signal_wait_call_t *call = arg;
    llam_signal_set_t *set;

    if (call == NULL || call->set == NULL) {
        return call;
    }
    set = call->set;
    call->signo = 0;
    call->error_code = 0;
    /*
     * Blocking workers are ordinary OS threads. Mask the subscribed signals in
     * the worker before sigtimedwait so they are consumed by this explicit wait
     * instead of being delivered through the process default path.
     */
    (void)pthread_sigmask(SIG_BLOCK, &set->mask, NULL);
    for (;;) {
        struct timespec timeout;
        siginfo_t info;
        uint64_t now_ns = llam_now_ns();
        int signo;

        if (set->owner_runtime != NULL &&
            (atomic_load_explicit(&set->owner_runtime->stop_requested, memory_order_acquire) ||
             !atomic_load_explicit(&set->owner_runtime->initialized, memory_order_acquire))) {
            call->error_code = ECANCELED;
            return call;
        }
        if (call->deadline_ns != LLAM_SIGNAL_NO_DEADLINE && call->deadline_ns <= now_ns) {
            call->error_code = ETIMEDOUT;
            return call;
        }
        llam_signal_slice_timespec(now_ns, call->deadline_ns, &timeout);
        memset(&info, 0, sizeof(info));
        signo = sigtimedwait(&set->mask, &info, &timeout);
        if (signo > 0) {
            call->signo = signo;
            return call;
        }
        if (errno == EAGAIN || errno == EINTR) {
            continue;
        }
        call->error_code = errno != 0 ? errno : EINVAL;
        return call;
    }
}
#endif

int llam_signal_set_create_ex(const int *signos,
                              size_t signo_count,
                              const llam_signal_opts_t *opts,
                              size_t opts_size,
                              llam_signal_set_t **out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (opts != NULL && opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_LINUX
    llam_signal_opts_t normalized;
    llam_signal_set_t *set;
    size_t copy_size;
    size_t slot = 0U;
    size_t i;
    int rc;

    memset(&normalized, 0, sizeof(normalized));
    if (opts != NULL && opts_size != 0U) {
        copy_size = opts_size < sizeof(normalized) ? opts_size : sizeof(normalized);
        memcpy(&normalized, opts, copy_size);
    }
    if (normalized.flags != 0U || llam_signal_validate_signos(signos, signo_count) != 0) {
        if (normalized.flags != 0U) {
            errno = ENOTSUP;
        }
        return -1;
    }
    set = calloc(1, sizeof(*set));
    if (set == NULL) {
        return -1;
    }
    set->owner_runtime = llam_runtime_owner_for_new_object();
    set->signo_count = signo_count;
    llam_public_active_op_init(&set->active_ops);
    atomic_init(&set->sequence, 0U);
    sigemptyset(&set->mask);
    for (i = 0U; i < signo_count; ++i) {
        set->signos[i] = signos[i];
        sigaddset(&set->mask, signos[i]);
    }
    pthread_mutex_lock(&g_llam_signal_registry_lock);
    if (llam_signal_reserve_signos_locked(signos, signo_count) != 0) {
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        free(set);
        return -1;
    }
    if (llam_signal_register_live_locked(set, &slot) != 0) {
        llam_signal_release_signos_locked(set);
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        free(set);
        return -1;
    }
    set->public_handle_slot = slot;
    set->public_handle_generation = llam_public_slot_generation(&g_llam_signal_public_slots, slot);
    set->registry_next = g_llam_signal_registry;
    g_llam_signal_registry = set;
    *out = llam_signal_public_handle(set);
    if (*out == NULL) {
        llam_signal_unregister_live_locked(set);
        llam_signal_release_signos_locked(set);
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        free(set);
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_unlock(&g_llam_signal_registry_lock);
    rc = pthread_sigmask(SIG_BLOCK, &set->mask, NULL);
    if (rc != 0) {
        int saved_errno = rc;

        (void)llam_signal_set_destroy(*out);
        errno = saved_errno;
        return -1;
    }
    return 0;
#else
    (void)signos;
    (void)signo_count;
    (void)opts;
    (void)opts_size;
    errno = ENOTSUP;
    return -1;
#endif
}

static int llam_signal_wait_impl(llam_signal_set_t *handle,
                                 uint64_t deadline_ns,
                                 llam_signal_event_t *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
#if LLAM_PLATFORM_LINUX
    llam_signal_set_t *set;
    llam_runtime_t *pinned_runtime = NULL;
    llam_signal_wait_call_t call;
    void *ignored = NULL;
    int saved_errno;

    set = llam_signal_resolve_public_handle(handle);
    if (set == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(set->owner_runtime, &pinned_runtime, EINVAL) != 0) {
        llam_signal_end_public_op(set);
        return -1;
    }
    memset(&call, 0, sizeof(call));
    call.set = set;
    call.deadline_ns = deadline_ns;
    if (llam_call_blocking_result(llam_signal_blocking_wait_call, &call, &ignored) != 0) {
        saved_errno = errno;
        if (pinned_runtime != NULL) {
            llam_runtime_end_public_op(pinned_runtime);
        }
        llam_signal_end_public_op(set);
        errno = saved_errno;
        return -1;
    }
    if (call.error_code != 0) {
        if (pinned_runtime != NULL) {
            llam_runtime_end_public_op(pinned_runtime);
        }
        llam_signal_end_public_op(set);
        errno = call.error_code;
        return -1;
    }
    out->signo = call.signo;
    out->sequence = atomic_fetch_add_explicit(&set->sequence, 1U, memory_order_relaxed) + 1U;
    if (pinned_runtime != NULL) {
        llam_runtime_end_public_op(pinned_runtime);
    }
    llam_signal_end_public_op(set);
    return 0;
#else
    (void)handle;
    (void)deadline_ns;
    errno = ENOTSUP;
    return -1;
#endif
}

int llam_signal_wait(llam_signal_set_t *set, llam_signal_event_t *out) {
    return llam_signal_wait_impl(set, LLAM_SIGNAL_NO_DEADLINE, out);
}

int llam_signal_wait_until(llam_signal_set_t *set, uint64_t deadline_ns, llam_signal_event_t *out) {
    return llam_signal_wait_impl(set, deadline_ns, out);
}

int llam_signal_set_destroy(llam_signal_set_t *handle) {
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_LINUX
    uintptr_t raw = (uintptr_t)handle;
    llam_signal_set_t *set;
    sigset_t mask;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_signal_registry_lock);
    set = llam_public_slot_resolve_encoded(&g_llam_signal_public_slots,
                                           raw,
                                           LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                           &slot,
                                           &generation);
    if (set == NULL ||
        set->public_handle_slot != slot ||
        set->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner_for_cleanup(set->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        return -1;
    }
    if (llam_public_active_op_count(&set->active_ops) != 0U) {
        pthread_mutex_unlock(&g_llam_signal_registry_lock);
        errno = EBUSY;
        return -1;
    }
    mask = set->mask;
    llam_signal_unregister_live_locked(set);
    llam_signal_release_signos_locked(set);
    pthread_mutex_unlock(&g_llam_signal_registry_lock);
    (void)pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    free(set);
    return 0;
#else
    (void)handle;
    errno = ENOTSUP;
    return -1;
#endif
}
