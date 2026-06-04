/**
 * @file examples/server_flood_stats.c
 * @brief Hardened stats parsing and accounting for server_flood.
 *
 * @details
 * The server under test controls when the stats file is written.  Read it via
 * O_NOFOLLOW and validate the opened fd so a broken or compromised server
 * cannot feed accounting from a symlinked or hard-linked outside file.
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

#include "server_flood_stats.h"
#include "server_flood_stats_internal.h"

#if defined(_WIN32)

#include <inttypes.h>
#include <stdio.h>

bool flood_read_server_stats(const char *stats_path, flood_server_stats_t *stats) {
    (void)stats_path;
    if (stats != NULL) {
        *stats = (flood_server_stats_t){0};
    }
    return false;
}

void flood_print_server_stats(const flood_server_stats_t *stats) {
    (void)stats;
}

intmax_t flood_abs_imax(intmax_t value) {
    return value < 0 ? -value : value;
}

intmax_t flood_print_accounting(const flood_server_stats_t *stats,
                                uint64_t expected_deliveries,
                                uint64_t observed_deliveries,
                                uint64_t missing_deliveries) {
    (void)stats;
    (void)expected_deliveries;
    (void)observed_deliveries;
    return (intmax_t)missing_deliveries;
}

intmax_t flood_accounting_tolerance(uint64_t expected_deliveries) {
    (void)expected_deliveries;
    return 0;
}

#else

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool flood_stats_key_boundary(const char *line, const char *cursor) {
    unsigned char prev;

    if (line == NULL || cursor == NULL) {
        return false;
    }
    if (cursor == line) {
        return true;
    }
    prev = (unsigned char)cursor[-1];
    return isspace(prev) || prev == ';';
}

static const char *flood_find_stats_field(const char *line, const char *key) {
    const char *cursor;
    size_t key_len;

    if (line == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }
    key_len = strlen(key);
    cursor = line;
    while ((cursor = strstr(cursor, key)) != NULL) {
        /*
         * Reject substrings inside another token, e.g.
         * "xoutbox_full_drops=0".  The flood harness treats stats as a
         * security boundary for accounting, so malformed keys must not be
         * accepted as valid fields.
         */
        if (flood_stats_key_boundary(line, cursor) && cursor[key_len] == '=') {
            return cursor;
        }
        ++cursor;
    }
    return NULL;
}

static bool flood_parse_u64_field(const char *line, const char *key, uint64_t *out) {
    const char *cursor = flood_find_stats_field(line, key);
    char *end = NULL;
    unsigned long long parsed;

    if (cursor == NULL || out == NULL) { return false; }
    cursor += strlen(key);
    if (*cursor != '=') { return false; }
    cursor += 1;
    if (*cursor == '-' || *cursor == '+') { return false; }
    errno = 0;
    parsed = strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || (*end != '\0' && !isspace((unsigned char)*end))) {
        return false;
    }
    if ((uint64_t)parsed > (uint64_t)INTMAX_MAX) { return false; }
    if (flood_find_stats_field(end, key) != NULL) { return false; }
    *out = (uint64_t)parsed;
    return true;
}

static bool flood_u64_add(uint64_t lhs, uint64_t rhs, uint64_t *out) {
    if (out == NULL || lhs > UINT64_MAX - rhs) { return false; }
    *out = lhs + rhs;
    return true;
}

static bool flood_server_stats_consistent(const flood_server_stats_t *stats) {
    uint64_t drops;

    if (stats == NULL || !flood_u64_add(stats->outbox_full_drops, stats->outbox_closed_drops, &drops)) {
        return false;
    }
    /*
     * The chat server accounts each attempted fanout delivery as exactly one
     * enqueue or one explicit outbox drop.  Reject impossible counters before
     * they can wrap accounting arithmetic and make corrupt stats look valid.
     */
    if (stats->broadcast_deliveries_enqueued > stats->broadcast_deliveries_attempted) {
        return false;
    }
    return drops == stats->broadcast_deliveries_attempted - stats->broadcast_deliveries_enqueued;
}

