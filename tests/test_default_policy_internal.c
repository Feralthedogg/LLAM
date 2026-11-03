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

int main(void) {
    llam_runtime_opts_t options;
    int failures = 0;

    memset(&options, 0xa5, sizeof(options));
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        fprintf(stderr, "test_default_policy_internal: default options init failed\n");
        return 1;
    }
    if (options.preempt_mode != LLAM_PREEMPT_AUTO) {
        fprintf(stderr,
                "test_default_policy_internal: default preempt mode changed to %u\n",
                options.preempt_mode);
        failures += 1;
    }
    if (llam_preempt_effective_policy_for(options.preempt_mode,
                                          LLAM_PREEMPT_BACKEND_COOPERATIVE,
                                          false,
                                          0U) != LLAM_PREEMPT_EFFECTIVE_COOPERATIVE) {
        fprintf(stderr,
                "test_default_policy_internal: AUTO did not preserve cooperative fallback\n");
        failures += 1;
    }
    if (llam_preempt_effective_policy_for(LLAM_PREEMPT_COOPERATIVE,
                                          LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT,
                                          true,
                                          4U) != LLAM_PREEMPT_EFFECTIVE_COOPERATIVE) {
        fprintf(stderr,
                "test_default_policy_internal: COOPERATIVE was not permanent\n");
        failures += 1;
    }
    if (failures != 0) {
        return 1;
    }
    puts("test_default_policy_internal: AUTO default and permanent cooperative mode passed");
    return 0;
}
