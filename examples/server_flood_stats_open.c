/**
 * @file examples/server_flood_stats_open.c
 * @brief Hardened stats-file opening for server_flood.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "server_flood_stats_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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

FILE *flood_open_stats_file(const char *stats_path) {
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

#endif
