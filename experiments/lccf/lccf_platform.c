// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "lccf_platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct lccf_platform_thread {
    HANDLE handle;
    lccf_platform_thread_fn fn;
    void *context;
    int result;
};

struct lccf_platform_event {
    SRWLOCK lock;
    CONDITION_VARIABLE condition;
    uint64_t epoch;
};

static DWORD WINAPI thread_entry(void *opaque) {
    lccf_platform_thread_t *thread = opaque;

    thread->result = thread->fn(thread->context);
    return 0U;
}

int lccf_platform_thread_start(lccf_platform_thread_t **out,
                               lccf_platform_thread_fn fn,
                               void *context) {
    lccf_platform_thread_t *thread;

    if (out == NULL || fn == NULL) {
        return EINVAL;
    }
    *out = NULL;
    thread = calloc(1U, sizeof(*thread));
    if (thread == NULL) {
        return ENOMEM;
    }
    thread->fn = fn;
    thread->context = context;
    thread->handle =
        CreateThread(NULL, 0U, thread_entry, thread, 0U, NULL);
    if (thread->handle == NULL) {
        free(thread);
        return EAGAIN;
    }
    *out = thread;
    return 0;
}

int lccf_platform_thread_join(lccf_platform_thread_t *thread,
                              int *out_result) {
    if (thread == NULL) {
        return EINVAL;
    }
    if (WaitForSingleObject(thread->handle, INFINITE) !=
        WAIT_OBJECT_0) {
        return EIO;
    }
    if (out_result != NULL) {
        *out_result = thread->result;
    }
    (void)CloseHandle(thread->handle);
    free(thread);
    return 0;
}

int lccf_platform_event_create(lccf_platform_event_t **out) {
    lccf_platform_event_t *event;

    if (out == NULL) {
        return EINVAL;
    }
    *out = NULL;
    event = calloc(1U, sizeof(*event));
    if (event == NULL) {
        return ENOMEM;
    }
    InitializeSRWLock(&event->lock);
    InitializeConditionVariable(&event->condition);
    *out = event;
    return 0;
}

void lccf_platform_event_destroy(lccf_platform_event_t *event) {
    free(event);
}

int lccf_platform_event_signal(lccf_platform_event_t *event) {
    if (event == NULL) {
        return EINVAL;
    }
    AcquireSRWLockExclusive(&event->lock);
    if (event->epoch == UINT64_MAX - UINT64_C(1)) {
        ReleaseSRWLockExclusive(&event->lock);
        return EOVERFLOW;
    }
    event->epoch += UINT64_C(1);
    WakeAllConditionVariable(&event->condition);
    ReleaseSRWLockExclusive(&event->lock);
    return 0;
}

int lccf_platform_event_wait(lccf_platform_event_t *event,
                             uint64_t observed_epoch) {
    if (event == NULL || observed_epoch == UINT64_MAX) {
        return EINVAL;
    }
    AcquireSRWLockExclusive(&event->lock);
    while (event->epoch == observed_epoch) {
        if (!SleepConditionVariableSRW(
                &event->condition,
                &event->lock,
                INFINITE,
                0U)) {
            ReleaseSRWLockExclusive(&event->lock);
            return EIO;
        }
    }
    if (event->epoch < observed_epoch) {
        ReleaseSRWLockExclusive(&event->lock);
        return EPROTO;
    }
    ReleaseSRWLockExclusive(&event->lock);
    return 0;
}

uint64_t lccf_platform_event_epoch(lccf_platform_event_t *event) {
    uint64_t epoch;

    if (event == NULL) {
        return UINT64_MAX;
    }
    AcquireSRWLockShared(&event->lock);
    epoch = event->epoch;
    ReleaseSRWLockShared(&event->lock);
    return epoch;
}

uint64_t lccf_platform_monotonic_ns(void) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    long double nanoseconds;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0 || counter.QuadPart < 0) {
        return 0U;
    }
    nanoseconds =
        (long double)counter.QuadPart *
        1000000000.0L / (long double)frequency.QuadPart;
    if (nanoseconds <= 0.0L ||
        nanoseconds > (long double)UINT64_MAX) {
        return 0U;
    }
    return (uint64_t)nanoseconds;
}

