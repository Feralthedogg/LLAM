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
uint64_t nm_now_ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t timebase;
    uint64_t ticks;
    uint32_t numer;
    uint32_t denom;

    if (timebase.denom == 0U) {
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0U) {
            return 0U;
        }
    }
    ticks = mach_absolute_time();
    numer = timebase.numer;
    denom = timebase.denom;
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
