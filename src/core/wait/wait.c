/**
 * @file src/core/wait/wait.c
 * @brief Wait-object parking, deadline handling, and waiter completion helpers.
 *
 * @details
 * Wait behavior is implemented in focused companion files:
 *  - wait tracking and cancellation/deadline glue in @c wait_tracking.c,
 *  - timer heap handling in @c timer.c,
 *  - synchronization wait queues in @c sync.c,
 *  - and public join/sleep APIs in @c yield_join_sleep.c.
 *
 * This file stays intentionally minimal so the build keeps a stable wait module
 * boundary while the implementation remains split by concern.
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
