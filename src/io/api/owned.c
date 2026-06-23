/**
 * @file src/io/api/owned.c
 * @brief Public owned I/O buffer wrappers and accessors.
 *
 * @details
 * Owned-buffer APIs are grouped away from descriptor read/write entry points so
 * their registry lifetime rules stay visible: public handles are consumed by
 * release, scalar accessors pin the wrapper while copying fields, and the data
 * accessor returns a borrowed pointer that callers must serialize against
 * release when sharing a buffer between threads.
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

#include "io/runtime_io_api_internal.h"

#if LLAM_PLATFORM_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool llam_io_buffer_alignment_valid(size_t alignment) {
    return alignment == 0U || (alignment >= sizeof(void *) && (alignment & (alignment - 1U)) == 0U);
}

static size_t llam_io_buffer_normalize_alignment(size_t alignment) {
    return alignment != 0U ? alignment : sizeof(void *);
}

static void llam_io_buffer_external_free(llam_io_buffer_t *buffer) {
    if (buffer == NULL || buffer->data == NULL) {
        return;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (buffer->aligned_storage) {
        _aligned_free(buffer->data);
        return;
    }
#endif
    free(buffer->data);
}

llam_io_buffer_t *llam_io_buffer_alloc_detached(size_t min_capacity, size_t alignment, uint32_t flags) {
    llam_io_buffer_t *buffer;
    size_t normalized_alignment;
    bool needs_external;

    if (!llam_io_buffer_alignment_valid(alignment) ||
        (flags & ~LLAM_IO_BUFFER_F_ZERO_FILL) != 0U) {
        errno = EINVAL;
        return NULL;
    }
    normalized_alignment = llam_io_buffer_normalize_alignment(alignment);
    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->detached_wrapper = true;
    buffer->data = buffer->inline_data;
    buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
    buffer->alignment = sizeof(void *);
    needs_external = min_capacity > LLAM_IO_BUFFER_INLINE_BYTES || normalized_alignment > sizeof(void *);
    if (needs_external) {
        void *data = NULL;
        size_t alloc_size = min_capacity != 0U ? min_capacity : normalized_alignment;

#if LLAM_RUNTIME_BACKEND_WINDOWS
        data = _aligned_malloc(alloc_size, normalized_alignment);
        if (data == NULL) {
            free(buffer);
            errno = ENOMEM;
            return NULL;
        }
#else
        if (posix_memalign(&data, normalized_alignment, alloc_size) != 0) {
            free(buffer);
            errno = ENOMEM;
            return NULL;
        }
#endif
        if ((flags & LLAM_IO_BUFFER_F_ZERO_FILL) != 0U) {
            memset(data, 0, alloc_size);
        }
        buffer->data = data;
        buffer->capacity = min_capacity;
        buffer->alignment = normalized_alignment;
        buffer->external_storage = true;
        buffer->aligned_storage = true;
    } else if ((flags & LLAM_IO_BUFFER_F_ZERO_FILL) != 0U) {
        memset(buffer->inline_data, 0, sizeof(buffer->inline_data));
    }
    if (llam_io_buffer_public_register(buffer) != 0) {
        if (buffer->external_storage) {
            llam_io_buffer_external_free(buffer);
        }
        free(buffer);
        return NULL;
    }
    return buffer;
}

ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, 0, false, out);
}

ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, flags, true, out);
}

static void llam_io_buffer_release_unregistered(llam_io_buffer_t *buffer) {
    llam_runtime_t *rt = NULL;

    if (buffer->provided_storage &&
        buffer->owner_runtime != NULL &&
        llam_runtime_begin_public_op(buffer->owner_runtime, &rt) == 0) {
        /*
         * Provided-buffer rings are backend-owned.  Pin the owner runtime before
         * looking at nodes so concurrent runtime_destroy() cannot tear down the
         * backend while release is recycling the bid.
         */
        if (atomic_load_explicit(&rt->initialized, memory_order_acquire) &&
            buffer->provided_node_index < rt->active_nodes) {
            (void)llam_node_recycle_recv_buffer(&rt->nodes[buffer->provided_node_index], buffer->provided_bid);
        }
        llam_runtime_end_public_op(rt);
        buffer->provided_storage = false;
        buffer->provided_bid = 0U;
        buffer->provided_node_index = UINT_MAX;
    }

    if (buffer->detached_wrapper) {
        if (buffer->external_storage && buffer->data != NULL) {
            llam_io_buffer_external_free(buffer);
        }
        free(buffer);
        return;
    }

    llam_io_buffer_allocator_free(buffer);
}

