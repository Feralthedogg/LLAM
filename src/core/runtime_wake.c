/**
 * @file src/core/runtime_wake.c
 * @brief Wakeup paths that move parked tasks back to runnable queues and notify workers.
 *
 * @details
 * Wake handles abstract Linux eventfd/futex, kqueue user events, Windows
 * events, and a pthread-cond fallback for opaque helpers.
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

#if LLAM_PLATFORM_WINDOWS
#include "runtime_windows_iocp.h"
#endif

#if LLAM_RUNTIME_BACKEND_DARWIN
#include <dlfcn.h>
#endif

#if LLAM_RUNTIME_BACKEND_KQUEUE
#include <sys/event.h>
#endif

#if LLAM_PLATFORM_WINDOWS
#define LLAM_WINDOWS_WAKE_TABLE_CAP 4096
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static SRWLOCK g_llam_windows_wake_lock = SRWLOCK_INIT;
static HANDLE g_llam_windows_wake_handles[LLAM_WINDOWS_WAKE_TABLE_CAP];
static HANDLE g_llam_windows_wake_timers[LLAM_WINDOWS_WAKE_TABLE_CAP];

static bool llam_windows_high_res_wake_timer_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_WINDOWS_HIGH_RES_WAKE_TIMER");

        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

static HANDLE llam_windows_wake_timer_create(void) {
    HANDLE timer = NULL;

    if (!llam_windows_high_res_wake_timer_enabled()) {
        return NULL;
    }
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL) {
        timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    return timer;
}

static int llam_windows_wake_handle_alloc(HANDLE handle, HANDLE timer) {
    int fd = -1;
    unsigned i;

    AcquireSRWLockExclusive(&g_llam_windows_wake_lock);
    for (i = 1U; i < LLAM_WINDOWS_WAKE_TABLE_CAP; ++i) {
        if (g_llam_windows_wake_handles[i] == NULL) {
            g_llam_windows_wake_handles[i] = handle;
            g_llam_windows_wake_timers[i] = timer;
            fd = (int)i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_llam_windows_wake_lock);
    if (fd < 0) {
        errno = EMFILE;
    }
    return fd;
}

static HANDLE llam_windows_wake_handle_get(int fd) {
    HANDLE handle = NULL;

    if (fd <= 0 || fd >= LLAM_WINDOWS_WAKE_TABLE_CAP) {
        return NULL;
    }
    AcquireSRWLockShared(&g_llam_windows_wake_lock);
    handle = g_llam_windows_wake_handles[fd];
    ReleaseSRWLockShared(&g_llam_windows_wake_lock);
    return handle;
}

static HANDLE llam_windows_wake_timer_get(int fd) {
    HANDLE timer = NULL;

    if (fd <= 0 || fd >= LLAM_WINDOWS_WAKE_TABLE_CAP) {
        return NULL;
    }
    AcquireSRWLockShared(&g_llam_windows_wake_lock);
    timer = g_llam_windows_wake_timers[fd];
    ReleaseSRWLockShared(&g_llam_windows_wake_lock);
    return timer;
}

static void llam_windows_wake_handle_take(int fd, HANDLE *handle_out, HANDLE *timer_out) {
    HANDLE handle = NULL;
    HANDLE timer = NULL;

    if (handle_out != NULL) {
        *handle_out = NULL;
    }
    if (timer_out != NULL) {
        *timer_out = NULL;
    }
    if (fd <= 0 || fd >= LLAM_WINDOWS_WAKE_TABLE_CAP) {
        return;
    }
    AcquireSRWLockExclusive(&g_llam_windows_wake_lock);
    handle = g_llam_windows_wake_handles[fd];
    timer = g_llam_windows_wake_timers[fd];
    g_llam_windows_wake_handles[fd] = NULL;
    g_llam_windows_wake_timers[fd] = NULL;
    ReleaseSRWLockExclusive(&g_llam_windows_wake_lock);
    if (handle_out != NULL) {
        *handle_out = handle;
    }
    if (timer_out != NULL) {
        *timer_out = timer;
    }
}
#endif

/**
 * @brief Create a platform wake handle.
 *
 * @return File descriptor for the wake handle, or -1 with @c errno set.
 */
