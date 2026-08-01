/**
 * @file src/io/api/io_api.c
 * @brief Runtime I/O API coordination and common request lifecycle handling.
 *
 * @details
 * The concrete public I/O entry points are split across focused files:
 *  - @c public.c for read/write/accept/poll wrappers,
 *  - @c issue.c for request parking and backend submission,
 *  - @c direct.c for immediate non-blocking fast paths,
 *  - @c blocking_ops.c for blocking-worker fallbacks.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "io/runtime_io_api_internal.h"
