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

#include "llam_internal.h"

#include <errno.h>
#include <stdatomic.h>

/* Include only the native errno macro here, not runtime_state.h's migrating
 * cursor. The compiler fence preserves the read-only lookup contract under
 * LTO so opaque calls invalidate it instead of reusing a source M's address. */
LLAM_NOINLINE int *llam_native_errno_location(void) {
    atomic_signal_fence(memory_order_acq_rel);
    return &errno;
}
