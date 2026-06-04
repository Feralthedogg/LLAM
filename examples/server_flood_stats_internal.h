/**
 * @file examples/server_flood_stats_internal.h
 * @brief Private file-open boundary for server_flood stats parsing.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLAM_EXAMPLES_SERVER_FLOOD_STATS_INTERNAL_H
#define LLAM_EXAMPLES_SERVER_FLOOD_STATS_INTERNAL_H

#include <stdio.h>

FILE *flood_open_stats_file(const char *stats_path);

#endif