int llam_wake_handle_create(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE
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
#elif LLAM_PLATFORM_WINDOWS
    HANDLE handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE timer = NULL;
    int fd;

    if (handle == NULL) {
        errno = ENOMEM;
        return -1;
    }
    timer = llam_windows_wake_timer_create();
    fd = llam_windows_wake_handle_alloc(handle, timer);
    if (fd < 0) {
        if (timer != NULL) {
            CloseHandle(timer);
        }
        CloseHandle(handle);
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
#if LLAM_PLATFORM_WINDOWS
        HANDLE handle = NULL;
        HANDLE timer = NULL;

        llam_windows_wake_handle_take(fd, &handle, &timer);
        if (timer != NULL) {
            (void)CancelWaitableTimer(timer);
            (void)CloseHandle(timer);
        }
        if (handle != NULL) {
            (void)CloseHandle(handle);
        }
#else
        close(fd);
#endif
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

#if LLAM_RUNTIME_BACKEND_KQUEUE
    struct kevent event;
    struct timespec ts;
    struct timespec *ts_ptr = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ULL);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
        ts_ptr = &ts;
    }
    return kevent(fd, NULL, 0, &event, 1, ts_ptr);
#elif LLAM_PLATFORM_WINDOWS
    {
        HANDLE os_handle = llam_windows_wake_handle_get(fd);
        HANDLE timer = llam_windows_wake_timer_get(fd);
        DWORD wait_ms;
        DWORD rc;

        if (os_handle == NULL) {
            errno = EINVAL;
            return -1;
        }
        if (timeout_ms < 0) {
            wait_ms = INFINITE;
        } else if (timeout_ns > 0U && timer != NULL) {
            LARGE_INTEGER due_time;
            HANDLE handles[2];

            due_time.QuadPart = -((LONGLONG)((timeout_ns + 99ULL) / 100ULL));
            handles[0] = os_handle;
            handles[1] = timer;
            if (SetWaitableTimer(timer, &due_time, 0, NULL, NULL, FALSE)) {
                rc = WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
                (void)CancelWaitableTimer(timer);
                if (rc == WAIT_OBJECT_0) {
                    return 1;
                }
                if (rc == WAIT_OBJECT_0 + 1U) {
                    return 0;
                }
                errno = EINVAL;
                return -1;
            }
            wait_ms = (DWORD)((timeout_ns + 999999ULL) / 1000000ULL);
        } else if (timeout_ns > 0U) {
            uint64_t rounded_ms = (timeout_ns + 999999ULL) / 1000000ULL;

            wait_ms = rounded_ms > (uint64_t)UINT32_MAX ? UINT32_MAX : (DWORD)rounded_ms;
        } else {
            wait_ms = (DWORD)timeout_ms;
        }
        rc = WaitForSingleObject(os_handle, wait_ms);
        if (rc == WAIT_OBJECT_0) {
            return 1;
        }
        if (rc == WAIT_TIMEOUT) {
            return 0;
        }
        errno = EINVAL;
        return -1;
    }
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

#if LLAM_RUNTIME_BACKEND_KQUEUE
    {
        struct kevent kev;

        EV_SET(&kev, 1, EVFILT_USER, 0U, NOTE_TRIGGER, 0, NULL);
        return kevent(fd, &kev, 1, NULL, 0, NULL) == 0;
    }
