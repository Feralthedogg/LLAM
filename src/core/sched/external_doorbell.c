/**
 * @file src/core/sched/external_doorbell.c
 * @brief Portable coalesced readiness notification for external host loops.
 *
 * @details
 * The runtime owns one native readiness object for its entire lifetime:
 * Linux uses a nonblocking close-on-exec eventfd, Darwin and BSD use a
 * nonblocking close-on-exec pipe, and Windows uses a manual-reset event.
 * Producers claim one atomic pending bit before touching the native object.
 * A driver drains the object, clears the bit, and then re-checks scheduler
 * state so a producer that raced with reset cannot be lost.
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

#if !LLAM_PLATFORM_WINDOWS && !LLAM_RUNTIME_BACKEND_LINUX
/** @brief Add nonblocking and close-on-exec flags to one pipe endpoint. */
static int llam_external_doorbell_configure_fd(int fd) {
    int status_flags;
    int descriptor_flags;

    status_flags = fcntl(fd, F_GETFL);
    if (status_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
        return -1;
    }
    descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return -1;
    }
    return 0;
}
#endif

int llam_external_doorbell_init(llam_external_doorbell_t *doorbell) {
    if (doorbell == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (doorbell->initialized) {
        errno = EBUSY;
        return -1;
    }

    memset(doorbell, 0, sizeof(*doorbell));
    atomic_init(&doorbell->pending, 0U);
#if LLAM_PLATFORM_WINDOWS
    doorbell->handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (doorbell->handle == NULL) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
#else
    doorbell->read_fd = -1;
    doorbell->write_fd = -1;
#if LLAM_RUNTIME_BACKEND_LINUX
    doorbell->read_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (doorbell->read_fd < 0) {
        return -1;
    }
    doorbell->write_fd = doorbell->read_fd;
#else
    {
        int fds[2] = {-1, -1};

        if (pipe(fds) != 0) {
            return -1;
        }
        if (llam_external_doorbell_configure_fd(fds[0]) != 0 ||
            llam_external_doorbell_configure_fd(fds[1]) != 0) {
            int saved_errno = errno;

            close(fds[0]);
            close(fds[1]);
            errno = saved_errno;
            return -1;
        }
        doorbell->read_fd = fds[0];
        doorbell->write_fd = fds[1];
    }
#endif
#endif
    doorbell->initialized = true;
    return 0;
}

void llam_external_doorbell_destroy(llam_external_doorbell_t *doorbell) {
    int saved_errno = errno;

    if (doorbell == NULL || !doorbell->initialized) {
        return;
    }
    doorbell->initialized = false;
    atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE handle = (HANDLE)doorbell->handle;

        doorbell->handle = NULL;
        if (handle != NULL) {
            (void)CloseHandle(handle);
        }
    }
#else
    {
        int read_fd = doorbell->read_fd;
        int write_fd = doorbell->write_fd;

        doorbell->read_fd = -1;
        doorbell->write_fd = -1;
        if (read_fd >= 0) {
            (void)close(read_fd);
        }
        if (write_fd >= 0 && write_fd != read_fd) {
            (void)close(write_fd);
        }
    }
#endif
    errno = saved_errno;
}

