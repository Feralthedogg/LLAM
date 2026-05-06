/**
 * @file src/core/runtime_wake.c
 * @brief Wakeup paths that move parked tasks back to runnable queues and notify workers.
 *
 * @details
 * Wake handles abstract eventfd on Linux/POSIX and kqueue user events on Darwin.
 * Shards and I/O nodes use an atomic pending bit to coalesce wakeups; this keeps
 * repeated kicks from flooding the kernel while a worker is already awake.
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

#if defined(__APPLE__)
#include <sys/event.h>
#endif

/**
 * @brief Create a platform wake handle.
 *
 * @return File descriptor for the wake handle, or -1 with @c errno set.
 */
int llam_wake_handle_create(void) {
#if defined(__APPLE__)
    struct kevent kev;
    int fd = kqueue();

    if (fd < 0) {
        return -1;
    }
    EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0U, 0, NULL);
    if (kevent(fd, &kev, 1, NULL, 0, NULL) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
#else
    return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
}

/**
 * @brief Close a wake handle if it is valid.
 *
 * @param fd Wake handle descriptor.
 */
void llam_wake_handle_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

/**
 * @brief Wait for a wake handle with optional nanosecond timeout precision.
 *
 * @param fd         Wake handle descriptor.
 * @param timeout_ms Timeout in milliseconds, or negative for default/infinite backend wait.
 * @param timeout_ns Precise timeout in nanoseconds for platforms that support it.
 *
 * @return Backend wait result.
 */
int llam_wake_handle_wait_ns(int fd, int timeout_ms, uint64_t timeout_ns) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

#if defined(__APPLE__)
    struct kevent event;
    struct timespec ts;
    struct timespec *ts_ptr = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ULL);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
        ts_ptr = &ts;
    }
    return kevent(fd, NULL, 0, &event, 1, ts_ptr);
#else
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    (void)timeout_ns;
    return poll(&pfd, 1, timeout_ms);
#endif
}

/**
 * @brief Wait for a wake handle with millisecond timeout precision.
 *
 * @param fd         Wake handle descriptor.
 * @param timeout_ms Timeout in milliseconds.
 *
 * @return Backend wait result.
 */
int llam_wake_handle_wait(int fd, int timeout_ms) {
    uint64_t timeout_ns = timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000000ULL : 0U;

    return llam_wake_handle_wait_ns(fd, timeout_ms, timeout_ns);
}

/**
 * @brief Trigger a raw wake descriptor without touching pending bits.
 *
 * @param fd Wake descriptor.
 *
 * @return @c true when the wake was delivered.
 */