void llam_io_buffer_release(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *raw_buffer;

    if (buffer == NULL) {
        return;
    }

    raw_buffer = llam_io_buffer_public_unregister(buffer);
    if (raw_buffer == NULL) {
        return;
    }
    llam_io_buffer_release_unregistered(raw_buffer);
}

void llam_io_buffer_release_raw(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *raw_buffer;

    if (buffer == NULL) {
        return;
    }

    /*
     * Only runtime-owned setup/error paths use raw wrappers before exposing the
     * encoded public handle. The public release API intentionally does not take
     * this path, so guessed raw addresses cannot consume live owned buffers.
     */
    raw_buffer = llam_io_buffer_public_unregister_raw(buffer);
    if (raw_buffer == NULL) {
        return;
    }
    llam_io_buffer_release_unregistered(raw_buffer);
}

void *llam_io_buffer_data(llam_io_buffer_t *buffer) {
    return llam_io_buffer_public_data(buffer);
}

size_t llam_io_buffer_size(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    size_t size;

    if (live == NULL) {
        return 0U;
    }
    size = live->size;
    llam_io_buffer_public_end_op(live);
    return size;
}

size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    size_t capacity;

    if (live == NULL) {
        return 0U;
    }
    capacity = live->capacity;
    llam_io_buffer_public_end_op(live);
    return capacity;
}

