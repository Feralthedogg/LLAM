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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef O_CLOEXEC
#define FLOOD_OPEN_CLOEXEC O_CLOEXEC
#else
#define FLOOD_OPEN_CLOEXEC 0
#endif

#ifdef O_DIRECTORY
#define FLOOD_OPEN_DIRECTORY O_DIRECTORY
#else
#define FLOOD_OPEN_DIRECTORY 0
#endif

#ifdef O_NOFOLLOW
#define FLOOD_OPEN_NOFOLLOW O_NOFOLLOW
#define FLOOD_OPEN_NOFOLLOW_MISSING 0
#else
#define FLOOD_OPEN_NOFOLLOW 0
#define FLOOD_OPEN_NOFOLLOW_MISSING 1
#endif

#ifdef ENOTSUP
#define FLOOD_OPEN_UNSUPPORTED_ERRNO ENOTSUP
#else
#define FLOOD_OPEN_UNSUPPORTED_ERRNO EINVAL
#endif

static char *flood_dup_range(const char *value, size_t len) {
    char *copy = malloc(len + 1U);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}

#if defined(__APPLE__)
static bool flood_component_matches(const char *path, const char *component) {
    size_t len = strlen(component);

    return strncmp(path, component, len) == 0 && (path[len] == '\0' || path[len] == '/');
}
#endif

static char *flood_rewrite_darwin_root_alias(const char *path) {
#if defined(__APPLE__)
    struct flood_alias_entry {
        const char *prefix;
        const char *resolved;
        const char *target_a;
        const char *target_b;
    };
    static const struct flood_alias_entry aliases[] = {
        {"/var", "/private/var", "private/var", "/private/var"},
        {"/tmp", "/private/tmp", "private/tmp", "/private/tmp"},
        {"/etc", "/private/etc", "private/etc", "/private/etc"},
    };

    for (size_t i = 0U; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        char target[128];
        ssize_t len;

        if (!flood_component_matches(path, aliases[i].prefix)) {
            continue;
        }
        len = readlink(aliases[i].prefix, target, sizeof(target) - 1U);
        if (len < 0) {
            break;
        }
        target[len] = '\0';
        if (strcmp(target, aliases[i].target_a) == 0 || strcmp(target, aliases[i].target_b) == 0) {
            size_t prefix_len = strlen(aliases[i].prefix);
            size_t resolved_len = strlen(aliases[i].resolved);
            size_t suffix_len = strlen(path + prefix_len);
            char *rewritten = malloc(resolved_len + suffix_len + 1U);

            if (rewritten == NULL) {
                return NULL;
            }
            memcpy(rewritten, aliases[i].resolved, resolved_len);
            memcpy(rewritten + resolved_len, path + prefix_len, suffix_len + 1U);
            return rewritten;
        }
    }
#else
    /*
     * Linux/BSD paths are traversed directly with O_NOFOLLOW.  Only Darwin's
     * root aliases need rewrite before per-component symlink checks.
     */
    (void)path;
#endif
    return NULL;
}

static int flood_open_directory_component(int dirfd, const char *component) {
    int fd = openat(dirfd, component, O_RDONLY | FLOOD_OPEN_DIRECTORY | FLOOD_OPEN_NOFOLLOW | FLOOD_OPEN_CLOEXEC);
    struct stat st;

    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return -1;
    }
    return fd;
}

static int flood_open_stats_parent_dir(const char *path, const char **leaf_out, char **rewritten_out) {
    char *rewritten = NULL;
    const char *work;
    const char *cursor;
    int dirfd;

    if (path == NULL || path[0] == '\0' || leaf_out == NULL || rewritten_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (path[strlen(path) - 1U] == '/') {
        errno = EINVAL;
        return -1;
    }

    rewritten = flood_rewrite_darwin_root_alias(path);
    work = rewritten != NULL ? rewritten : path;
    if (work[0] == '/') {
        dirfd = open("/", O_RDONLY | FLOOD_OPEN_DIRECTORY | FLOOD_OPEN_CLOEXEC);
        cursor = work + 1;
    } else {
        dirfd = open(".", O_RDONLY | FLOOD_OPEN_DIRECTORY | FLOOD_OPEN_CLOEXEC);
        cursor = work;
    }
    if (dirfd < 0) {
        free(rewritten);
        return -1;
    }

    for (;;) {
        const char *slash = strchr(cursor, '/');
        size_t part_len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        char *part;

        if (part_len == 0U) {
            cursor = slash != NULL ? slash + 1 : cursor + strlen(cursor);
            continue;
        }
        part = flood_dup_range(cursor, part_len);
        if (part == NULL) {
            close(dirfd);
            free(rewritten);
            errno = ENOMEM;
            return -1;
        }
        if (strcmp(part, ".") == 0) {
            free(part);
            if (slash == NULL) {
                close(dirfd);
                free(rewritten);
                errno = EINVAL;
                return -1;
            }
            cursor = slash + 1;
            continue;
        }
        if (strcmp(part, "..") == 0) {
            free(part);
            close(dirfd);
            free(rewritten);
            errno = EINVAL;
            return -1;
        }
        if (slash == NULL) {
            *leaf_out = cursor;
            *rewritten_out = rewritten;
            free(part);
            return dirfd;
        }

        {
            int next_fd = flood_open_directory_component(dirfd, part);

            free(part);
            if (next_fd < 0) {
                int saved_errno = errno;

                close(dirfd);
                free(rewritten);
                errno = saved_errno;
                return -1;
            }
            close(dirfd);
            dirfd = next_fd;
        }
        cursor = slash + 1;
    }
}

static bool flood_parse_u64_field(const char *line, const char *key, uint64_t *out) {
    const char *cursor = strstr(line, key);
    char *end = NULL;
    unsigned long long parsed;

    if (cursor == NULL || out == NULL) {
        return false;
    }
    cursor += strlen(key);
    if (*cursor != '=') {
        return false;
    }
    cursor += 1;
    errno = 0;
    parsed = strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor) {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static FILE *flood_open_stats_file(const char *stats_path) {
    const char *leaf = NULL;
    char *rewritten = NULL;
    int parent_fd;
    int fd;
    struct stat st;
    FILE *file;

    if (stats_path == NULL || stats_path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
#if FLOOD_OPEN_NOFOLLOW_MISSING
    errno = FLOOD_OPEN_UNSUPPORTED_ERRNO;
    return NULL;
#endif
    /*
     * The server process is not trusted for accounting.  It receives the stats
     * path through the environment and can replace the private parent
     * directory with a symlink before shutdown.  Resolve each parent component
     * with O_NOFOLLOW, then open only the final leaf.
     */
    parent_fd = flood_open_stats_parent_dir(stats_path, &leaf, &rewritten);
    if (parent_fd < 0) {
        return NULL;
    }
    fd = openat(parent_fd, leaf, O_RDONLY | FLOOD_OPEN_NOFOLLOW | FLOOD_OPEN_CLOEXEC);
    close(parent_fd);
    free(rewritten);
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    if (st.st_nlink > 1U) {
        close(fd);
        errno = EMLINK;
        return NULL;
    }
    file = fdopen(fd, "r");
    if (file == NULL) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return NULL;
    }
    return file;
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
