/**
 * @file examples/server_flood_stats.h
 * @brief Stats parsing and accounting helpers for server_flood.
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