static bool llam_kick_fd_raw(int fd) {
    if (fd < 0) {
        return false;
    }

#if defined(__APPLE__)
    {
        struct kevent kev;

        EV_SET(&kev, 1, EVFILT_USER, 0U, NOTE_TRIGGER, 0, NULL);
        return kevent(fd, &kev, 1, NULL, 0, NULL) == 0;
    }
#else
    {
        uint64_t one = 1;

        for (;;) {
            ssize_t rc = write(fd, &one, sizeof(one));
            if (rc == (ssize_t)sizeof(one)) {
                return true;
            }
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
    }
#endif
}

/**
 * @brief Drain a raw wake descriptor.
 *
 * Linux/POSIX eventfd wake counts are read until empty. Darwin EVFILT_USER with
 * EV_CLEAR is drained by the kevent wait itself, so no explicit read is needed.
 *
 * @param fd Wake descriptor.
 */
static void llam_drain_fd_raw(int fd) {
    if (fd < 0) {
        return;
    }

#if !defined(__APPLE__)
    {
        uint64_t value;

        for (;;) {
            ssize_t rc = read(fd, &value, sizeof(value));
            if (rc == (ssize_t)sizeof(value)) {
                continue;
            }
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }
#else
    (void)fd;
#endif
}

#if defined(__APPLE__)
/** @brief Mach message used to wake a Darwin I/O node through a Mach port. */
typedef struct llam_mach_wake_msg {
    mach_msg_header_t header;
    uint8_t trailer[64];
} llam_mach_wake_msg_t;

/**
 * @brief Wake a Darwin I/O node through its Mach port.
 *
 * @param node Node to wake.
 *
 * @return @c true when the Mach send succeeded.
 */
static bool llam_kick_node_mach(llam_node_t *node) {
    llam_mach_wake_msg_t msg;
    kern_return_t kr;

    if (node == NULL || !node->mach_wake_enabled) {
        return false;
    }
    memset(&msg, 0, sizeof(msg));
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_remote_port = (mach_port_t)node->mach_wake_port;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_size = (mach_msg_size_t)sizeof(msg.header);
    msg.header.msgh_id = 1;
    kr = mach_msg(&msg.header,
                  MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                  msg.header.msgh_size,
                  0,
                  MACH_PORT_NULL,
                  0,
                  MACH_PORT_NULL);
    return kr == MACH_MSG_SUCCESS;
}

/**
 * @brief Drain pending Mach wake messages from a Darwin I/O node.
 *
 * @param node Node whose Mach wake port should be drained.
 */
static void llam_drain_node_mach(llam_node_t *node) {
    llam_mach_wake_msg_t msg;

    if (node == NULL || !node->mach_wake_enabled) {
        return;
    }

    for (;;) {
        kern_return_t kr;

        memset(&msg, 0, sizeof(msg));
        kr = mach_msg(&msg.header,
                      MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                      0,
                      (mach_msg_size_t)sizeof(msg),
                      (mach_port_t)node->mach_wake_port,
                      0,
                      MACH_PORT_NULL);
        if (kr == MACH_MSG_SUCCESS) {
            continue;
        }
        if (kr == MACH_RCV_TIMED_OUT) {
            break;
        }
        break;
    }
}
#endif

#if !defined(__linux__) || !defined(__x86_64__)
/**
 * @brief Claim a pending wake bit for non-assembly wake paths.
 *
 * @param pending Atomic pending flag.
 *
 * @return 1 if this caller should signal the kernel object, otherwise 0.
 */
unsigned llam_eventfd_try_claim(atomic_uint *pending) {
    unsigned expected = 0U;

    if (pending == NULL) {
        return 0U;
    }
    return atomic_compare_exchange_strong_explicit(pending,
                                                   &expected,
                                                   1U,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)
               ? 1U
               : 0U;
}

/**
 * @brief Futex wait wrapper used when the assembly fast path is unavailable.
 */
long llam_linux_futex_wait_private(atomic_uint *addr, unsigned expected) {
#if defined(__linux__)
    return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
#else
    (void)addr;
    (void)expected;
    errno = ENOSYS;
    return -1;
#endif
}

/**
 * @brief Futex timed wait wrapper used when the assembly fast path is unavailable.
 */
long llam_linux_futex_wait_private_timeout(atomic_uint *addr, unsigned expected, const struct timespec *timeout) {
#if defined(__linux__)
    return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, timeout, NULL, 0);
#else
    (void)addr;
    (void)expected;
    (void)timeout;
    errno = ENOSYS;
    return -1;
#endif
}

/**
 * @brief Futex wake wrapper used when the assembly fast path is unavailable.
 */
long llam_linux_futex_wake_private(atomic_uint *addr, unsigned count) {
#if defined(__linux__)
    return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
#else
    (void)addr;
    (void)count;
    errno = ENOSYS;
    return -1;
#endif
}
#endif

/**
 * @brief Wake a scheduler shard if no wake is already pending.
 *
 * @param shard Shard to kick.
 */
void llam_kick_shard(llam_shard_t *shard) {
    if (shard == NULL || shard->event_fd < 0) {
        return;
    }
    if (llam_eventfd_try_claim(&shard->event_pending) == 0U) {
        return;
    }
#if defined(__linux__)
    // Linux scheduler workers sleep directly on the pending word via futex.
    (void)llam_linux_futex_wake_private(&shard->event_pending, 1U);
    if (atomic_load_explicit(&shard->opaque_helper_opaque_wait, memory_order_acquire) != 0U) {
        llam_opaque_wake_signal(shard);
    }
    return;
#else
    if (!llam_kick_fd_raw(shard->event_fd)) {
        atomic_store_explicit(&shard->event_pending, 0U, memory_order_release);
    }
#endif
}

/**
 * @brief Wake an I/O node worker if no wake is already pending.
 *
 * @param node Node to kick.
 */
void llam_kick_node(llam_node_t *node) {
    bool kicked = false;

    if (node == NULL || node->event_fd < 0) {
        return;
    }
    if (llam_eventfd_try_claim(&node->event_pending) == 0U) {
        return;
    }
#if defined(__APPLE__)
    if (node->mach_wake_enabled) {
        kicked = llam_kick_node_mach(node);
    }
#endif
    if (llam_kick_fd_raw(node->event_fd)) {
        kicked = true;
    }
    if (!kicked) {
        atomic_store_explicit(&node->event_pending, 0U, memory_order_release);
    }
}

/**
 * @brief Clear a shard wake notification after an idle wait returns.
 *
 * @param shard Shard whose wake state should be drained.
 */
void llam_drain_shard_wake(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
#if defined(__linux__)
    atomic_store_explicit(&shard->event_pending, 0U, memory_order_release);
#else
    llam_drain_fd_raw(shard->event_fd);
    atomic_store_explicit(&shard->event_pending, 0U, memory_order_release);
#endif
}

/**
 * @brief Clear an I/O node wake notification after worker wakeup.
 *
 * @param node Node whose wake state should be drained.
 */
void llam_drain_node_wake(llam_node_t *node) {
    if (node == NULL) {
        return;
    }
#if defined(__APPLE__)
    if (node->mach_wake_enabled) {
        llam_drain_node_mach(node);
    }
#endif
    llam_drain_fd_raw(node->event_fd);
    atomic_store_explicit(&node->event_pending, 0U, memory_order_release);
}

/**
 * @brief Initialize optional opaque-helper wake resources for a shard.
 *
 * @param shard Shard to initialize.
 *
 * @return 0 on success, or -1 with @c errno set for invalid input.
 */
int llam_opaque_wake_init(llam_shard_t *shard) {
    if (shard == NULL) {
        errno = EINVAL;
        return -1;
    }
#if defined(__APPLE__)
    {
        const char *env = llam_env_get("LLAM_OPAQUE_MACH_SEM");

        if (env == NULL || env[0] == '\0' || strcmp(env, "0") == 0) {
            return 0;
        }
        if (semaphore_create(mach_task_self(), &shard->opaque_sem, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS) {
            shard->opaque_sem_initialized = true;
        }
    }
#endif
    return 0;
}

/**
 * @brief Destroy optional opaque-helper wake resources for a shard.
 *
 * @param shard Shard to clean up.
 */
void llam_opaque_wake_destroy(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
#if defined(__APPLE__)
    if (shard->opaque_sem_initialized) {
        (void)semaphore_destroy(mach_task_self(), shard->opaque_sem);
        shard->opaque_sem = MACH_PORT_NULL;
        shard->opaque_sem_initialized = false;
    }
#endif
}

/**
 * @brief Signal waiters on a shard's opaque-helper wake path.
 *
 * @param shard Shard whose opaque waiters should be woken.
 */
void llam_opaque_wake_signal(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
#if defined(__linux__)
    atomic_fetch_add_explicit(&shard->opaque_wake_seq, 1U, memory_order_release);
    (void)llam_linux_futex_wake_private(&shard->opaque_wake_seq, INT_MAX);
    return;
#endif
#if defined(__APPLE__)
    if (shard->opaque_sem_initialized) {
        (void)semaphore_signal_all(shard->opaque_sem);
        return;
    }
#endif
    pthread_cond_broadcast(&shard->opaque_cv);
}

/**
 * @brief Wait on a shard's opaque-helper wake path.
 *
 * The caller must hold @c shard->opaque_lock. Backends drop the lock while
 * sleeping and reacquire it before returning.
 *
 * @param shard Shard whose opaque wake path should be waited on.
 */
void llam_opaque_wake_wait(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
#if defined(__linux__)
    {
        unsigned seq = atomic_load_explicit(&shard->opaque_wake_seq, memory_order_acquire);

        pthread_mutex_unlock(&shard->opaque_lock);
        (void)llam_linux_futex_wait_private(&shard->opaque_wake_seq, seq);
        pthread_mutex_lock(&shard->opaque_lock);
        return;
    }
#endif
#if defined(__APPLE__)
    if (shard->opaque_sem_initialized) {
        kern_return_t kr;
        mach_timespec_t timeout = {
            .tv_sec = 0,
            .tv_nsec = 1000000U,
        };

        pthread_mutex_unlock(&shard->opaque_lock);
        do {
            kr = semaphore_timedwait(shard->opaque_sem, timeout);
        } while (kr == KERN_ABORTED);
        pthread_mutex_lock(&shard->opaque_lock);
        return;
    }
#endif
    (void)pthread_cond_wait(&shard->opaque_cv, &shard->opaque_lock);
}

/**
 * @brief Wake every scheduler shard in the runtime.
 *
 * @param rt Runtime whose shards should be kicked.
 */
void llam_wake_all_shards(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        llam_kick_shard(&rt->shards[i]);
    }
}

/**
 * @brief Wake every I/O node in the runtime.
 *
 * @param rt Runtime whose nodes should be kicked.
 */
void llam_wake_all_nodes(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_nodes; ++i) {
        llam_kick_node(&rt->nodes[i]);
    }
}

/**
 * @brief Request global runtime stop and wake all sleeping workers.
 *
 * @param rt Runtime to stop.
 */
void llam_request_stop(llam_runtime_t *rt) {
    atomic_store(&rt->stop_requested, true);
    if (rt->block_lock_initialized) {
        atomic_fetch_add_explicit(&rt->block_wake_seq, 1U, memory_order_release);
#if defined(__linux__)
        (void)llam_linux_futex_wake_private(&rt->block_wake_seq, INT_MAX);
#else
        if (rt->block_cv_initialized) {
            pthread_mutex_lock(&rt->block_lock);
            pthread_cond_broadcast(&rt->block_cv);
            pthread_mutex_unlock(&rt->block_lock);
        }
#endif
    }
    llam_wake_all_shards(rt);
    llam_wake_all_nodes(rt);
}

/**
 * @brief Record the first fatal runtime error and request shutdown.
 *
 * @param rt  Runtime to update.
 * @param err Positive errno value; 0 is ignored.
 */
void llam_record_fatal(llam_runtime_t *rt, int err) {
    int expected = 0;

    if (err == 0) {
        return;
    }

    if (atomic_compare_exchange_strong(&rt->fatal_errno, &expected, err)) {
        llam_request_stop(rt);
    }
}