#elif LLAM_PLATFORM_WINDOWS
    {
        HANDLE os_handle = llam_windows_wake_handle_get(fd);

        return os_handle != NULL && SetEvent(os_handle) != 0;
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
     * Linux eventfd wake counts are read until empty. EVFILT_USER with EV_CLEAR
     * is drained by the kevent wait itself, so no explicit read is needed.
 *
 * @param fd Wake descriptor.
 */
static void llam_drain_fd_raw(int fd) {
    if (fd < 0) {
        return;
    }

#if !LLAM_RUNTIME_BACKEND_KQUEUE
#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE os_handle = llam_windows_wake_handle_get(fd);

        if (os_handle != NULL) {
            (void)ResetEvent(os_handle);
        }
    }
#else
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
#endif
#else
    (void)fd;
#endif
}

int llam_wake_handle_signal(int fd) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!llam_kick_fd_raw(fd)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

void llam_wake_handle_drain(int fd) {
    if (fd < 0) {
        return;
    }
#if LLAM_RUNTIME_BACKEND_KQUEUE
    /*
     * EVFILT_USER with EV_CLEAR is consumed by a zero-timeout wait. The shard
     * and node paths normally call drain after a wait has already consumed the
     * event; the explicit poll here makes standalone doorbells safe when a
     * producer signals before the consumer reaches the kernel wait.
     */
    (void)llam_wake_handle_wait_ns(fd, 0, 0U);
#endif
    llam_drain_fd_raw(fd);
}

#if LLAM_RUNTIME_BACKEND_DARWIN
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
#if LLAM_RUNTIME_BACKEND_KQUEUE
        /*
         * EVFILT_USER wakeups are edge-like around the drain path:
         * a producer can observe event_pending just before the worker clears it
         * after consuming the kevent.  Re-triggering the user event is
         * idempotent and closes the same reset-after-producer race that Windows
         * manual-reset events need below.
         */
        (void)llam_kick_fd_raw(shard->event_fd);
#endif
#if LLAM_PLATFORM_WINDOWS
        /*
         * Manual-reset events can lose a coalesced wake if a second producer
         * observes event_pending just before the worker drains and resets the
         * event.  Re-set the OS event on Windows even when the logical pending
         * bit is already claimed; SetEvent is idempotent for the common case and
         * closes the reset-after-producer race.
         */
        (void)llam_kick_fd_raw(shard->event_fd);
#endif
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
#if LLAM_RUNTIME_BACKEND_KQUEUE
        /*
         * I/O cancellation controls must wake the kqueue worker even when a
         * previous wake bit is still visible.  Without this re-trigger, a cancel
         * can land between kevent delivery and event_pending reset and leave the
         * worker asleep with a queued control op.
         */
#if LLAM_RUNTIME_BACKEND_DARWIN
        if (node->mach_wake_enabled) {
            (void)llam_kick_node_mach(node);
        }
#endif
        (void)llam_kick_fd_raw(node->event_fd);
#endif
#if LLAM_PLATFORM_WINDOWS
        (void)llam_kick_fd_raw(node->event_fd);
        if (node->windows_iocp_handle != NULL) {
            (void)llam_windows_iocp_post(node->windows_iocp_handle, LLAM_WINDOWS_IOCP_WAKE_KEY, 0U, 0U);
        }
#endif
        return;
    }
#if LLAM_RUNTIME_BACKEND_DARWIN
    if (node->mach_wake_enabled) {
        kicked = llam_kick_node_mach(node);
    }
#endif
#if LLAM_PLATFORM_WINDOWS
    if (node->windows_iocp_handle != NULL &&
        llam_windows_iocp_post(node->windows_iocp_handle, LLAM_WINDOWS_IOCP_WAKE_KEY, 0U, 0U) == 0) {
        kicked = true;
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
#if LLAM_RUNTIME_BACKEND_DARWIN
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

        if (llam_env_flag_value(env, 1U) == 0U) {
            return 0;
        }
        if (semaphore_create(mach_task_self(), &shard->opaque_sem, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS) {
            shard->opaque_sem_initialized = true;
        }
    }
#endif
    return 0;
}

