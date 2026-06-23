/**
 * @file src/io/api/blocking_wrappers.c
 * @brief Public blocking syscall wrappers for DNS and filesystem metadata.
 *
 * @details
 * getaddrinfo/open/stat are allowed to block in libc, kernel filesystems, or
 * network-backed mounts. These wrappers route the operation through
 * ::llam_call_blocking_result so managed tasks park cooperatively instead of
 * pinning a scheduler worker. The wrappers are intentionally thin: they expose
 * stable LLAM result contracts without pretending these operations are native
 * async syscalls on every platform.
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

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#if LLAM_PLATFORM_POSIX
#include <netdb.h>
#include <unistd.h>
#endif

#if LLAM_RUNTIME_BACKEND_WINDOWS
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif
#ifndef O_APPEND
#define O_APPEND _O_APPEND
#endif
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif
#endif

typedef struct llam_getaddrinfo_call {
    const char *node;
    const char *service;
    const struct addrinfo *hints;
    struct addrinfo *result;
    int gai_error;
} llam_getaddrinfo_call_t;

typedef struct llam_open_call {
    const char *path;
    int flags;
    uint32_t mode;
    llam_handle_t handle;
    int error_code;
} llam_open_call_t;

typedef struct llam_stat_call {
    const char *path;
    llam_file_stat_t stat;
    int error_code;
} llam_stat_call_t;

static void *llam_blocking_getaddrinfo_call(void *arg) {
    llam_getaddrinfo_call_t *call = arg;

    if (call == NULL) {
        return NULL;
    }
    call->result = NULL;
    call->gai_error = getaddrinfo(call->node, call->service, call->hints, &call->result);
    return call;
}

int llam_getaddrinfo_result(const char *node,
                            const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **out,
                            int *gai_error) {
    llam_getaddrinfo_call_t call;
    void *ignored = NULL;

    if (out == NULL || gai_error == NULL || (node == NULL && service == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    *gai_error = 0;
    memset(&call, 0, sizeof(call));
    call.node = node;
    call.service = service;
    call.hints = hints;
    if (llam_call_blocking_result(llam_blocking_getaddrinfo_call, &call, &ignored) != 0) {
        return -1;
    }
    *gai_error = call.gai_error;
    if (call.gai_error == 0) {
        *out = call.result;
    }
    return 0;
}

void llam_freeaddrinfo_result(struct addrinfo *result) {
    if (result != NULL) {
        freeaddrinfo(result);
    }
}

#if LLAM_RUNTIME_BACKEND_WINDOWS
static DWORD llam_windows_open_access_from_flags(int flags) {
    DWORD access = 0;

    switch (flags & O_ACCMODE) {
    case O_WRONLY:
        access = GENERIC_WRITE;
        break;
    case O_RDWR:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    case O_RDONLY:
    default:
        access = GENERIC_READ;
        break;
    }
    if ((flags & O_APPEND) != 0) {
        access &= ~GENERIC_WRITE;
        access |= FILE_APPEND_DATA;
    }
    return access;
}

static DWORD llam_windows_open_disposition_from_flags(int flags) {
    if ((flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
        return CREATE_NEW;
    }
    if ((flags & O_CREAT) != 0 && (flags & O_TRUNC) != 0) {
        return CREATE_ALWAYS;
    }
    if ((flags & O_CREAT) != 0) {
        return OPEN_ALWAYS;
    }
    if ((flags & O_TRUNC) != 0) {
        return TRUNCATE_EXISTING;
    }
    return OPEN_EXISTING;
}
#endif

static void *llam_blocking_open_call(void *arg) {
    llam_open_call_t *call = arg;

    if (call == NULL) {
        return NULL;
    }
    call->handle = LLAM_INVALID_HANDLE;
    call->error_code = 0;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        HANDLE handle = CreateFileA(call->path,
                                    llam_windows_open_access_from_flags(call->flags),
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL,
                                    llam_windows_open_disposition_from_flags(call->flags),
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);

        if (handle == INVALID_HANDLE_VALUE) {
            call->error_code = llam_windows_system_error_to_errno(GetLastError());
            return call;
        }
        call->handle = (llam_handle_t)handle;
        return call;
    }
#elif LLAM_PLATFORM_POSIX
    {
        int fd = open(call->path, call->flags, (mode_t)call->mode);

        if (fd < 0) {
            call->error_code = errno;
            return call;
        }
        call->handle = (llam_handle_t)fd;
        return call;
    }
#else
    call->error_code = ENOTSUP;
    return call;
#endif
}

int llam_open_async(const char *path, int flags, uint32_t mode, llam_handle_t *out) {
    llam_open_call_t call;
    void *ignored = NULL;

    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = LLAM_INVALID_HANDLE;
    memset(&call, 0, sizeof(call));
    call.path = path;
    call.flags = flags;
    call.mode = mode;
    call.handle = LLAM_INVALID_HANDLE;
    if (llam_call_blocking_result(llam_blocking_open_call, &call, &ignored) != 0) {
        return -1;
    }
    if (call.error_code != 0) {
        errno = call.error_code;
        return -1;
    }
    *out = call.handle;
    return 0;
}

static uint64_t llam_unix_seconds_to_ns(int64_t seconds) {
    if (seconds <= 0) {
        return 0U;
    }
    if ((uint64_t)seconds > UINT64_MAX / 1000000000ULL) {
        return UINT64_MAX;
    }
    return (uint64_t)seconds * 1000000000ULL;
}

static uint32_t llam_file_type_from_mode(uint32_t mode) {
#if LLAM_PLATFORM_POSIX
    if (S_ISREG((mode_t)mode)) {
        return LLAM_FILE_TYPE_REGULAR;
    }
    if (S_ISDIR((mode_t)mode)) {
        return LLAM_FILE_TYPE_DIRECTORY;
    }
#ifdef S_ISLNK
    if (S_ISLNK((mode_t)mode)) {
        return LLAM_FILE_TYPE_SYMLINK;
    }
#endif
#else
    (void)mode;
#endif
    return LLAM_FILE_TYPE_OTHER;
}

#if LLAM_RUNTIME_BACKEND_WINDOWS
static uint64_t llam_windows_filetime_to_unix_ns(const FILETIME *filetime) {
    ULARGE_INTEGER value;
    uint64_t ticks;

    if (filetime == NULL) {
        return 0U;
    }
    value.LowPart = filetime->dwLowDateTime;
    value.HighPart = filetime->dwHighDateTime;
    if (value.QuadPart < UINT64_C(116444736000000000)) {
        return 0U;
    }
    ticks = value.QuadPart - UINT64_C(116444736000000000);
    if (ticks > UINT64_MAX / 100ULL) {
        return UINT64_MAX;
    }
    return ticks * 100ULL;
}
#endif

static void *llam_blocking_stat_call(void *arg) {
    llam_stat_call_t *call = arg;

    if (call == NULL) {
        return NULL;
    }
    memset(&call->stat, 0, sizeof(call->stat));
    call->error_code = 0;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        uint64_t high;
        uint64_t low;

        if (!GetFileAttributesExA(call->path, GetFileExInfoStandard, &data)) {
            call->error_code = llam_windows_system_error_to_errno(GetLastError());
            return call;
        }
        high = ((uint64_t)data.nFileSizeHigh) << 32U;
        low = (uint64_t)data.nFileSizeLow;
        call->stat.size = high | low;
        call->stat.mtime_ns = llam_windows_filetime_to_unix_ns(&data.ftLastWriteTime);
        call->stat.atime_ns = llam_windows_filetime_to_unix_ns(&data.ftLastAccessTime);
        call->stat.ctime_ns = llam_windows_filetime_to_unix_ns(&data.ftCreationTime);
        call->stat.mode = data.dwFileAttributes;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            call->stat.type = LLAM_FILE_TYPE_DIRECTORY;
        } else if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            call->stat.type = LLAM_FILE_TYPE_SYMLINK;
        } else {
            call->stat.type = LLAM_FILE_TYPE_REGULAR;
        }
        return call;
    }
#elif LLAM_PLATFORM_POSIX
    {
        struct stat st;

        if (stat(call->path, &st) != 0) {
            call->error_code = errno;
            return call;
        }
        call->stat.size = st.st_size >= 0 ? (uint64_t)st.st_size : 0U;
        call->stat.mtime_ns = llam_unix_seconds_to_ns((int64_t)st.st_mtime);
        call->stat.atime_ns = llam_unix_seconds_to_ns((int64_t)st.st_atime);
        call->stat.ctime_ns = llam_unix_seconds_to_ns((int64_t)st.st_ctime);
        call->stat.mode = (uint32_t)st.st_mode;
        call->stat.type = llam_file_type_from_mode((uint32_t)st.st_mode);
        return call;
    }
#else
    call->error_code = ENOTSUP;
    return call;
#endif
}

int llam_stat_path_ex(const char *path, llam_file_stat_t *out, size_t out_size) {
    llam_stat_call_t call;
    void *ignored = NULL;
    size_t copy_size;

    if (path == NULL || out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    copy_size = out_size < sizeof(call.stat) ? out_size : sizeof(call.stat);
    memset(out, 0, copy_size);
    memset(&call, 0, sizeof(call));
    call.path = path;
    if (llam_call_blocking_result(llam_blocking_stat_call, &call, &ignored) != 0) {
        return -1;
    }
    if (call.error_code != 0) {
        errno = call.error_code;
        return -1;
    }
    memcpy(out, &call.stat, copy_size);
    return 0;
}
