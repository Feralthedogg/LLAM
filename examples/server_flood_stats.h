/**
 * @file examples/server_flood_stats.h
 * @brief Stats parsing and accounting helpers for server_flood.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLAM_EXAMPLES_SERVER_FLOOD_STATS_H
#define LLAM_EXAMPLES_SERVER_FLOOD_STATS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct flood_server_stats {
    bool available;
    uint64_t outbox_full_drops;
    uint64_t outbox_closed_drops;
    uint64_t broadcast_messages_created;
    uint64_t broadcast_deliveries_attempted;
    uint64_t broadcast_deliveries_enqueued;
} flood_server_stats_t;

bool flood_read_server_stats(const char *stats_path, flood_server_stats_t *stats);
void flood_print_server_stats(const flood_server_stats_t *stats);
intmax_t flood_abs_imax(intmax_t value);
intmax_t flood_print_accounting(const flood_server_stats_t *stats,
                                uint64_t expected_deliveries,
                                uint64_t observed_deliveries,
                                uint64_t missing_deliveries);
intmax_t flood_accounting_tolerance(uint64_t expected_deliveries);

#endif
