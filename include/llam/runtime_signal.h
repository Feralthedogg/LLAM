/**
 * @file include/llam/runtime_signal.h
 * @brief Public process-signal policy constants for LLAM embedders.
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

#ifndef LLAM_RUNTIME_SIGNAL_H
#define LLAM_RUNTIME_SIGNAL_H

/** @brief Process-signal integration flags accepted by llam_runtime_opts_t. */
enum {
    /** Install the cooperative-preemption handler on the selected signal. */
    LLAM_RUNTIME_SIGNAL_F_PREEMPT = 1U << 0,
    /** Install task guard-page fault handling on the platform fault signal. */
    LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT = 1U << 1,
};

/** @brief Backward-compatible signal policy used by the option initializer. */
#define LLAM_RUNTIME_SIGNAL_DEFAULT_FLAGS \
    (LLAM_RUNTIME_SIGNAL_F_PREEMPT | LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT)

#endif