int llam_io_buffer_opts_init(llam_io_buffer_opts_t *opts, size_t opts_size) {
    llam_io_buffer_opts_t defaults;
    size_t copy_size;

    if (opts == NULL || opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(&defaults, 0, sizeof(defaults));
    copy_size = opts_size < sizeof(defaults) ? opts_size : sizeof(defaults);
    /* Only initialize the ABI prefix known to this library; future tail bytes
     * belong to the caller's newer struct definition. */
    memset(opts, 0, copy_size);
    memcpy(opts, &defaults, copy_size);
    return 0;
}

int llam_io_buffer_alloc_ex(const llam_io_buffer_opts_t *opts, size_t opts_size, llam_io_buffer_t **out) {
    llam_io_buffer_opts_t normalized;
    llam_io_buffer_t *buffer;
    llam_io_buffer_t *handle;
    size_t copy_size;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    memset(&normalized, 0, sizeof(normalized));
    if (opts != NULL) {
        if (opts_size == 0U) {
            errno = EINVAL;
            return -1;
        }
        copy_size = opts_size < sizeof(normalized) ? opts_size : sizeof(normalized);
        memcpy(&normalized, opts, copy_size);
    }
    buffer = llam_io_buffer_alloc_detached(normalized.capacity, normalized.alignment, normalized.flags);
    if (buffer == NULL) {
        return -1;
    }
    handle = llam_io_buffer_public_handle(buffer);
    if (handle == NULL) {
        llam_io_buffer_release_raw(buffer);
        errno = ENOMEM;
        return -1;
    }
    *out = handle;
    return 0;
}

int llam_io_buffer_alloc(size_t capacity, llam_io_buffer_t **out) {
    llam_io_buffer_opts_t opts;

    opts.capacity = capacity;
    opts.alignment = 0U;
    opts.flags = 0U;
    opts.reserved0 = 0U;
    return llam_io_buffer_alloc_ex(&opts, sizeof(opts), out);
}

int llam_io_buffer_alloc_aligned(size_t capacity, size_t alignment, llam_io_buffer_t **out) {
    llam_io_buffer_opts_t opts;

    opts.capacity = capacity;
    opts.alignment = alignment;
    opts.flags = 0U;
    opts.reserved0 = 0U;
    return llam_io_buffer_alloc_ex(&opts, sizeof(opts), out);
}

size_t llam_io_buffer_alignment(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    size_t alignment;

    if (live == NULL) {
        return 0U;
    }
    alignment = live->alignment;
    llam_io_buffer_public_end_op(live);
    return alignment;
}

static int llam_pread_owned_validate_target_before_alloc(bool handle_mode, llam_handle_t handle, llam_fd_t fd) {
#if LLAM_PLATFORM_POSIX
    int saved_errno = errno;
    llam_fd_t probe_fd = handle_mode ? (llam_fd_t)handle : fd;
    struct stat st;
    off_t current;
    int flags;

    /*
     * Positional owned reads allocate the destination up front. Probe descriptor
     * access and seekability first so fd errors report their native errno
     * instead of being masked by a hostile allocation request.
     */
    flags = fcntl(probe_fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return -1;
    }
    if (fstat(probe_fd, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    current = lseek(probe_fd, 0, SEEK_CUR);
    if (current == (off_t)-1) {
        return -1;
    }
    errno = saved_errno;
    return 0;
#elif LLAM_RUNTIME_BACKEND_WINDOWS
    DWORD file_type;
    DWORD error_code;
    int saved_errno = errno;

    if (!handle_mode) {
        /*
         * Windows positional file I/O is exposed through llam_handle_t.  Reject
         * the fd-flavoured API before allocation instead of allocating and then
         * returning ENOTSUP from the backend shim.
         */
        (void)fd;
        errno = ENOTSUP;
        return -1;
    }
    file_type = GetFileType((HANDLE)handle);
    if (file_type == FILE_TYPE_UNKNOWN) {
        error_code = GetLastError();
        if (error_code != NO_ERROR) {
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
    }
    errno = saved_errno;
    return 0;
#else
    (void)handle;
    (void)handle_mode;
    (void)fd;
    errno = ENOTSUP;
    return -1;
#endif
}

static ssize_t llam_pread_owned_aligned_impl(bool handle_mode,
                                             llam_handle_t handle,
                                             llam_fd_t fd,
                                             size_t max_count,
                                             uint64_t offset,
                                             size_t alignment,
                                             llam_io_buffer_t **out) {
    llam_io_buffer_t *buffer;
    llam_io_buffer_t *public_handle;
    ssize_t bytes;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (llam_positional_validate_async_rw_count(max_count) != 0) {
        return -1;
    }
    if (!llam_io_buffer_alignment_valid(alignment)) {
        errno = EINVAL;
        return -1;
    }
    if (max_count == 0U) {
        return 0;
    }
    /*
     * Owned positional reads allocate the destination buffer before issuing the
     * syscall. Reject obvious invalid sentinels first so huge counts cannot
     * turn a descriptor/handle error into an allocation failure.
     */
    if (!handle_mode && LLAM_FD_IS_INVALID(fd)) {
        errno = EBADF;
        return -1;
    }
    if (handle_mode && LLAM_HANDLE_IS_INVALID(handle)) {
#if LLAM_PLATFORM_POSIX
        errno = EBADF;
#else
        errno = EINVAL;
#endif
        return -1;
    }
    if (llam_pread_owned_validate_target_before_alloc(handle_mode, handle, fd) != 0) {
        return -1;
    }
    buffer = llam_io_buffer_alloc_detached(max_count, alignment, 0U);
    if (buffer == NULL) {
        return -1;
    }
    bytes = handle_mode ? llam_pread_handle(handle, buffer->data, max_count, offset) :
                          llam_pread(fd, buffer->data, max_count, offset);
    if (bytes <= 0) {
        int saved_errno = errno;

        llam_io_buffer_release_raw(buffer);
        if (bytes < 0) {
            errno = saved_errno;
        }
        return bytes;
    }
    buffer->size = (size_t)bytes;
    public_handle = llam_io_buffer_public_handle(buffer);
    if (public_handle == NULL) {
        llam_io_buffer_release_raw(buffer);
        errno = ENOMEM;
        return -1;
    }
    *out = public_handle;
    return bytes;
}

ssize_t llam_pread_owned_aligned(llam_fd_t fd,
                                 size_t max_count,
                                 uint64_t offset,
                                 size_t alignment,
                                 llam_io_buffer_t **out) {
    return llam_pread_owned_aligned_impl(false, LLAM_INVALID_HANDLE, fd, max_count, offset, alignment, out);
}

ssize_t llam_pread_handle_owned_aligned(llam_handle_t handle,
                                        size_t max_count,
                                        uint64_t offset,
                                        size_t alignment,
                                        llam_io_buffer_t **out) {
    return llam_pread_owned_aligned_impl(true, handle, LLAM_INVALID_FD, max_count, offset, alignment, out);
}
