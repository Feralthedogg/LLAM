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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "runtime_internal.h"
