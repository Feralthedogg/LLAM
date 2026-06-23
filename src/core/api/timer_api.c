/**
 * @file src/core/api/timer_api.c
 * @brief Public waitable interval timer API.
 *
 * @details
 * Timers are exposed as encoded slot+generation handles rather than raw
 * pointers, matching the rest of the hardened public object surface. The timer
 * itself is deliberately waitable instead of callback-driven: callbacks create
 * reentrancy and lifetime questions that are better handled by explicit LLAM
 * tasks waiting on the timer.
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

#define LLAM_TIMER_PUBLIC_INITIAL_CAPACITY 32U
#define LLAM_TIMER_CANCEL_POLL_NS 10000000ULL
#define LLAM_TIMER_NO_DEADLINE UINT64_MAX

struct llam_timer {
    llam_runtime_t *owner_runtime;
    _Atomic size_t active_ops;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    struct llam_timer *registry_next;
    pthread_mutex_t lock;
    uint64_t next_deadline_ns;
    uint64_t interval_ns;
    bool canceled;
};

static pthread_mutex_t g_llam_timer_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_timer_t *g_llam_timer_registry;
static llam_public_slot_table_t g_llam_timer_public_slots;

static llam_timer_t *llam_timer_public_handle(llam_timer_t *timer) {
    if (timer == NULL) {
        return NULL;
    }
    return (llam_timer_t *)llam_public_slot_encode_handle(timer->public_handle_slot,
                                                          timer->public_handle_generation,
                                                          LLAM_SYNC_PUBLIC_HANDLE_SHIFT);
}

static int llam_timer_register_live_locked(llam_timer_t *timer, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_timer_public_slots,
                                                  timer,
                                                  LLAM_TIMER_PUBLIC_INITIAL_CAPACITY,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_TIMER,
                                                  timer->owner_runtime != NULL
                                                      ? timer->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static void llam_timer_unregister_live_locked(llam_timer_t *timer) {
    llam_timer_t **link = &g_llam_timer_registry;

    if (timer->public_handle_slot < g_llam_timer_public_slots.count) {
        llam_public_slot_release(&g_llam_timer_public_slots,
                                 timer->public_handle_slot,
                                 timer,
                                 timer->public_handle_generation);
    }
    while (*link != NULL) {
        if (*link == timer) {
            *link = timer->registry_next;
            timer->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

static llam_timer_t *llam_timer_resolve_public_handle(const llam_timer_t *handle) {
    llam_timer_t *timer;

    if (handle == NULL) {
        errno = EINVAL;
        return NULL;
    }
    pthread_mutex_lock(&g_llam_timer_registry_lock);
    timer = llam_public_slot_resolve_encoded(&g_llam_timer_public_slots,
                                             (uintptr_t)handle,
                                             LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                             NULL,
                                             NULL);
    if (timer == NULL) {
        errno = EINVAL;
    } else if (llam_public_active_op_try_begin(&timer->active_ops) != 0) {
        timer = NULL;
    }
    pthread_mutex_unlock(&g_llam_timer_registry_lock);
    return timer;
}

static void llam_timer_end_public_op(llam_timer_t *timer) {
    if (timer != NULL) {
        llam_public_active_op_end(&timer->active_ops);
    }
}

static uint64_t llam_timer_saturating_add(uint64_t base, uint64_t delta) {
    return delta > UINT64_MAX - base ? UINT64_MAX : base + delta;
}

static uint64_t llam_timer_next_from_interval(uint64_t interval_ns) {
    return llam_timer_saturating_add(llam_now_ns(), interval_ns);
}

static int llam_timer_normalize_opts(const llam_timer_opts_t *opts,
                                     size_t opts_size,
                                     llam_timer_opts_t *out) {
    size_t copy_size;

    if (opts == NULL || out == NULL || opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    copy_size = opts_size < sizeof(*out) ? opts_size : sizeof(*out);
    memcpy(out, opts, copy_size);
    if (out->interval_ns == 0U || out->flags != 0U) {
        errno = out->interval_ns == 0U ? EINVAL : ENOTSUP;
        return -1;
    }
    if (out->first_deadline_ns == 0U) {
        out->first_deadline_ns = llam_now_ns();
    }
    return 0;
}

int llam_timer_create_ex(const llam_timer_opts_t *opts, size_t opts_size, llam_timer_t **out) {
    llam_timer_opts_t normalized;
    llam_timer_t *timer;
    size_t slot = 0U;
    int rc;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (llam_timer_normalize_opts(opts, opts_size, &normalized) != 0) {
        return -1;
    }
    timer = calloc(1, sizeof(*timer));
    if (timer == NULL) {
        return -1;
    }
    timer->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&timer->active_ops);
    timer->next_deadline_ns = normalized.first_deadline_ns;
    timer->interval_ns = normalized.interval_ns;
    rc = pthread_mutex_init(&timer->lock, NULL);
    if (rc != 0) {
        free(timer);
        errno = rc;
        return -1;
    }
    pthread_mutex_lock(&g_llam_timer_registry_lock);
    if (llam_timer_register_live_locked(timer, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_timer_registry_lock);
        pthread_mutex_destroy(&timer->lock);
        free(timer);
        return -1;
    }
    timer->public_handle_slot = slot;
    timer->public_handle_generation = llam_public_slot_generation(&g_llam_timer_public_slots, slot);
    timer->registry_next = g_llam_timer_registry;
    g_llam_timer_registry = timer;
    *out = llam_timer_public_handle(timer);
    if (*out == NULL) {
        llam_timer_unregister_live_locked(timer);
        pthread_mutex_unlock(&g_llam_timer_registry_lock);
        pthread_mutex_destroy(&timer->lock);
        free(timer);
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_unlock(&g_llam_timer_registry_lock);
    return 0;
}

int llam_timer_create(uint64_t interval_ns, llam_timer_t **out) {
    llam_timer_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.interval_ns = interval_ns;
    opts.first_deadline_ns = interval_ns != 0U ? llam_timer_next_from_interval(interval_ns) : 0U;
    return llam_timer_create_ex(&opts, LLAM_TIMER_OPTS_CURRENT_SIZE, out);
}

static int llam_timer_advance_if_ready_locked(llam_timer_t *timer,
                                              uint64_t now_ns,
                                              uint64_t *ticks_out) {
    uint64_t ticks;
    uint64_t max_ticks;

    if (timer->next_deadline_ns > now_ns) {
        return 0;
    }
    ticks = ((now_ns - timer->next_deadline_ns) / timer->interval_ns) + 1U;
    max_ticks = (UINT64_MAX - timer->next_deadline_ns) / timer->interval_ns;
    if (ticks > max_ticks) {
        timer->next_deadline_ns = UINT64_MAX;
    } else {
        timer->next_deadline_ns += ticks * timer->interval_ns;
    }
    *ticks_out = ticks;
    return 1;
}

static uint64_t llam_timer_wait_slice_deadline(uint64_t now_ns,
                                               uint64_t timer_deadline_ns,
                                               uint64_t caller_deadline_ns) {
    uint64_t slice_deadline = llam_timer_saturating_add(now_ns, LLAM_TIMER_CANCEL_POLL_NS);

    if (timer_deadline_ns < slice_deadline) {
        slice_deadline = timer_deadline_ns;
    }
    if (caller_deadline_ns < slice_deadline) {
        slice_deadline = caller_deadline_ns;
    }
    return slice_deadline;
}

static int llam_timer_wait_impl(llam_timer_t *handle, uint64_t caller_deadline_ns, uint64_t *ticks_out) {
    llam_timer_t *timer;
    llam_runtime_t *pinned_runtime = NULL;

    if (ticks_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *ticks_out = 0U;
    if (caller_deadline_ns == 0U) {
        errno = ETIMEDOUT;
        return -1;
    }
    timer = llam_timer_resolve_public_handle(handle);
    if (timer == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(timer->owner_runtime, &pinned_runtime, EINVAL) != 0) {
        llam_timer_end_public_op(timer);
        return -1;
    }
    for (;;) {
        uint64_t now_ns = llam_now_ns();
        uint64_t next_deadline_ns;
        uint64_t slice_deadline_ns;
        int ready;

        pthread_mutex_lock(&timer->lock);
        if (timer->canceled) {
            pthread_mutex_unlock(&timer->lock);
            if (pinned_runtime != NULL) {
                llam_runtime_end_public_op(pinned_runtime);
            }
            llam_timer_end_public_op(timer);
            errno = ECANCELED;
            return -1;
        }
        ready = llam_timer_advance_if_ready_locked(timer, now_ns, ticks_out);
        next_deadline_ns = timer->next_deadline_ns;
        pthread_mutex_unlock(&timer->lock);
        if (ready != 0) {
            if (pinned_runtime != NULL) {
                llam_runtime_end_public_op(pinned_runtime);
            }
            llam_timer_end_public_op(timer);
            return 0;
        }
        if (caller_deadline_ns != LLAM_TIMER_NO_DEADLINE && caller_deadline_ns <= now_ns) {
            if (pinned_runtime != NULL) {
                llam_runtime_end_public_op(pinned_runtime);
            }
            llam_timer_end_public_op(timer);
            errno = ETIMEDOUT;
            return -1;
        }
        slice_deadline_ns = llam_timer_wait_slice_deadline(now_ns, next_deadline_ns, caller_deadline_ns);
        if (llam_sleep_until(slice_deadline_ns) != 0) {
            int saved_errno = errno;

            if (pinned_runtime != NULL) {
                llam_runtime_end_public_op(pinned_runtime);
            }
            llam_timer_end_public_op(timer);
            errno = saved_errno;
            return -1;
        }
    }
}

int llam_timer_wait(llam_timer_t *timer, uint64_t *ticks_out) {
    return llam_timer_wait_impl(timer, LLAM_TIMER_NO_DEADLINE, ticks_out);
}

int llam_timer_wait_until(llam_timer_t *timer, uint64_t deadline_ns, uint64_t *ticks_out) {
    return llam_timer_wait_impl(timer, deadline_ns, ticks_out);
}

int llam_timer_reset(llam_timer_t *handle, uint64_t first_deadline_ns, uint64_t interval_ns) {
    llam_timer_t *timer;
    llam_runtime_t *pinned_runtime = NULL;

    if (interval_ns == 0U) {
        errno = EINVAL;
        return -1;
    }
    timer = llam_timer_resolve_public_handle(handle);
    if (timer == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(timer->owner_runtime, &pinned_runtime, EINVAL) != 0) {
        llam_timer_end_public_op(timer);
        return -1;
    }
    pthread_mutex_lock(&timer->lock);
    timer->canceled = false;
    timer->interval_ns = interval_ns;
    timer->next_deadline_ns = first_deadline_ns != 0U ? first_deadline_ns : llam_now_ns();
    pthread_mutex_unlock(&timer->lock);
    if (pinned_runtime != NULL) {
        llam_runtime_end_public_op(pinned_runtime);
    }
    llam_timer_end_public_op(timer);
    return 0;
}

int llam_timer_cancel(llam_timer_t *handle) {
    llam_timer_t *timer;
    llam_runtime_t *pinned_runtime = NULL;

    timer = llam_timer_resolve_public_handle(handle);
    if (timer == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(timer->owner_runtime, &pinned_runtime, EINVAL) != 0) {
        llam_timer_end_public_op(timer);
        return -1;
    }
    pthread_mutex_lock(&timer->lock);
    timer->canceled = true;
    pthread_mutex_unlock(&timer->lock);
    if (pinned_runtime != NULL) {
        llam_runtime_end_public_op(pinned_runtime);
    }
    llam_timer_end_public_op(timer);
    return 0;
}

int llam_timer_destroy(llam_timer_t *handle) {
    uintptr_t raw = (uintptr_t)handle;
    llam_timer_t *timer;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_timer_registry_lock);
    timer = llam_public_slot_resolve_encoded(&g_llam_timer_public_slots,
                                             raw,
                                             LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                             &slot,
                                             &generation);
    if (timer == NULL ||
        timer->public_handle_slot != slot ||
        timer->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_timer_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner_for_cleanup(timer->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_timer_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&timer->lock);
    if (llam_public_active_op_count(&timer->active_ops) != 0U) {
        pthread_mutex_unlock(&timer->lock);
        pthread_mutex_unlock(&g_llam_timer_registry_lock);
        errno = EBUSY;
        return -1;
    }
    llam_timer_unregister_live_locked(timer);
    pthread_mutex_unlock(&timer->lock);
    pthread_mutex_unlock(&g_llam_timer_registry_lock);
    pthread_mutex_destroy(&timer->lock);
    free(timer);
    return 0;
}
