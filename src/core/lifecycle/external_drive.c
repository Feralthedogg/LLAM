/**
 * @file src/core/lifecycle/external_drive.c
 * @brief Bounded host-loop driver for externally owned scheduler execution.
 *
 * @details
 * External mode serializes scheduler execution with the runtime run token,
 * installs shard 0 on the calling host thread for one non-waiting quantum, and
 * projects the runtime-owned doorbell and timer deadline to a foreign event
 * loop. Runtime storage is never touched after the run token is released.
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

/** @brief Pin and validate one external-driver runtime handle. */
static int llam_external_runtime_begin(llam_runtime_t *runtime,
                                       llam_runtime_t **out_runtime) {
    llam_runtime_t *pinned = NULL;

    if (runtime == NULL || out_runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) {
        return -1;
    }
    if (!atomic_load_explicit(&pinned->initialized, memory_order_acquire)) {
        llam_runtime_end_public_op(pinned);
        errno = EINVAL;
        return -1;
    }
    if (!pinned->external_driver.enabled) {
        llam_runtime_end_public_op(pinned);
        errno = ENOTSUP;
        return -1;
    }
    *out_runtime = pinned;
    return 0;
}

/** @brief Convert a private quantum result to the public fixed-width value. */
static uint32_t llam_external_public_result(
    llam_scheduler_quantum_result_t result) {
    switch (result) {
    case LLAM_SCHEDULER_QUANTUM_PROGRESS:
        return LLAM_RUNTIME_DRIVE_PROGRESS;
    case LLAM_SCHEDULER_QUANTUM_IDLE:
        return LLAM_RUNTIME_DRIVE_IDLE;
    case LLAM_SCHEDULER_QUANTUM_DONE:
    default:
        return LLAM_RUNTIME_DRIVE_DONE;
    }
}

int llam_runtime_drive_once(llam_runtime_t *runtime,
                            uint32_t *result) {
    llam_runtime_t *pinned = NULL;
    llam_scheduler_quantum_result_t quantum =
        LLAM_SCHEDULER_QUANTUM_DONE;
    llam_thread_signal_stack_t signal_stack;
    bool expected_started = false;
    bool scheduler_entered = false;
    int result_errno = 0;

    memset(&signal_stack, 0, sizeof(signal_stack));
    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (llam_external_runtime_begin(runtime, &pinned) != 0) {
        return -1;
    }
    runtime = pinned;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime->exec_started,
            &expected_started,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        llam_runtime_end_public_op(pinned);
        errno = EBUSY;
        return -1;
    }

    /*
     * Destroy may now claim the handle and wait on exec_started. Keeping the
     * public-operation pin for the whole task segment would deadlock teardown.
     */
    llam_runtime_end_public_op(pinned);
    if (llam_runtime_capture_driver_affinity(runtime) != 0) {
        result_errno = errno != 0 ? errno : EIO;
        goto finish;
    }
    if (llam_scheduler_thread_enter(&runtime->shards[0],
                                    &runtime->host_threads_live,
                                    &signal_stack) != 0) {
        result_errno = errno != 0 ? errno : EIO;
        goto finish;
    }
    scheduler_entered = true;
    llam_external_doorbell_drain(&runtime->external_driver.doorbell);
    quantum = llam_scheduler_run_quantum(&runtime->shards[0]);

finish:
    if (scheduler_entered) {
        llam_scheduler_thread_leave(&runtime->shards[0],
                                    &runtime->host_threads_live,
                                    &signal_stack);
    }
    if (llam_runtime_restore_driver_affinity(runtime) != 0 &&
        result_errno == 0) {
        result_errno = errno != 0 ? errno : EIO;
    }
    if (result_errno == 0) {
        int fatal_errno =
            atomic_load_explicit(&runtime->fatal_errno,
                                 memory_order_acquire);

        if (fatal_errno != 0) {
            result_errno = fatal_errno;
        } else if (quantum == LLAM_SCHEDULER_QUANTUM_DONE) {
            /*
             * Natural drain and explicit stop share the stop bit. Match the
             * full driver and make a cleanly drained handle reusable.
             */
            atomic_store_explicit(&runtime->stop_requested,
                                  false,
                                  memory_order_release);
        } else {
            llam_external_doorbell_rearm(runtime);
        }
    }

    /*
     * This is the final runtime access. A concurrent destroy can free owned
     * state as soon as it observes false.
     */
    atomic_store_explicit(&runtime->exec_started,
                          false,
                          memory_order_release);
    if (result_errno != 0) {
        errno = result_errno;
        return -1;
    }
    *result = llam_external_public_result(quantum);
    return 0;
}

int llam_runtime_next_deadline(llam_runtime_t *runtime,
                               uint64_t *deadline_ns) {
    llam_runtime_t *pinned = NULL;
    llam_shard_t *shard;
    uint64_t deadline = UINT64_MAX;

    if (deadline_ns == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_external_runtime_begin(runtime, &pinned) != 0) {
        return -1;
    }
    shard = &pinned->shards[0];
    pthread_mutex_lock(&shard->lock);
    if (shard->timer_heap_len != 0U &&
        shard->timer_heap != NULL &&
        shard->timer_heap[0] != NULL) {
        deadline = shard->timer_heap[0]->deadline_ns;
    }
    pthread_mutex_unlock(&shard->lock);
    llam_runtime_end_public_op(pinned);
    *deadline_ns = deadline;
    return 0;
}

int llam_runtime_get_readiness(
    llam_runtime_t *runtime,
    llam_runtime_readiness_t *readiness,
    size_t readiness_size) {
    llam_runtime_t *pinned = NULL;
    llam_runtime_readiness_t current;
    size_t copy_size;

    if (readiness == NULL || readiness_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_external_runtime_begin(runtime, &pinned) != 0) {
        return -1;
    }
    memset(&current, 0, sizeof(current));
#if LLAM_PLATFORM_WINDOWS
    if (pinned->external_driver.doorbell.handle == NULL) {
        llam_runtime_end_public_op(pinned);
        errno = EIO;
        return -1;
    }
    current.kind = LLAM_RUNTIME_READINESS_WINDOWS_HANDLE;
    current.value =
        (uintptr_t)pinned->external_driver.doorbell.handle;
#else
    if (pinned->external_driver.doorbell.read_fd < 0) {
        llam_runtime_end_public_op(pinned);
        errno = EIO;
        return -1;
    }
    current.kind = LLAM_RUNTIME_READINESS_FD;
    current.value =
        (uintptr_t)pinned->external_driver.doorbell.read_fd;
#endif
    copy_size = readiness_size < sizeof(current)
                    ? readiness_size
                    : sizeof(current);
    memcpy(readiness, &current, copy_size);
    llam_runtime_end_public_op(pinned);
    return 0;
}

int llam_runtime_wake(llam_runtime_t *runtime) {
    llam_runtime_t *pinned = NULL;
    int rc;
    int saved_errno;

    if (llam_external_runtime_begin(runtime, &pinned) != 0) {
        return -1;
    }
    rc = llam_external_doorbell_signal(
        &pinned->external_driver.doorbell);
    saved_errno = errno;
    llam_runtime_end_public_op(pinned);
    if (rc != 0) {
        errno = saved_errno != 0 ? saved_errno : EIO;
        return -1;
    }
    return 0;
}
