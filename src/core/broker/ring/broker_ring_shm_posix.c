/**
 * @file src/core/broker/ring/broker_ring_shm_posix.c
 * @brief POSIX broker ring shared-memory and fd mapping backend.
 *
 * @details
 * POSIX ring authority is represented by shm names for compatibility or by
 * private fds passed over SCM_RIGHTS for hardened broker setup. This file keeps
 * those ownership rules separate from the platform-neutral ring helpers.
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

#include "runtime_internal.h"
#include "runtime_broker.h"
#include "runtime_broker_ring.h"

#if !LLAM_PLATFORM_WINDOWS

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int llam_broker_ring_shm_open_cloexec(const char *name, int oflag, mode_t mode) {
    int fd;

#if defined(O_CLOEXEC)
    /*
     * Ring mappings can carry data-plane authority.  Prefer atomic close-on-exec
     * creation/open so multi-threaded embedders do not expose a transient
     * inheritable shm fd between shm_open() and the fallback fcntl() guard.
     */
    fd = shm_open(name, oflag | O_CLOEXEC, mode);
    if (fd >= 0) {
        return fd;
    }
    if (errno != EINVAL) {
        return -1;
    }
#endif
    return shm_open(name, oflag, mode);
}

static int llam_broker_ring_set_cloexec_fd(int fd) {
    int flags;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int llam_broker_ring_map_posix_fd(const char *name,
                                         int fd,
                                         bool owner,
                                         llam_broker_ring_mapping_t *out_mapping) {
    void *addr;
    size_t bytes = sizeof(llam_broker_ring_t);

    if (llam_broker_ring_set_cloexec_fd(fd) != 0) {
        return -1;
    }
    addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        return -1;
    }
    llam_broker_ring_mapping_reset(out_mapping);
    out_mapping->ring = (llam_broker_ring_t *)addr;
    out_mapping->bytes = bytes;
    out_mapping->fd = fd;
    out_mapping->mapping_handle = LLAM_INVALID_HANDLE;
    out_mapping->owner = owner;
    (void)snprintf(out_mapping->name, sizeof(out_mapping->name), "%s", name);
    return 0;
}

int llam_broker_ring_map_fd(int fd, bool take_ownership, llam_broker_ring_mapping_t *out_mapping) {
    int mapped_fd = fd;
    struct stat st;
    size_t bytes = sizeof(llam_broker_ring_t);

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (LLAM_UNLIKELY(fd < 0 || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;

        if (take_ownership) {
            close(fd);
        }
        errno = saved_errno;
        return -1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size < (uint64_t)bytes) {
        if (take_ownership) {
            close(fd);
        }
        errno = EINVAL;
        return -1;
    }
    if (!take_ownership) {
        mapped_fd = llam_broker_dup_cloexec_fd(fd);
        if (mapped_fd < 0) {
            return -1;
        }
    }
    if (llam_broker_ring_map_posix_fd("", mapped_fd, false, out_mapping) != 0) {
        int saved_errno = errno;

        /*
         * take_ownership transfers fd lifetime to this import call even when
         * validation or mmap fails. Otherwise a malformed broker response can
         * force the caller to leak descriptors on repeated failed imports.
         */
        close(mapped_fd);
        errno = saved_errno;
        return -1;
    }
    if (!llam_broker_ring_mapping_ring_valid(out_mapping->ring)) {
        llam_broker_ring_unmap(out_mapping);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_broker_ring_create_shm(const char *name, llam_broker_ring_mapping_t *out_mapping) {
    int fd;
    size_t bytes = sizeof(llam_broker_ring_t);

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_name_valid(name) || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }

    fd = llam_broker_ring_shm_open_cloexec(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        int saved_errno = errno;

        close(fd);
        (void)shm_unlink(name);
        errno = saved_errno;
        return -1;
    }
    if (llam_broker_ring_map_posix_fd(name, fd, true, out_mapping) != 0) {
        int saved_errno = errno;

        close(fd);
        (void)shm_unlink(name);
        errno = saved_errno;
        return -1;
    }
    if (llam_broker_ring_init(out_mapping->ring) != 0) {
        int saved_errno = errno;

        llam_broker_ring_unmap(out_mapping);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int llam_broker_ring_create_private_fd(llam_broker_ring_mapping_t *out_mapping) {
    char name[sizeof(((llam_broker_ring_mapping_t *)0)->name)];
    size_t attempt;

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (LLAM_UNLIKELY(out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Create through POSIX shm for portability, then unlink immediately. The
     * returned fd is the rendezvous authority and can be passed over SCM_RIGHTS;
     * no reusable name remains for another same-UID process to open.
     */
    for (attempt = 0U; attempt < 8U; ++attempt) {
        int fd;
        size_t bytes = sizeof(llam_broker_ring_t);

        if (llam_broker_ring_private_name(name, sizeof(name)) != 0) {
            return -1;
        }
        fd = llam_broker_ring_shm_open_cloexec(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            return -1;
        }
        if (shm_unlink(name) != 0) {
            int saved_errno = errno;

            close(fd);
            (void)shm_unlink(name);
            errno = saved_errno;
            return -1;
        }
        if (ftruncate(fd, (off_t)bytes) != 0) {
            int saved_errno = errno;

            close(fd);
            errno = saved_errno;
            return -1;
        }
        if (llam_broker_ring_map_posix_fd("", fd, true, out_mapping) != 0) {
            int saved_errno = errno;

            close(fd);
            errno = saved_errno;
            return -1;
        }
        if (llam_broker_ring_init(out_mapping->ring) != 0) {
            int saved_errno = errno;

            llam_broker_ring_unmap(out_mapping);
            errno = saved_errno;
            return -1;
        }
        return 0;
    }
    errno = EEXIST;
    return -1;
}

int llam_broker_ring_open_shm(const char *name, llam_broker_ring_mapping_t *out_mapping) {
    int fd;
    struct stat st;
    size_t bytes = sizeof(llam_broker_ring_t);

    if (out_mapping != NULL) {
        llam_broker_ring_mapping_reset(out_mapping);
    }
    if (LLAM_UNLIKELY(!llam_broker_ring_name_valid(name) || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }

    fd = llam_broker_ring_shm_open_cloexec(name, O_RDWR, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size < (uint64_t)bytes) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_ring_map_posix_fd(name, fd, false, out_mapping) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!llam_broker_ring_mapping_ring_valid(out_mapping->ring)) {
        llam_broker_ring_unmap(out_mapping);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void llam_broker_ring_unmap(llam_broker_ring_mapping_t *mapping) {
    bool owner;
    char name[sizeof(((llam_broker_ring_mapping_t *)0)->name)];

    if (mapping == NULL) {
        return;
    }
    owner = mapping->owner;
    (void)snprintf(name, sizeof(name), "%s", mapping->name);
    if (mapping->ring != NULL && mapping->bytes != 0U) {
        (void)munmap(mapping->ring, mapping->bytes);
    }
    if (mapping->fd >= 0) {
        (void)close(mapping->fd);
    }
    if (owner && name[0] != '\0') {
        (void)shm_unlink(name);
    }
    llam_broker_ring_mapping_reset(mapping);
}

#endif
