/**
 * @file examples/env_compat.h
 * @brief Environment-variable compatibility helper shared by examples and benchmarks.
 *
 * @details
 * Examples accept current LLAM_* names first, then old NM_* aliases. Keeping
 * that fallback here avoids stale automation breakage without leaking the old
 * prefix into new user-facing documentation.
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

#ifndef LLAM_EXAMPLES_ENV_COMPAT_H
#define LLAM_EXAMPLES_ENV_COMPAT_H

const char *llam_env_get(const char *name);

static inline const char *llam_example_env_get(const char *name) {
    /*
     * Examples are linked with the runtime in this repository. Delegate alias
     * handling to the runtime utility so LLAM_/legacy NM_ compatibility stays
     * in one implementation instead of drifting across examples and core code.
     */
    return llam_env_get(name);
}

#endif
