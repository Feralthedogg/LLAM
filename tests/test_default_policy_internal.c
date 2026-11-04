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
 * See LICENSE.
 */

#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

#include "llam/managed.h"

static void unqualified_task(void *argument) {
    (void)argument;
}

static int fail(int line) {
    fprintf(stderr, "test_default_policy_internal:%d: errno=%d (%s)\n", line, errno, strerror(errno));
    return 1;
}

int main(void) {
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    llam_runtime_t *runtime = NULL;
    llam_preempt_info_t info;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0 ||
        llam_preempt_opts_init(&policy, sizeof(policy)) != 0 ||
        options.preempt_mode != LLAM_PREEMPT_AUTO ||
        policy.policy != LLAM_PREEMPT_POLICY_ASYNC_REQUIRED) return fail(__LINE__);
    options.deterministic = 1U;
    int created = llam_runtime_create(&options, sizeof(options), &runtime);
    int create_error = errno;
#if !LLAM_ASYNC_CONTEXT_COMPILED || (LLAM_PLATFORM_LINUX && defined(LLAM_TSAN_BUILD))
    if (created != -1 || errno != ENOTSUP || runtime != NULL) return fail(__LINE__);
    if (llam_runtime_init(NULL) != -1 || errno != ENOTSUP) return fail(__LINE__);
    if (llam_runtime_preempt_info_ex(llam_runtime_default(), &info, sizeof(info)) != -1 || errno != EINVAL)
        return fail(__LINE__);
#endif
    if (created == 0) {
        if (runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED ||
            llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) != 0 ||
            info.requested_policy != LLAM_PREEMPT_POLICY_ASYNC_REQUIRED ||
            info.compatibility_processors != 0U || info.legacy_tasks != 0U ||
            info.reason == LLAM_PREEMPT_REASON_LEGACY_RUNTIME ||
            llam_runtime_spawn_ex(runtime, unqualified_task, NULL, NULL, 0U) != NULL ||
            errno != ENOTSUP) return fail(__LINE__);
        llam_runtime_destroy(runtime);
    } else if (create_error != ENOTSUP || runtime != NULL) return fail(__LINE__);

    /* Explicit cooperative choices must not masquerade as the async default. */
    options.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    if (llam_runtime_create(&options, sizeof(options), &runtime) != -1 ||
        errno != EINVAL || runtime != NULL) return fail(__LINE__);
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0 ||
        llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) != 0 ||
        info.requested_policy != LLAM_PREEMPT_POLICY_COOPERATIVE ||
        info.reason != LLAM_PREEMPT_REASON_LEGACY_RUNTIME) return fail(__LINE__);
    llam_task_t *task = llam_runtime_spawn_ex(runtime, unqualified_task, NULL, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) return fail(__LINE__);
    llam_runtime_destroy(runtime);
    if (llam_runtime_init_compat(&options) != 0) return fail(__LINE__);
    task = llam_spawn(unqualified_task, NULL, NULL);
    if (task == NULL || llam_run() != 0 || llam_join(task) != 0) return fail(__LINE__);
    llam_runtime_shutdown();
    puts("test_default_policy_internal: required async defaults, unqualified refusal and explicit compatibility passed");
    return 0;
}
