/**
 * @file src/engine/runtime_watchdog.c
 * @brief Watchdog orchestration for detecting pressure, stalls, and dynamic-worker adjustments.
 *
 * @details
 * The watchdog implementation is split into focused translation units. This
 * compilation unit keeps the public watchdog module boundary stable while the
 * concrete implementation lives in
 * @c runtime_watchdog_worker.c, @c runtime_watchdog_probe.c,
 * @c runtime_watchdog_scale.c, @c runtime_watchdog_merge.c, and
 * @c runtime_watchdog_rehome.c.
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

#include "runtime_watchdog_internal.h"
