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
#include "windows_async_lifetime.h"

#include <stdio.h>

static void never_run(void *opaque) {
    (void)opaque;
    abort();
}

int main(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *rt;

    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    rt = llam_runtime_default_storage();
    if (rt == NULL || llam_windows_async_lifetime_enable(rt) != 0) {
        return 1;
    }
    if (llam_spawn(never_run, NULL, NULL) == NULL ||
        llam_windows_async_lifetime_charged_bytes(rt) == 0U ||
        atomic_load_explicit(&rt->shards[0].windows_async_execution_debt,
                             memory_order_acquire) != 0U) {
        return 1;
    }
    /* A prepared but never-dispatched G has no M-side retirement obligation. */
    llam_runtime_shutdown();
    if (llam_windows_async_lifetime_charged_bytes(rt) != 0U ||
        rt->async_preempt_control != NULL) {
        return 1;
    }
    puts("llam-async-lifetime: never-started capsule cleaned");
    return 0;
}