uint64_t lccf_platform_process_cpu_ns(void) {
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER kernel_ticks;
    ULARGE_INTEGER user_ticks;
    uint64_t ticks;

    if (!GetProcessTimes(GetCurrentProcess(),
                         &created,
                         &exited,
                         &kernel,
                         &user)) {
        return 0U;
    }
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    if (UINT64_MAX - kernel_ticks.QuadPart <
        user_ticks.QuadPart) {
        return 0U;
    }
    ticks = kernel_ticks.QuadPart + user_ticks.QuadPart;
    if (ticks > UINT64_MAX / UINT64_C(100)) {
        return 0U;
    }
    return ticks * UINT64_C(100);
}

int lccf_platform_pin_current_thread(unsigned cpu) {
    const unsigned bit_count =
        (unsigned)(sizeof(DWORD_PTR) * CHAR_BIT);
    DWORD_PTR mask;

    if (cpu >= bit_count) {
        return ENOTSUP;
    }
    mask = (DWORD_PTR)1U << cpu;
    return SetThreadAffinityMask(GetCurrentThread(), mask) == 0U ?
               EIO :
               0;
}

int lccf_platform_describe(char *buffer, size_t buffer_size) {
    DWORD active;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return EINVAL;
    }
    active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    written = snprintf(buffer,
                       buffer_size,
                       "os=windows active_processors=%lu qos=unsupported",
                       (unsigned long)active);
    return written < 0 || (size_t)written >= buffer_size ?
               ENOSPC :
               0;
}

#else

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/qos.h>
#include <sys/sysctl.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

struct lccf_platform_thread {
    pthread_t handle;
    lccf_platform_thread_fn fn;
    void *context;
    int result;
};

struct lccf_platform_event {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint64_t epoch;
};

static void *thread_entry(void *opaque) {
    lccf_platform_thread_t *thread = opaque;

    thread->result = thread->fn(thread->context);
    return NULL;
}

int lccf_platform_thread_start(lccf_platform_thread_t **out,
                               lccf_platform_thread_fn fn,
                               void *context) {
    lccf_platform_thread_t *thread;
    int rc;

    if (out == NULL || fn == NULL) {
        return EINVAL;
    }
    *out = NULL;
    thread = calloc(1U, sizeof(*thread));
    if (thread == NULL) {
        return ENOMEM;
    }
    thread->fn = fn;
    thread->context = context;
    rc = pthread_create(
        &thread->handle, NULL, thread_entry, thread);
    if (rc != 0) {
        free(thread);
        return rc;
    }
    *out = thread;
    return 0;
}

int lccf_platform_thread_join(lccf_platform_thread_t *thread,
                              int *out_result) {
    int rc;

    if (thread == NULL) {
        return EINVAL;
    }
    rc = pthread_join(thread->handle, NULL);
    if (rc != 0) {
        return rc;
    }
    if (out_result != NULL) {
        *out_result = thread->result;
    }
    free(thread);
    return 0;
}

int lccf_platform_event_create(lccf_platform_event_t **out) {
    lccf_platform_event_t *event;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    *out = NULL;
    event = calloc(1U, sizeof(*event));
    if (event == NULL) {
        return ENOMEM;
    }
    rc = pthread_mutex_init(&event->mutex, NULL);
    if (rc != 0) {
        free(event);
        return rc;
    }
    rc = pthread_cond_init(&event->condition, NULL);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&event->mutex);
        free(event);
        return rc;
    }
    *out = event;
    return 0;
}

void lccf_platform_event_destroy(lccf_platform_event_t *event) {
    if (event == NULL) {
        return;
    }
    (void)pthread_cond_destroy(&event->condition);
    (void)pthread_mutex_destroy(&event->mutex);
    free(event);
}

