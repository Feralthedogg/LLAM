/**
 * @file src/engine/watchdog/watchdog.c
 * @brief Watchdog orchestration for detecting pressure, stalls, and dynamic-worker adjustments.
 *
 * @details
 * The watchdog implementation is split into focused translation units. This
 * compilation unit keeps the public watchdog module boundary stable while the
 * concrete implementation lives in
 * @c watchdog_worker.c, @c watchdog_probe.c,
 * @c watchdog_scale.c, @c watchdog_merge.c, and
 * @c watchdog_rehome.c.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "engine/runtime_watchdog_internal.h"