int llam_external_doorbell_signal(llam_external_doorbell_t *doorbell) {
    unsigned state;
    int saved_errno = errno;
    int error_code = 0;

    if (doorbell == NULL || !doorbell->initialized) {
        errno = EINVAL;
        return -1;
    }
    state = atomic_load_explicit(&doorbell->pending,
                                 memory_order_acquire);
    for (;;) {
        if (state == 1U) {
            errno = saved_errno;
            return 0;
        }
        /*
         * State 2 means a consumer is resetting native readiness. A producer
         * must claim 2 -> 1 and publish a fresh native token so a reset that
         * crosses this signal can be detected and repaired by the consumer.
         */
        if (atomic_compare_exchange_weak_explicit(&doorbell->pending,
                                                  &state,
                                                  1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

#if LLAM_PLATFORM_WINDOWS
    if (!SetEvent((HANDLE)doorbell->handle)) {
        error_code = llam_windows_system_error_to_errno(GetLastError());
    }
#else
    for (;;) {
#if LLAM_RUNTIME_BACKEND_LINUX
        uint64_t one = 1U;
        ssize_t rc = write(doorbell->write_fd, &one, sizeof(one));

        if (rc == (ssize_t)sizeof(one)) {
            break;
        }
#else
        unsigned char one = 1U;
        ssize_t rc = write(doorbell->write_fd, &one, sizeof(one));

        if (rc == (ssize_t)sizeof(one)) {
            break;
        }
#endif
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            errno = saved_errno;
            return 0;
        }
        error_code = rc < 0 && errno != 0 ? errno : EIO;
        break;
    }
#endif

    if (error_code != 0) {
        atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
        errno = error_code;
        return -1;
    }
    errno = saved_errno;
    return 0;
}

void llam_external_doorbell_drain(llam_external_doorbell_t *doorbell) {
    int saved_errno = errno;
    unsigned reset_state;

    if (doorbell == NULL || !doorbell->initialized) {
        return;
    }
    /*
     * State 2 closes reset-after-producer races. A producer that overlaps the
     * native drain changes it to 1 and publishes a token; after draining, the
     * consumer observes that claim and re-signals from a clean state.
     */
    atomic_exchange_explicit(&doorbell->pending, 2U, memory_order_acq_rel);
#if LLAM_PLATFORM_WINDOWS
    (void)ResetEvent((HANDLE)doorbell->handle);
#elif LLAM_RUNTIME_BACKEND_LINUX
    for (;;) {
        uint64_t value;
        ssize_t rc = read(doorbell->read_fd, &value, sizeof(value));

        if (rc == (ssize_t)sizeof(value)) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
#else
    for (;;) {
        unsigned char values[64];
        ssize_t rc = read(doorbell->read_fd, values, sizeof(values));

        if (rc > 0) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
#endif
    reset_state =
        atomic_exchange_explicit(&doorbell->pending,
                                 0U,
                                 memory_order_acq_rel);
    if (reset_state == 1U) {
        /*
         * Native signaling failure clears pending again. The runtime-level
         * readiness recheck remains the scheduler-state recovery path.
         */
        (void)llam_external_doorbell_signal(doorbell);
    }
    errno = saved_errno;
}

/**
 * @brief Return whether an external driver should remain immediately runnable.
 *
 * @note Called outside shard locks after a bounded scheduler quantum.
 */
static bool llam_external_runtime_ready(llam_runtime_t *rt) {
    uint64_t now_ns;

    if (rt == NULL) {
        return false;
    }
    if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&rt->fatal_errno, memory_order_acquire) != 0 ||
        atomic_load_explicit(&rt->live_tasks, memory_order_acquire) == 0U ||
        atomic_load_explicit(&rt->overflow_depth, memory_order_acquire) != 0U) {
        return true;
    }
    if (rt->shards == NULL) {
        return false;
    }

    now_ns = llam_now_ns();
    for (unsigned i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];
        bool ready;

        if (atomic_load_explicit(&shard->inject_depth,
                                 memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->norm_depth,
                                 memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->current,
                                 memory_order_acquire) != NULL) {
            return true;
        }
        if (!shard->lock_initialized) {
            continue;
        }
        pthread_mutex_lock(&shard->lock);
        ready = shard->inject_q.depth != 0U ||
                shard->hot_q.depth != 0U ||
                llam_norm_queue_depth(shard) != 0U ||
                (shard->timer_heap_len != 0U &&
                 shard->timer_heap != NULL &&
                 shard->timer_heap[0] != NULL &&
                 shard->timer_heap[0]->deadline_ns <= now_ns);
        pthread_mutex_unlock(&shard->lock);
        if (ready) {
            return true;
        }
    }
    return false;
}

void llam_external_doorbell_rearm(llam_runtime_t *rt) {
    int saved_errno = errno;

    if (rt == NULL || !rt->external_driver.doorbell.initialized ||
        !llam_external_runtime_ready(rt)) {
        errno = saved_errno;
        return;
    }
    if (llam_external_doorbell_signal(&rt->external_driver.doorbell) != 0) {
        int signal_errno = errno != 0 ? errno : EIO;

        llam_record_fatal_deferred(rt, signal_errno);
    }
    errno = saved_errno;
}