int lccf_platform_event_signal(lccf_platform_event_t *event) {
    int rc;
    int unlock_rc;

    if (event == NULL) {
        return EINVAL;
    }
    rc = pthread_mutex_lock(&event->mutex);
    if (rc != 0) {
        return rc;
    }
    if (event->epoch == UINT64_MAX - UINT64_C(1)) {
        (void)pthread_mutex_unlock(&event->mutex);
        return EOVERFLOW;
    }
    event->epoch += UINT64_C(1);
    rc = pthread_cond_broadcast(&event->condition);
    unlock_rc = pthread_mutex_unlock(&event->mutex);
    return rc != 0 ? rc : unlock_rc;
}

int lccf_platform_event_wait(lccf_platform_event_t *event,
                             uint64_t observed_epoch) {
    int rc;
    int unlock_rc;

    if (event == NULL || observed_epoch == UINT64_MAX) {
        return EINVAL;
    }
    rc = pthread_mutex_lock(&event->mutex);
    if (rc != 0) {
        return rc;
    }
    while (event->epoch == observed_epoch) {
        rc = pthread_cond_wait(
            &event->condition, &event->mutex);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&event->mutex);
            return rc;
        }
    }
    if (event->epoch < observed_epoch) {
        (void)pthread_mutex_unlock(&event->mutex);
        return EPROTO;
    }
    unlock_rc = pthread_mutex_unlock(&event->mutex);
    return unlock_rc;
}

uint64_t lccf_platform_event_epoch(lccf_platform_event_t *event) {
    uint64_t epoch;

    if (event == NULL ||
        pthread_mutex_lock(&event->mutex) != 0) {
        return UINT64_MAX;
    }
    epoch = event->epoch;
    if (pthread_mutex_unlock(&event->mutex) != 0) {
        return UINT64_MAX;
    }
    return epoch;
}

static uint64_t clock_ns(clockid_t clock_id) {
    struct timespec now;
    uint64_t seconds;

    if (clock_gettime(clock_id, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L) {
        return 0U;
    }
    seconds = (uint64_t)now.tv_sec;
    if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
        return 0U;
    }
    return seconds * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

uint64_t lccf_platform_monotonic_ns(void) {
    return clock_ns(CLOCK_MONOTONIC);
}

uint64_t lccf_platform_process_cpu_ns(void) {
    return clock_ns(CLOCK_PROCESS_CPUTIME_ID);
}

int lccf_platform_pin_current_thread(unsigned cpu) {
#if defined(__linux__)
    cpu_set_t set;

    if (cpu >= CPU_SETSIZE) {
        return EINVAL;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(
        pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
    return ENOTSUP;
#endif
}

int lccf_platform_describe(char *buffer, size_t buffer_size) {
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return EINVAL;
    }
#if defined(__APPLE__)
    int active = 0;
    size_t active_size = sizeof(active);
    int relative_priority = 0;
    qos_class_t qos_class = QOS_CLASS_UNSPECIFIED;
    const int qos_rc = pthread_get_qos_class_np(
        pthread_self(), &qos_class, &relative_priority);
    const int active_rc = sysctlbyname(
        "hw.activecpu", &active, &active_size, NULL, 0U);

    written = snprintf(
        buffer,
        buffer_size,
        "os=darwin active_processors=%s%d qos_class=%s%u "
        "qos_relative=%d",
        active_rc == 0 ? "" : "unavailable:",
        active_rc == 0 ? active : active_rc,
        qos_rc == 0 ? "" : "unavailable:",
        (unsigned)qos_class,
        relative_priority);
#elif defined(__linux__)
    const long active = sysconf(_SC_NPROCESSORS_ONLN);

    written = snprintf(
        buffer,
        buffer_size,
        "os=linux active_processors=%ld qos=unsupported",
        active);
#else
    const long active = sysconf(_SC_NPROCESSORS_ONLN);

    written = snprintf(
        buffer,
        buffer_size,
        "os=posix active_processors=%ld qos=unsupported",
        active);
#endif
    return written < 0 || (size_t)written >= buffer_size ?
               ENOSPC :
               0;
}

#endif