bool flood_read_server_stats(const char *stats_path, flood_server_stats_t *stats) {
    char line[512];
    char last_line[512];
    FILE *file;

    if (stats == NULL) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    file = flood_open_stats_file(stats_path);
    if (file == NULL) {
        return false;
    }
    last_line[0] = '\0';
    while (fgets(line, sizeof(line), file) != NULL) {
        if (line[0] != '\0') {
            (void)snprintf(last_line, sizeof(last_line), "%s", line);
        }
    }
    fclose(file);
    if (last_line[0] == '\0') {
        return false;
    }
    last_line[strcspn(last_line, "\r\n")] = '\0';
    if (!flood_parse_u64_field(last_line, "outbox_full_drops", &stats->outbox_full_drops) ||
        !flood_parse_u64_field(last_line, "outbox_closed_drops", &stats->outbox_closed_drops) ||
        !flood_parse_u64_field(last_line, "broadcast_messages_created", &stats->broadcast_messages_created) ||
        !flood_parse_u64_field(last_line, "broadcast_deliveries_attempted", &stats->broadcast_deliveries_attempted) ||
        !flood_parse_u64_field(last_line, "broadcast_deliveries_enqueued", &stats->broadcast_deliveries_enqueued)) {
        return false;
    }
    if (!flood_server_stats_consistent(stats)) {
        memset(stats, 0, sizeof(*stats));
        return false;
    }
    stats->available = true;
    return true;
}

void flood_print_server_stats(const flood_server_stats_t *stats) {
    printf("server flood stats: server stopped; outbox_full_drops=%" PRIu64
           " outbox_closed_drops=%" PRIu64
           " broadcast_messages_created=%" PRIu64
           " broadcast_deliveries_attempted=%" PRIu64
           " broadcast_deliveries_enqueued=%" PRIu64 "\n",
           stats->outbox_full_drops,
           stats->outbox_closed_drops,
           stats->broadcast_messages_created,
           stats->broadcast_deliveries_attempted,
           stats->broadcast_deliveries_enqueued);
}

static intmax_t flood_delta_u64(uint64_t lhs, uint64_t rhs) {
    if (lhs >= rhs) {
        return (intmax_t)(lhs - rhs);
    }
    return -(intmax_t)(rhs - lhs);
}

intmax_t flood_abs_imax(intmax_t value) {
    if (value == INTMAX_MIN) { return INTMAX_MAX; }
    return value < 0 ? -value : value;
}

intmax_t flood_print_accounting(const flood_server_stats_t *stats,
                                uint64_t expected_deliveries,
                                uint64_t observed_deliveries,
                                uint64_t missing_deliveries) {
    uint64_t outbox_drops = stats->outbox_full_drops + stats->outbox_closed_drops;
    intmax_t expected_minus_attempted =
        flood_delta_u64(expected_deliveries, stats->broadcast_deliveries_attempted);
    intmax_t enqueued_minus_observed =
        flood_delta_u64(stats->broadcast_deliveries_enqueued, observed_deliveries);
    intmax_t explained_missing =
        expected_minus_attempted + (intmax_t)outbox_drops + enqueued_minus_observed;
    intmax_t accounting_gap = (intmax_t)missing_deliveries - explained_missing;
    double drop_explained_ratio =
        missing_deliveries > 0U ? (double)outbox_drops / (double)missing_deliveries : 0.0;

    printf("server flood accounting: expected_minus_attempted=%" PRIdMAX
           " outbox_drops=%" PRIu64
           " enqueued_minus_observed=%" PRIdMAX
           " explained_missing=%" PRIdMAX
           " accounting_gap=%" PRIdMAX
           " drop_explained_ratio=%.9f\n",
           expected_minus_attempted,
           outbox_drops,
           enqueued_minus_observed,
           explained_missing,
           accounting_gap,
           drop_explained_ratio);
    return accounting_gap;
}

intmax_t flood_accounting_tolerance(uint64_t expected_deliveries) {
    uint64_t scaled = expected_deliveries / 1000000U;

    if (scaled < 4096U) {
        scaled = 4096U;
    }
    if (scaled > 100000U) {
        scaled = 100000U;
    }
    return (intmax_t)scaled;
}

#endif
