/**
 * @file src/core/api/core_api.c
 * @brief Canonical llam_* public runtime API entry points and argument validation.
 *
 * @details
 * The canonical public API is split across subsystem-specific translation units
 * (spawn, run, synchronization, I/O, cancellation, diagnostics, etc.). This file
 * intentionally stays minimal so build systems that expect @c core_api.c to
 * exist continue to compile cleanly.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "runtime_internal.h"
