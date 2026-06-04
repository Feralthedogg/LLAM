/**
 * @file src/core/broker/ring/broker_ring_doorbell.c
 * @brief Kernel-assisted waits for broker shared rings.
 *
 * @details
 * The ring cursors remain lock-free and authoritative. A doorbell is an
 * optional side-channel used by transports that would otherwise spin on empty
 * or full SPSC rings. It reuses LLAM's platform wake handles: eventfd/futex
 * adjacent wakeups on Linux, EVFILT_USER kqueue wakeups on Darwin, and manual
 * reset events on Windows.
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
#include "runtime_broker_ring.h"

typedef bool (*llam_broker_ring_ready_fn)(const llam_broker_ring_t *ring);

static bool llam_broker_ring_submit_available(const llam_broker_ring_t *ring) {
    uint64_t head = atomic_load_explicit(&ring->submit_head.value, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_acquire);

    return llam_broker_ring_window_valid(head, tail) && head != tail;
}

static bool llam_broker_ring_submit_space(const llam_broker_ring_t *ring) {
    uint64_t head = atomic_load_explicit(&ring->submit_head.value, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_acquire);

    return llam_broker_ring_window_valid(head, tail) && tail - head < LLAM_BROKER_RING_CAP;
}

static bool llam_broker_ring_completion_available(const llam_broker_ring_t *ring) {
    uint64_t head = atomic_load_explicit(&ring->complete_head.value, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_acquire);

    return llam_broker_ring_window_valid(head, tail) && head != tail;
}

static bool llam_broker_ring_completion_space(const llam_broker_ring_t *ring) {
    uint64_t head = atomic_load_explicit(&ring->complete_head.value, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_acquire);

    return llam_broker_ring_window_valid(head, tail) && tail - head < LLAM_BROKER_RING_CAP;
}

static int llam_broker_ring_wait_ready(const llam_broker_ring_t *ring,
                                       llam_broker_ring_doorbell_t *doorbell,
                                       int timeout_ms,
                                       llam_broker_ring_ready_fn ready) {
    int wait_rc;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || doorbell == NULL || ready == NULL || doorbell->handle < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (ready(ring)) {
        if (atomic_load_explicit(&doorbell->pending, memory_order_acquire) != 0U) {
            llam_wake_handle_drain(doorbell->handle);
            atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
        }
        return 0;
    }
    if (atomic_load_explicit(&doorbell->pending, memory_order_acquire) != 0U) {
        llam_wake_handle_drain(doorbell->handle);
        atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
        if (ready(ring)) {
            return 0;
        }
        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }

    wait_rc = llam_wake_handle_wait(doorbell->handle, timeout_ms);
    if (wait_rc < 0) {
        return -1;
    }
    if (wait_rc == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    llam_wake_handle_drain(doorbell->handle);
    atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
    if (!ready(ring)) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

int llam_broker_ring_doorbell_init(llam_broker_ring_doorbell_t *doorbell) {
    int handle;

    if (LLAM_UNLIKELY(doorbell == NULL)) {
        errno = EINVAL;
        return -1;
    }
    doorbell->handle = -1;
    atomic_init(&doorbell->pending, 0U);
    handle = llam_wake_handle_create();
    if (handle < 0) {
        return -1;
    }
    doorbell->handle = handle;
    return 0;
}

void llam_broker_ring_doorbell_destroy(llam_broker_ring_doorbell_t *doorbell) {
    int handle;

    if (doorbell == NULL) {
        return;
    }
    handle = doorbell->handle;
    doorbell->handle = -1;
    atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
    llam_wake_handle_close(handle);
}

int llam_broker_ring_doorbell_signal(llam_broker_ring_doorbell_t *doorbell) {
    if (LLAM_UNLIKELY(doorbell == NULL || doorbell->handle < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_eventfd_try_claim(&doorbell->pending) == 0U) {
        return 0;
    }
    if (llam_wake_handle_signal(doorbell->handle) != 0) {
        atomic_store_explicit(&doorbell->pending, 0U, memory_order_release);
        return -1;
    }
    return 0;
}

int llam_broker_ring_wait_submit_available(const llam_broker_ring_t *ring,
                                           llam_broker_ring_doorbell_t *doorbell,
                                           int timeout_ms) {
    return llam_broker_ring_wait_ready(ring, doorbell, timeout_ms, llam_broker_ring_submit_available);
}

int llam_broker_ring_wait_submit_space(const llam_broker_ring_t *ring,
                                       llam_broker_ring_doorbell_t *doorbell,
                                       int timeout_ms) {
    return llam_broker_ring_wait_ready(ring, doorbell, timeout_ms, llam_broker_ring_submit_space);
}

int llam_broker_ring_wait_completion_available(const llam_broker_ring_t *ring,
                                               llam_broker_ring_doorbell_t *doorbell,
                                               int timeout_ms) {
    return llam_broker_ring_wait_ready(ring, doorbell, timeout_ms, llam_broker_ring_completion_available);
}

int llam_broker_ring_wait_completion_space(const llam_broker_ring_t *ring,
                                           llam_broker_ring_doorbell_t *doorbell,
                                           int timeout_ms) {
    return llam_broker_ring_wait_ready(ring, doorbell, timeout_ms, llam_broker_ring_completion_space);
}