#if defined(__APPLE__)
#define LLAM_DARWIN_UL_COMPARE_AND_WAIT 1U
#define LLAM_DARWIN_ULF_WAKE_ALL 0x00000100U

typedef int (*llam_darwin_ulock_wait_fn)(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
typedef int (*llam_darwin_ulock_wake_fn)(uint32_t operation, void *addr, uint64_t wake_value);

static pthread_once_t g_llam_darwin_ulock_once = PTHREAD_ONCE_INIT;
static int g_llam_darwin_ulock_enabled = 0;
static llam_darwin_ulock_wait_fn g_llam_darwin_ulock_wait = NULL;
static llam_darwin_ulock_wake_fn g_llam_darwin_ulock_wake = NULL;

static void llam_darwin_ulock_resolve_once(void) {
    const char *env = llam_env_get("LLAM_OPAQUE_DARWIN_ULOCK");

    if (llam_env_flag_value(env, 0U) != 0U) {
        union {
            void *object;
            llam_darwin_ulock_wait_fn function;
        } wait_symbol;
        union {
            void *object;
            llam_darwin_ulock_wake_fn function;
        } wake_symbol;

        wait_symbol.object = dlsym(RTLD_DEFAULT, "__ulock_wait");
        wake_symbol.object = dlsym(RTLD_DEFAULT, "__ulock_wake");
        g_llam_darwin_ulock_wait = wait_symbol.function;
        g_llam_darwin_ulock_wake = wake_symbol.function;
        g_llam_darwin_ulock_enabled =
            g_llam_darwin_ulock_wait != NULL && g_llam_darwin_ulock_wake != NULL ? 1 : 0;
    }
}

/**
 * @brief Resolve private Darwin ulock entry points for opt-in opaque wake probes.
 *
 * @details
 * The symbols are resolved once with @c pthread_once so concurrent first use
 * cannot race on the cached function pointers.  The normal public Mach
 * semaphore path remains the default ABI-safe implementation.
 */
static bool llam_darwin_ulock_functions(llam_darwin_ulock_wait_fn *wait_out,
                                        llam_darwin_ulock_wake_fn *wake_out) {
    (void)pthread_once(&g_llam_darwin_ulock_once, llam_darwin_ulock_resolve_once);
    if (g_llam_darwin_ulock_enabled == 0) {
        return false;
    }
    if (wait_out != NULL) {
        *wait_out = g_llam_darwin_ulock_wait;
    }
    if (wake_out != NULL) {
        *wake_out = g_llam_darwin_ulock_wake;
    }
    return g_llam_darwin_ulock_wait != NULL && g_llam_darwin_ulock_wake != NULL;
}

/**
 * @brief Return whether Darwin opaque semaphores should skip timed recovery.
 *
 * The default path keeps a bounded 1ms timed wait because opaque helper wake
 * races can otherwise strand a helper on some Darwin schedules. Indefinite
 * waits remain available as an opt-in probe for profiling controlled workloads.
 */
static bool llam_opaque_mach_sem_indefinite_wait_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_MACH_SEM_INDEFINITE");

        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}
#endif

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
#if LLAM_PLATFORM_WINDOWS
    atomic_fetch_add_explicit(&shard->opaque_wake_seq, 1U, memory_order_release);
    WakeByAddressAll((PVOID)&shard->opaque_wake_seq);
    return;
#endif
#if defined(__APPLE__)
    {
        llam_darwin_ulock_wake_fn ulock_wake = NULL;

        if (llam_darwin_ulock_functions(NULL, &ulock_wake) && ulock_wake != NULL) {
            int saved_errno = errno;

            atomic_fetch_add_explicit(&shard->opaque_wake_seq, 1U, memory_order_release);
            (void)ulock_wake(LLAM_DARWIN_UL_COMPARE_AND_WAIT | LLAM_DARWIN_ULF_WAKE_ALL,
                             (void *)&shard->opaque_wake_seq,
                             0U);
            errno = saved_errno;
            return;
        }
    }
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
#if LLAM_PLATFORM_WINDOWS
    {
        unsigned seq = atomic_load_explicit(&shard->opaque_wake_seq, memory_order_acquire);

        pthread_mutex_unlock(&shard->opaque_lock);
        (void)WaitOnAddress((volatile VOID *)&shard->opaque_wake_seq, &seq, sizeof(seq), INFINITE);
        pthread_mutex_lock(&shard->opaque_lock);
        return;
    }
