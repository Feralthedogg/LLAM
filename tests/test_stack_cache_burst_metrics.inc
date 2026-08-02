/**
 * @file tests/test_stack_cache_burst_metrics.inc
 * @brief Bounded long-burst controls and Linux RSS evidence.
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

#include "test_env.h"
#if LLAM_RUNTIME_BACKEND_LINUX
#include <sys/resource.h>
#endif

static unsigned stack_cache_test_iterations(void) {
    return llam_test_env_u32("LLAM_TEST_STACK_CACHE_ITERATIONS",
                             400U,
                             1000000U);
}

static int stack_cache_test_report_long_burst(
    unsigned iterations,
    const llam_runtime_stats_t *stats) {
#if LLAM_RUNTIME_BACKEND_LINUX
    struct rusage usage;

    if (iterations == 400U) {
        return 0;
    }
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return fail_errno("long-burst RSS collection failed");
    }
    fprintf(stderr,
            "[test_stack_cache] iterations=%u max_rss_kib=%ld "
            "cached_bytes=%llu committed_bytes=%llu\n",
            iterations,
            usage.ru_maxrss,
            (unsigned long long)stats->stack_cache_cached_bytes,
            (unsigned long long)stats->stack_cache_committed_bytes);
#else
    (void)iterations;
    (void)stats;
#endif
    return 0;
}
