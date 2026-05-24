/**
 * @file examples/diagnostic_output.c
 * @brief Race-resistant diagnostic file opening for POSIX examples.
 *
 * @details
 * Diagnostic paths may be supplied by CI environment variables.  Traverse
 * parent directories with openat()+O_NOFOLLOW so a workspace-owned symlink
 * cannot redirect dumps or stats outside the intended artifact directory.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "diagnostic_output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef O_CLOEXEC
#define LLAM_EXAMPLE_OPEN_CLOEXEC O_CLOEXEC
#else
#define LLAM_EXAMPLE_OPEN_CLOEXEC 0
#endif

#ifdef O_DIRECTORY
#define LLAM_EXAMPLE_OPEN_DIRECTORY O_DIRECTORY
#else
#define LLAM_EXAMPLE_OPEN_DIRECTORY 0
#endif

#ifdef O_NOFOLLOW
#define LLAM_EXAMPLE_OPEN_NOFOLLOW O_NOFOLLOW
#define LLAM_EXAMPLE_OPEN_NOFOLLOW_MISSING 0
#else
#define LLAM_EXAMPLE_OPEN_NOFOLLOW 0
#define LLAM_EXAMPLE_OPEN_NOFOLLOW_MISSING 1
#endif

#ifdef ENOTSUP
#define LLAM_EXAMPLE_OPEN_UNSUPPORTED_ERRNO ENOTSUP
#else
#define LLAM_EXAMPLE_OPEN_UNSUPPORTED_ERRNO EINVAL
#endif

static char *diag_dup_range(const char *value, size_t len) {
    char *copy = malloc(len + 1U);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}

#if defined(__APPLE__)
static bool diag_component_matches(const char *path, const char *component) {
    size_t len = strlen(component);

    return strncmp(path, component, len) == 0 && (path[len] == '\0' || path[len] == '/');
}
#endif

static char *diag_rewrite_darwin_root_alias(const char *path) {
#if defined(__APPLE__)
    struct alias_entry {
        const char *prefix;
        const char *resolved;
        const char *target_a;
        const char *target_b;
    };
    static const struct alias_entry aliases[] = {
        {"/var", "/private/var", "private/var", "/private/var"},
        {"/tmp", "/private/tmp", "private/tmp", "/private/tmp"},
        {"/etc", "/private/etc", "private/etc", "/private/etc"},
    };

    for (size_t i = 0U; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        char target[128];
        ssize_t len;

        if (!diag_component_matches(path, aliases[i].prefix)) {
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
     * Non-Darwin platforms do not expose the /var, /tmp, and /etc symlink
     * aliases that need normalization before O_NOFOLLOW traversal.
     */
    (void)path;
#endif
    return NULL;
}

static int diag_open_directory_component(int dirfd, const char *component) {
    int fd = openat(dirfd,
                    component,
                    O_RDONLY | LLAM_EXAMPLE_OPEN_DIRECTORY | LLAM_EXAMPLE_OPEN_NOFOLLOW |
                        LLAM_EXAMPLE_OPEN_CLOEXEC);
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

static int diag_open_parent_dir(const char *path, const char **leaf_out, char **rewritten_out) {
    char *rewritten = NULL;
    char *work;
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
    rewritten = diag_rewrite_darwin_root_alias(path);
    work = rewritten != NULL ? rewritten : (char *)path;

    if (work[0] == '/') {
        dirfd = open("/", O_RDONLY | LLAM_EXAMPLE_OPEN_DIRECTORY | LLAM_EXAMPLE_OPEN_CLOEXEC);
        cursor = work + 1;
    } else {
        dirfd = open(".", O_RDONLY | LLAM_EXAMPLE_OPEN_DIRECTORY | LLAM_EXAMPLE_OPEN_CLOEXEC);
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
        part = diag_dup_range(cursor, part_len);
        if (part == NULL) {
            int saved_errno = ENOMEM;

            close(dirfd);
            free(rewritten);
            errno = saved_errno;
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
            int next_fd = diag_open_directory_component(dirfd, part);

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

int llam_example_open_append_regular(const char *path) {
    const char *leaf = NULL;
    char *rewritten = NULL;
    int parent_fd;
    int fd;
    struct stat st;

#if LLAM_EXAMPLE_OPEN_NOFOLLOW_MISSING
    /*
     * Without O_NOFOLLOW the helper cannot keep its "no symlink traversal"
     * contract for attacker-controlled diagnostic paths.  Fail closed rather
     * than silently downgrading to a best-effort open().
     */
    (void)path;
    errno = LLAM_EXAMPLE_OPEN_UNSUPPORTED_ERRNO;
    return -1;
#endif

    parent_fd = diag_open_parent_dir(path, &leaf, &rewritten);
    if (parent_fd < 0) {
        return -1;
    }
    fd = openat(parent_fd,
                leaf,
                O_WRONLY | O_CREAT | O_APPEND | LLAM_EXAMPLE_OPEN_NOFOLLOW | LLAM_EXAMPLE_OPEN_CLOEXEC,
                0644);
    close(parent_fd);
    free(rewritten);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (st.st_nlink > 1) {
        close(fd);
        errno = EMLINK;
        return -1;
    }
    return fd;
}