#endif
#if defined(__APPLE__)
    {
        llam_darwin_ulock_wait_fn ulock_wait = NULL;

        if (llam_darwin_ulock_functions(&ulock_wait, NULL) && ulock_wait != NULL) {
            unsigned seq = atomic_load_explicit(&shard->opaque_wake_seq, memory_order_acquire);
            int saved_errno = errno;

            pthread_mutex_unlock(&shard->opaque_lock);
            (void)ulock_wait(LLAM_DARWIN_UL_COMPARE_AND_WAIT,
                             (void *)&shard->opaque_wake_seq,
                             (uint64_t)seq,
                             1000U);
            pthread_mutex_lock(&shard->opaque_lock);
            errno = saved_errno;
            return;
        }
    }
    if (shard->opaque_sem_initialized) {
        kern_return_t kr;

        pthread_mutex_unlock(&shard->opaque_lock);
        if (llam_opaque_mach_sem_indefinite_wait_enabled()) {
            do {
                kr = semaphore_wait(shard->opaque_sem);
            } while (kr == KERN_ABORTED);
        } else {
            mach_timespec_t timeout = {
                .tv_sec = 0,
                .tv_nsec = 1000000U,
            };

            do {
                kr = semaphore_timedwait(shard->opaque_sem, timeout);
            } while (kr == KERN_ABORTED);
        }
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

    if (rt == NULL || rt->shards == NULL) {
        return;
    }
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

    if (rt == NULL || rt->nodes == NULL) {
        return;
    }
    for (i = 0; i < rt->active_nodes; ++i) {
        llam_kick_node(&rt->nodes[i]);
    }
}

/**
 * @brief Request stop for one runtime and wake its sleeping workers.
 *
 * @param rt Runtime to stop.
 */
void llam_request_stop(llam_runtime_t *rt) {
    atomic_store(&rt->stop_requested, true);
    /*
     * Partial-init failure paths reuse shutdown cleanup before any public task
     * can exist. Avoid scanning shard task lists there: shard locks may not have
     * been initialized yet, and there are no parked waiters to cancel.
     */
    if (atomic_load_explicit(&rt->initialized, memory_order_acquire) ||
        atomic_load_explicit(&rt->exec_started, memory_order_acquire)) {
        llam_runtime_cancel_parked_waiters(rt);
    }
    if (rt->block_lock_initialized) {
        /*
         * Keep the shutdown condition and the futex/WaitOnAddress sequence in
         * one block-lock critical section.  Otherwise a worker can read the new
         * sequence after the wake but still have entered the wait loop from an
         * older shutdown_requested=false observation, losing the only shutdown
         * wake and hanging runtime teardown.
         */
        pthread_mutex_lock(&rt->block_lock);
        atomic_fetch_add_explicit(&rt->block_wake_seq, 1U, memory_order_release);
#if defined(__linux__)
        pthread_mutex_unlock(&rt->block_lock);
        (void)llam_linux_futex_wake_private(&rt->block_wake_seq, INT_MAX);
#elif LLAM_PLATFORM_WINDOWS
        pthread_mutex_unlock(&rt->block_lock);
        WakeByAddressAll((PVOID)&rt->block_wake_seq);
#else
        if (rt->block_cv_initialized) {
            pthread_cond_broadcast(&rt->block_cv);
        }
        pthread_mutex_unlock(&rt->block_lock);
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
