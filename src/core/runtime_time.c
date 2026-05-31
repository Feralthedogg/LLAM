/**
 * @file src/core/runtime_time.c
 * @brief Monotonic time helpers exposed to the runtime and public API.
 *
 * @details
 * The scheduler uses monotonic nanoseconds for deadlines, watchdog slices, idle
 * waits, and benchmark timing. Wall-clock time is deliberately not used here.
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
#include <mach/mach_time.h>
#endif

/**
 * @brief Return current monotonic time in nanoseconds.
 *
 * Darwin uses @c mach_absolute_time with cached timebase conversion. POSIX
 * platforms use @c CLOCK_MONOTONIC.
 *
 * @return Monotonic nanoseconds, or 0 when the platform clock call fails.
 */
uint64_t llam_now_ns(void) {
#if defined(__APPLE__)
    static atomic_uint timebase_numer;
    static atomic_uint timebase_denom;
    uint64_t ticks;
    uint32_t numer;
    uint32_t denom;

    denom = atomic_load_explicit(&timebase_denom, memory_order_acquire);
    if (denom == 0U) {
        mach_timebase_info_data_t timebase;

        if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0U) {
            return 0U;
        }
        /*
         * Time queries can run from user threads before worker startup has
         * settled. Publish numer before denom so an acquire load of denom sees
         * a complete timebase without racing on a mutable static struct.
         */
        atomic_store_explicit(&timebase_numer, timebase.numer, memory_order_release);
        atomic_store_explicit(&timebase_denom, timebase.denom, memory_order_release);
        numer = timebase.numer;
        denom = timebase.denom;
    } else {
        numer = atomic_load_explicit(&timebase_numer, memory_order_acquire);
    }
    ticks = mach_absolute_time();
    if (numer == denom) {
        return ticks;
    }
    if (numer == 125U && denom == 3U) {
        // Common Apple timebase; keep the fast path division explicit.
        return (ticks * 125ULL) / 3ULL;
    }
    return (ticks * (uint64_t)numer) / (uint64_t)denom;
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/**
 * @brief Check whether an absolute monotonic deadline has passed.
 *
 * @param deadline_ns Absolute deadline in ::llam_now_ns units.
 * @return true when the deadline is now or in the past.
 */
bool llam_deadline_passed(uint64_t deadline_ns) {
    return deadline_ns <= llam_now_ns();
}
