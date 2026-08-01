/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "lrpa_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

#if defined(_WIN32)

typedef struct lrpa_thread_start {
    lrpa_thread_fn function;
    void *argument;
} lrpa_thread_start_t;

static DWORD WINAPI
thread_trampoline(LPVOID opaque)
{
    lrpa_thread_start_t *start = opaque;
    const lrpa_thread_fn function = start->function;
    void *argument = start->argument;
    int result;

    free(start);
    result = function(argument);
    return (DWORD)result;
}

uint64_t
lrpa_platform_monotonic_ns(void)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t whole_seconds;
    uint64_t remainder;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return 0U;
    }
    whole_seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return whole_seconds * UINT64_C(1000000000) +
           (remainder * UINT64_C(1000000000)) /
               (uint64_t)frequency.QuadPart;
}

void
lrpa_platform_yield(void)
{
    if (!SwitchToThread()) {
        Sleep(0U);
    }
}

void
lrpa_platform_spin(uint32_t iterations)
{
    uint32_t index;

    for (index = 0U; index < iterations; ++index) {
        atomic_signal_fence(memory_order_seq_cst);
        YieldProcessor();
    }
}

lrpa_status_t
lrpa_platform_thread_create(lrpa_platform_thread_t *thread,
                            lrpa_thread_fn function, void *argument)
{
    lrpa_thread_start_t *start;

    if (thread == NULL || function == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    start = malloc(sizeof(*start));
    if (start == NULL) {
        return LRPA_STATUS_OUT_OF_MEMORY;
    }
    start->function = function;
    start->argument = argument;
    *thread = CreateThread(NULL, 0U, thread_trampoline, start, 0U, NULL);
    if (*thread == NULL) {
        free(start);
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    return LRPA_STATUS_OK;
}

lrpa_status_t
lrpa_platform_thread_join(lrpa_platform_thread_t thread)
{
    const DWORD wait_result = WaitForSingleObject(thread, INFINITE);

    if (wait_result != WAIT_OBJECT_0) {
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    return CloseHandle(thread) ? LRPA_STATUS_OK : LRPA_STATUS_PLATFORM_ERROR;
}

lrpa_status_t
lrpa_platform_gate_init(lrpa_platform_gate_t *gate)
{
    if (gate == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    InitializeCriticalSection(&gate->mutex);
    InitializeConditionVariable(&gate->condition);
    gate->open = false;
    gate->aborted = false;
    return LRPA_STATUS_OK;
}

void
lrpa_platform_gate_open(lrpa_platform_gate_t *gate, bool aborted)
{
    EnterCriticalSection(&gate->mutex);
    gate->aborted = aborted;
    gate->open = true;
    WakeAllConditionVariable(&gate->condition);
    LeaveCriticalSection(&gate->mutex);
}

bool
lrpa_platform_gate_wait(lrpa_platform_gate_t *gate)
{
    bool allowed;

    EnterCriticalSection(&gate->mutex);
    while (!gate->open) {
        (void)SleepConditionVariableCS(&gate->condition, &gate->mutex,
                                       INFINITE);
    }
    allowed = !gate->aborted;
    LeaveCriticalSection(&gate->mutex);
    return allowed;
}

void
lrpa_platform_gate_destroy(lrpa_platform_gate_t *gate)
{
    DeleteCriticalSection(&gate->mutex);
}

lrpa_status_t
lrpa_platform_barrier_init(lrpa_platform_barrier_t *barrier,
                           uint32_t participants)
{
    if (barrier == NULL || participants == 0U) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    InitializeCriticalSection(&barrier->mutex);
    InitializeConditionVariable(&barrier->condition);
    barrier->participants = participants;
    barrier->arrivals = 0U;
    barrier->generation = 0U;
    barrier->broken = false;
    return LRPA_STATUS_OK;
}

static DWORD
timeout_milliseconds(uint64_t timeout_ns)
{
    uint64_t milliseconds = (timeout_ns + UINT64_C(999999)) /
                            UINT64_C(1000000);

    if (milliseconds == 0U) {
        milliseconds = 1U;
    }
    if (milliseconds >= (uint64_t)INFINITE) {
        milliseconds = (uint64_t)INFINITE - 1U;
    }
    return (DWORD)milliseconds;
}

bool
lrpa_platform_barrier_wait(lrpa_platform_barrier_t *barrier,
                           uint64_t timeout_ns)
{
    const DWORD timeout_ms = timeout_milliseconds(timeout_ns);
    uint64_t generation;
    bool success = true;

    EnterCriticalSection(&barrier->mutex);
    if (barrier->broken) {
        LeaveCriticalSection(&barrier->mutex);
        return false;
    }
    generation = barrier->generation;
    barrier->arrivals += 1U;
    if (barrier->arrivals == barrier->participants) {
        barrier->arrivals = 0U;
        barrier->generation += 1U;
        WakeAllConditionVariable(&barrier->condition);
        LeaveCriticalSection(&barrier->mutex);
        return true;
    }
    while (!barrier->broken && generation == barrier->generation) {
        if (!SleepConditionVariableCS(&barrier->condition, &barrier->mutex,
                                      timeout_ms)) {
            if (GetLastError() == ERROR_TIMEOUT) {
                barrier->broken = true;
                WakeAllConditionVariable(&barrier->condition);
                success = false;
                break;
            }
        }
    }
    if (barrier->broken) {
        success = false;
    }
    LeaveCriticalSection(&barrier->mutex);
    return success;
}

void
lrpa_platform_barrier_break(lrpa_platform_barrier_t *barrier)
{
    EnterCriticalSection(&barrier->mutex);
    barrier->broken = true;
    WakeAllConditionVariable(&barrier->condition);
    LeaveCriticalSection(&barrier->mutex);
}

void
lrpa_platform_barrier_destroy(lrpa_platform_barrier_t *barrier)
{
    DeleteCriticalSection(&barrier->mutex);
}

#else

#include <errno.h>
#include <sched.h>
#include <time.h>

typedef struct lrpa_thread_start {
    lrpa_thread_fn function;
    void *argument;
} lrpa_thread_start_t;

static void *
thread_trampoline(void *opaque)
{
    lrpa_thread_start_t *start = opaque;
    const lrpa_thread_fn function = start->function;
    void *argument = start->argument;
    int result;

    free(start);
    result = function(argument);
    (void)result;
    return NULL;
}

uint64_t
lrpa_platform_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

void
lrpa_platform_yield(void)
{
    (void)sched_yield();
}

void
lrpa_platform_spin(uint32_t iterations)
{
    uint32_t index;

    for (index = 0U; index < iterations; ++index) {
        atomic_signal_fence(memory_order_seq_cst);
    }
}

lrpa_status_t
lrpa_platform_thread_create(lrpa_platform_thread_t *thread,
                            lrpa_thread_fn function, void *argument)
{
    lrpa_thread_start_t *start;
    int status;

    if (thread == NULL || function == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    start = malloc(sizeof(*start));
    if (start == NULL) {
        return LRPA_STATUS_OUT_OF_MEMORY;
    }
    start->function = function;
    start->argument = argument;
    status = pthread_create(thread, NULL, thread_trampoline, start);
    if (status != 0) {
        free(start);
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    return LRPA_STATUS_OK;
}

lrpa_status_t
lrpa_platform_thread_join(lrpa_platform_thread_t thread)
{
    return pthread_join(thread, NULL) == 0 ? LRPA_STATUS_OK
                                           : LRPA_STATUS_PLATFORM_ERROR;
}

lrpa_status_t
lrpa_platform_gate_init(lrpa_platform_gate_t *gate)
{
    if (gate == NULL) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    if (pthread_cond_init(&gate->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&gate->mutex);
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    gate->open = false;
    gate->aborted = false;
    return LRPA_STATUS_OK;
}

void
lrpa_platform_gate_open(lrpa_platform_gate_t *gate, bool aborted)
{
    (void)pthread_mutex_lock(&gate->mutex);
    gate->aborted = aborted;
    gate->open = true;
    (void)pthread_cond_broadcast(&gate->condition);
    (void)pthread_mutex_unlock(&gate->mutex);
}

bool
lrpa_platform_gate_wait(lrpa_platform_gate_t *gate)
{
    bool allowed;

    (void)pthread_mutex_lock(&gate->mutex);
    while (!gate->open) {
        (void)pthread_cond_wait(&gate->condition, &gate->mutex);
    }
    allowed = !gate->aborted;
    (void)pthread_mutex_unlock(&gate->mutex);
    return allowed;
}

void
lrpa_platform_gate_destroy(lrpa_platform_gate_t *gate)
{
    (void)pthread_cond_destroy(&gate->condition);
    (void)pthread_mutex_destroy(&gate->mutex);
}

static struct timespec
realtime_deadline(uint64_t timeout_ns)
{
    struct timespec deadline;
    uint64_t nanoseconds;

    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    nanoseconds = (uint64_t)deadline.tv_nsec +
                  timeout_ns % UINT64_C(1000000000);
    deadline.tv_sec += (time_t)(timeout_ns / UINT64_C(1000000000));
    deadline.tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    deadline.tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));
    return deadline;
}

lrpa_status_t
lrpa_platform_barrier_init(lrpa_platform_barrier_t *barrier,
                           uint32_t participants)
{
    if (barrier == NULL || participants == 0U) {
        return LRPA_STATUS_INVALID_ARGUMENT;
    }
    if (pthread_mutex_init(&barrier->mutex, NULL) != 0) {
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    if (pthread_cond_init(&barrier->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&barrier->mutex);
        return LRPA_STATUS_PLATFORM_ERROR;
    }
    barrier->participants = participants;
    barrier->arrivals = 0U;
    barrier->generation = 0U;
    barrier->broken = false;
    return LRPA_STATUS_OK;
}

bool
lrpa_platform_barrier_wait(lrpa_platform_barrier_t *barrier,
                           uint64_t timeout_ns)
{
    const struct timespec deadline = realtime_deadline(timeout_ns);
    uint64_t generation;
    bool success = true;

    (void)pthread_mutex_lock(&barrier->mutex);
    if (barrier->broken) {
        (void)pthread_mutex_unlock(&barrier->mutex);
        return false;
    }
    generation = barrier->generation;
    barrier->arrivals += 1U;
    if (barrier->arrivals == barrier->participants) {
        barrier->arrivals = 0U;
        barrier->generation += 1U;
        (void)pthread_cond_broadcast(&barrier->condition);
        (void)pthread_mutex_unlock(&barrier->mutex);
        return true;
    }
    while (!barrier->broken && generation == barrier->generation) {
        const int status = pthread_cond_timedwait(
            &barrier->condition, &barrier->mutex, &deadline);

        if (status == ETIMEDOUT) {
            barrier->broken = true;
            (void)pthread_cond_broadcast(&barrier->condition);
            success = false;
            break;
        }
        if (status != 0) {
            barrier->broken = true;
            (void)pthread_cond_broadcast(&barrier->condition);
            success = false;
            break;
        }
    }
    if (barrier->broken) {
        success = false;
    }
    (void)pthread_mutex_unlock(&barrier->mutex);
    return success;
}

void
lrpa_platform_barrier_break(lrpa_platform_barrier_t *barrier)
{
    (void)pthread_mutex_lock(&barrier->mutex);
    barrier->broken = true;
    (void)pthread_cond_broadcast(&barrier->condition);
    (void)pthread_mutex_unlock(&barrier->mutex);
}

void
lrpa_platform_barrier_destroy(lrpa_platform_barrier_t *barrier)
{
    (void)pthread_cond_destroy(&barrier->condition);
    (void)pthread_mutex_destroy(&barrier->mutex);
}

#endif
