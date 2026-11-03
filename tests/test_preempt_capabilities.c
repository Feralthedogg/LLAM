/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_preempt_capabilities: %s\n", message);
        g_failures += 1U;
    }
}

int main(void) {
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    uint32_t async_backend = LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT;
    unsigned state;

    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_OFF,
                                            async_backend,
                                            true,
                                            1U) == LLAM_PREEMPT_EFFECTIVE_OFF,
          "OFF did not remain a permanent disabled policy");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_COOPERATIVE,
                                            async_backend,
                                            true,
                                            1U) == LLAM_PREEMPT_EFFECTIVE_COOPERATIVE,
          "COOPERATIVE did not remain permanently cooperative");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_AUTO,
                                            LLAM_PREEMPT_BACKEND_COOPERATIVE,
                                            true,
                                            1U) == LLAM_PREEMPT_EFFECTIVE_COOPERATIVE,
          "AUTO did not fall back on a cooperative-only target");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_AUTO,
                                            async_backend,
                                            false,
                                            1U) == LLAM_PREEMPT_EFFECTIVE_COOPERATIVE,
          "AUTO advertised hybrid policy before async activation");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_AUTO,
                                            async_backend,
                                            true,
                                            0U) == LLAM_PREEMPT_EFFECTIVE_COOPERATIVE,
          "AUTO advertised async policy without a registered range");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_AUTO,
                                            async_backend,
                                            true,
                                            2U) == LLAM_PREEMPT_EFFECTIVE_HYBRID,
          "AUTO did not select hybrid fallback for active safe ranges");
    check(llam_preempt_effective_policy_for(LLAM_PREEMPT_STRICT,
                                            async_backend,
                                            true,
                                            2U) == LLAM_PREEMPT_EFFECTIVE_HYBRID,
          "STRICT did not preserve hybrid fallback for registered safe ranges");

    check(runtime != NULL, "runtime capability test allocation failed");
    if (runtime == NULL) {
        return 1;
    }
    runtime->preempt_mode = LLAM_PREEMPT_AUTO;
    atomic_init(&runtime->async_preempt_enabled, 0U);
    llam_preempt_capabilities_init(runtime);
    check((runtime->preempt_capability.compiled_backends &
           LLAM_PREEMPT_BACKEND_COOPERATIVE) != 0U,
          "cooperative backend was not always available");
    check((runtime->preempt_capability.compiled_backends &
           LLAM_PREEMPT_BACKEND_METADATA_V1) != 0U,
          "version-1 metadata capability was not reported");
#if (LLAM_PLATFORM_LINUX || LLAM_PLATFORM_WINDOWS) && LLAM_ARCH_X86_64 && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    check((runtime->preempt_capability.compiled_backends &
           LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U,
          "experimental x86-64 async build was not reported");
#else
    check((runtime->preempt_capability.compiled_backends &
           LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) == 0U,
          "unsupported/default build advertised async context capture");
#endif
    llam_preempt_capabilities_note_ranges(runtime, 2U);
    state = atomic_load_explicit(&runtime->preempt_capability.async_state,
                                 memory_order_acquire);
    if ((runtime->preempt_capability.compiled_backends &
         LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U) {
        check(state == LLAM_PREEMPT_ASYNC_RANGES_READY,
              "safe range publication did not advance the async capability state");
        llam_preempt_capabilities_note_async_result(runtime,
                                                     true,
                                                     LLAM_PREEMPT_FAILURE_NONE);
        check(atomic_load_explicit(&runtime->preempt_capability.async_state,
                                   memory_order_acquire) == LLAM_PREEMPT_ASYNC_ACTIVE,
              "successful activation did not publish the active state");
        llam_preempt_capabilities_note_async_result(runtime,
                                                     false,
                                                     LLAM_PREEMPT_FAILURE_SIGNAL_CONFLICT);
        check(atomic_load_explicit(&runtime->preempt_capability.async_state,
                                   memory_order_acquire) == LLAM_PREEMPT_ASYNC_RANGES_READY &&
                  atomic_load_explicit(&runtime->preempt_capability.failure_reason,
                                       memory_order_acquire) ==
                      LLAM_PREEMPT_FAILURE_SIGNAL_CONFLICT,
              "failed activation lost its safe ranges or failure reason");
    } else {
        check(state == LLAM_PREEMPT_ASYNC_UNAVAILABLE,
              "unsupported async target did not stay unavailable");
        check(atomic_load_explicit(&runtime->preempt_capability.failure_reason,
                                   memory_order_acquire) != LLAM_PREEMPT_FAILURE_NONE,
              "unsupported async target omitted its fallback reason");
    }
    check(strcmp(llam_preempt_async_state_name((llam_preempt_async_state_t)state),
                 "ranges-ready") == 0 ||
              strcmp(llam_preempt_async_state_name((llam_preempt_async_state_t)state),
                     "unavailable") == 0,
          "capability state formatter returned an invalid state");
    check(strcmp(llam_preempt_failure_reason_name(LLAM_PREEMPT_FAILURE_NONE), "none") == 0,
          "failure reason formatter lost the no-failure state");
    check(strcmp(llam_preempt_effective_policy_name(LLAM_PREEMPT_EFFECTIVE_HYBRID),
                 "hybrid") == 0,
          "effective policy formatter lost the hybrid state");
    free(runtime);
#if !(LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT))
    {
        llam_runtime_opts_t runtime_options;
        llam_runtime_t *public_runtime = NULL;
        llam_windows_async_preempt_range_t range = {1U, 2U, 0U, 0U};
        llam_windows_async_preempt_opts_t async_options = {0};

        check(llam_runtime_opts_init(&runtime_options,
                                     LLAM_RUNTIME_OPTS_CURRENT_SIZE) == 0,
              "public unsupported options init failed");
        runtime_options.deterministic = 1U;
        runtime_options.preempt_mode = LLAM_PREEMPT_STRICT;
        check(llam_runtime_create(&runtime_options,
                                  LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                                  &public_runtime) == 0 && public_runtime != NULL,
              "public unsupported runtime create failed");
        if (public_runtime != NULL) {
            async_options.flags =
                LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
            async_options.ranges = &range;
            async_options.range_count = 1U;
            errno = 0;
            check(llam_runtime_enable_experimental_windows_async_preempt_ex(
                      public_runtime, &async_options, sizeof(async_options)) == -1 &&
                      errno == ENOTSUP,
                  "unsupported/default build did not expose an ENOTSUP stub");
            async_options.flags = 0U;
            errno = 0;
            check(llam_runtime_enable_experimental_windows_async_preempt_ex(
                      public_runtime, &async_options, sizeof(async_options)) == -1 &&
                      errno == EINVAL,
                  "missing process-failfast acknowledgement was accepted");
            llam_runtime_destroy(public_runtime);
        }
    }
#endif
    if (g_failures != 0U) {
        fprintf(stderr, "test_preempt_capabilities: %u failure(s)\n", g_failures);
        return 1;
    }
    puts("test_preempt_capabilities: fallback matrix and state tracking passed");
    return 0;
}
