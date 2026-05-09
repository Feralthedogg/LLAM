/**
 * @file src/internal/runtime_windows_compat.h
 * @brief Internal POSIX-shaped shims used by the native Windows backend.
 *
 * @details
 * This header is intentionally private. It keeps the first native Windows
 * backend pass source-compatible with the existing scheduler while the hot
 * paths are moved to explicit Windows primitives module by module.
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

#ifndef LLAM_RUNTIME_WINDOWS_COMPAT_H
#define LLAM_RUNTIME_WINDOWS_COMPAT_H

#include "llam/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if LLAM_PLATFORM_WINDOWS
#if defined(_MSC_VER)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#if LLAM_PLATFORM_WINDOWS

#if defined(_MSC_VER)
#define read(fd, buf, count) _read((fd), (buf), (unsigned int)(count))
#define write(fd, buf, count) _write((fd), (const void *)(buf), (unsigned int)(count))
#define close(fd) _close((fd))
#define dup2(oldfd, newfd) _dup2((oldfd), (newfd))
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int llam_windows_clock_gettime(int clock_id, struct timespec *ts) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t ns;

    if (clock_id != CLOCK_MONOTONIC || ts == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (frequency.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
            errno = EINVAL;
            return -1;
        }
    }
    if (!QueryPerformanceCounter(&counter)) {
        errno = EINVAL;
        return -1;
    }
    ns = ((uint64_t)counter.QuadPart * 1000000000ULL) / (uint64_t)frequency.QuadPart;
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}

static inline int llam_windows_nanosleep(const struct timespec *req, struct timespec *rem) {
    uint64_t ns;
    DWORD ms;

    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    ns = (uint64_t)req->tv_sec * 1000000000ULL + (uint64_t)req->tv_nsec;
    ms = (DWORD)((ns + 999999ULL) / 1000000ULL);
    Sleep(ms);
    return 0;
}

#define clock_gettime(clock_id, ts) llam_windows_clock_gettime((clock_id), (ts))
#define nanosleep(req, rem) llam_windows_nanosleep((req), (rem))

#ifndef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif
#ifndef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif
#ifndef EALREADY
#define EALREADY WSAEALREADY
#endif
#ifndef ENOTSOCK
#define ENOTSOCK WSAENOTSOCK
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ WSAEDESTADDRREQ
#endif
#ifndef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE WSAEPROTOTYPE
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT WSAENOPROTOOPT
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP WSAEOPNOTSUPP
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#endif
#ifndef EADDRINUSE
#define EADDRINUSE WSAEADDRINUSE
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL WSAEADDRNOTAVAIL
#endif
#ifndef ENETDOWN
#define ENETDOWN WSAENETDOWN
#endif
#ifndef ENETUNREACH
#define ENETUNREACH WSAENETUNREACH
#endif
#ifndef ENETRESET
#define ENETRESET WSAENETRESET
#endif
#ifndef ECONNABORTED
#define ECONNABORTED WSAECONNABORTED
#endif
#ifndef ECONNRESET
#define ECONNRESET WSAECONNRESET
#endif
#ifndef ENOBUFS
#define ENOBUFS WSAENOBUFS
#endif
#ifndef EISCONN
#define EISCONN WSAEISCONN
#endif
#ifndef ENOTCONN
#define ENOTCONN WSAENOTCONN
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif
#ifndef EDEADLK
#define EDEADLK 36
#endif
#ifndef ENOSYS
#define ENOSYS 38
#endif
#ifndef ENODEV
#define ENODEV 19
#endif

#ifndef POLLIN
#define POLLIN 0x0100
#endif
#ifndef POLLPRI
#define POLLPRI 0x0200
#endif
#ifndef POLLOUT
#define POLLOUT 0x0010
#endif
#ifndef POLLERR
#define POLLERR 0x0001
#endif
#ifndef POLLHUP
#define POLLHUP 0x0002
#endif
#ifndef POLLNVAL
#define POLLNVAL 0x0004
#endif

#ifndef SIGUSR1
#define SIGUSR1 SIGTERM
#endif
#ifndef SIGSTKSZ
#define SIGSTKSZ 8192
#endif
#if defined(_MSC_VER)
typedef int sigset_t;
#define LLAM_WINDOWS_SIGSET_T_DEFINED 1
#endif

static inline int llam_windows_wsa_error_to_errno(int error_code) {
    switch (error_code) {
    case 0:
        return 0;
    case WSA_OPERATION_ABORTED:
        return ECANCELED;
    case WSAEINTR:
        return EINTR;
    case WSAEWOULDBLOCK:
        return EWOULDBLOCK;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEALREADY:
        return EALREADY;
    case WSAENOTSOCK:
        return ENOTSOCK;
    case WSAEDESTADDRREQ:
        return EDESTADDRREQ;
    case WSAEMSGSIZE:
        return EMSGSIZE;
    case WSAEPROTOTYPE:
        return EPROTOTYPE;
    case WSAENOPROTOOPT:
        return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT:
        return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP:
        return EOPNOTSUPP;
    case WSAEAFNOSUPPORT:
        return EAFNOSUPPORT;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAENETDOWN:
        return ENETDOWN;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAENETRESET:
        return ENETRESET;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAENOBUFS:
        return ENOBUFS;
    case WSAEISCONN:
        return EISCONN;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAEHOSTUNREACH:
        return EHOSTUNREACH;
    case WSAEINVAL:
        return EINVAL;
    default:
        return EIO;
    }
}

static inline int llam_windows_system_error_to_errno(unsigned long error_code) {
    switch (error_code) {
    case 0:
        return 0;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case WAIT_TIMEOUT:
        return ETIMEDOUT;
    case ERROR_OPERATION_ABORTED:
        return ECANCELED;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
        return EPIPE;
    case ERROR_IO_PENDING:
        return EAGAIN;
    default:
        return EIO;
    }
}

static inline int llam_windows_poll(struct pollfd *fds, unsigned long nfds, int timeout_ms) {
    int rc = WSAPoll((WSAPOLLFD *)fds, nfds, timeout_ms);

    if (rc == SOCKET_ERROR) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    return rc;
}

#define poll llam_windows_poll

#ifndef LLAM_WINDOWS_SIGSET_T_DEFINED
typedef int sigset_t;
#define LLAM_WINDOWS_SIGSET_T_DEFINED 1
#endif

typedef uintptr_t pthread_t;

typedef struct llam_windows_pthread_mutex {
    SRWLOCK lock;
} pthread_mutex_t;

typedef struct llam_windows_pthread_cond {
    CONDITION_VARIABLE cv;
} pthread_cond_t;

typedef struct llam_windows_pthread_start {
    void *(*fn)(void *);
    void *arg;
} llam_windows_pthread_start_t;

#define PTHREAD_MUTEX_INITIALIZER \
    { SRWLOCK_INIT }

static unsigned __stdcall llam_windows_pthread_entry(void *opaque) {
    llam_windows_pthread_start_t *start = (llam_windows_pthread_start_t *)opaque;

    if (start != NULL) {
        void *(*fn)(void *) = start->fn;
        void *arg = start->arg;

        free(start);
        if (fn != NULL) {
            (void)fn(arg);
        }
    }
    return 0U;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    if (mutex == NULL) {
        return EINVAL;
    }
    InitializeSRWLock(&mutex->lock);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        return EINVAL;
    }
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        return EINVAL;
    }
    AcquireSRWLockExclusive(&mutex->lock);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        return EINVAL;
    }
    return TryAcquireSRWLockExclusive(&mutex->lock) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        return EINVAL;
    }
    ReleaseSRWLockExclusive(&mutex->lock);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cond, const void *attr) {
    (void)attr;
    if (cond == NULL) {
        return EINVAL;
    }
    InitializeConditionVariable(&cond->cv);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond) {
    return cond != NULL ? 0 : EINVAL;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (cond == NULL || mutex == NULL) {
        return EINVAL;
    }
    return SleepConditionVariableSRW(&cond->cv, &mutex->lock, INFINITE, 0U) ? 0 : EINVAL;
}

static inline int pthread_cond_signal(pthread_cond_t *cond) {
    if (cond == NULL) {
        return EINVAL;
    }
    WakeConditionVariable(&cond->cv);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (cond == NULL) {
        return EINVAL;
    }
    WakeAllConditionVariable(&cond->cv);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *), void *arg) {
    llam_windows_pthread_start_t *start;
    uintptr_t handle;
    unsigned thread_id = 0U;

    (void)attr;
    if (thread == NULL || fn == NULL) {
        return EINVAL;
    }
    start = (llam_windows_pthread_start_t *)malloc(sizeof(*start));
    if (start == NULL) {
        return ENOMEM;
    }
    start->fn = fn;
    start->arg = arg;
    handle = _beginthreadex(NULL, 0U, llam_windows_pthread_entry, start, 0U, &thread_id);
    if (handle == 0U) {
        free(start);
        return errno != 0 ? errno : EAGAIN;
    }
    *thread = handle;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    HANDLE handle = (HANDLE)thread;

    if (retval != NULL) {
        *retval = NULL;
    }
    if (handle == NULL) {
        return 0;
    }
    (void)WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
    return 0;
}

static inline pthread_t pthread_self(void) {
    return (pthread_t)GetCurrentThreadId();
}

static inline int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

static inline int pthread_kill(pthread_t thread, int signo) {
    (void)thread;
    (void)signo;
    return 0;
}

#ifndef pthread_sigmask
static inline int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how;
    (void)set;
    if (oldset != NULL) {
        *oldset = 0;
    }
    return 0;
}
#endif

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif

static inline int llam_windows_fcntl(llam_fd_t fd, int cmd, int flags) {
    if (cmd == F_GETFL) {
        return 0;
    }
    if (cmd == F_SETFL) {
        u_long nonblocking = (flags & O_NONBLOCK) != 0 ? 1UL : 0UL;

        if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
            errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
            return -1;
        }
        return 0;
    }
    errno = EINVAL;
    return -1;
}

#define fcntl(fd, cmd, flags) llam_windows_fcntl((fd), (cmd), (flags))
#define getsockopt(s, level, optname, optval, optlen) getsockopt((s), (level), (optname), (char *)(optval), (optlen))

static inline int llam_windows_socket_count(size_t count) {
    return count > (size_t)INT_MAX ? INT_MAX : (int)count;
}

static inline ssize_t llam_windows_socket_recv(llam_fd_t fd, void *buf, size_t count, int flags) {
    int rc = recv(fd, (char *)buf, llam_windows_socket_count(count), flags);

    if (rc == SOCKET_ERROR) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    return (ssize_t)rc;
}

static inline ssize_t llam_windows_socket_send(llam_fd_t fd, const void *buf, size_t count, int flags) {
    int rc = send(fd, (const char *)buf, llam_windows_socket_count(count), flags);

    if (rc == SOCKET_ERROR) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    return (ssize_t)rc;
}

static inline llam_fd_t llam_windows_socket_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen) {
    SOCKET accepted = accept(fd, addr, addrlen);

    if (accepted == INVALID_SOCKET) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return LLAM_INVALID_FD;
    }
    return (llam_fd_t)accepted;
}

static inline int llam_windows_socket_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (connect(fd, addr, addrlen) == 0) {
        return 0;
    }
    errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
    return -1;
}

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_STACK 0
#define MAP_FAILED ((void *)-1)

static inline DWORD llam_windows_protect_from_posix(int prot) {
    if (prot == PROT_NONE) {
        return PAGE_NOACCESS;
    }
    if ((prot & PROT_WRITE) != 0) {
        return PAGE_READWRITE;
    }
    if ((prot & PROT_READ) != 0) {
        return PAGE_READONLY;
    }
    return PAGE_NOACCESS;
}

static inline void *llam_windows_mmap(void *addr, size_t len, int prot, int flags, int fd, long long offset) {
    DWORD protect = llam_windows_protect_from_posix(prot);
    void *mapping;

    (void)flags;
    (void)offset;
    if (addr != NULL || len == 0U || fd != -1) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    mapping = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, protect);
    if (mapping == NULL) {
        errno = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EINVAL;
        return MAP_FAILED;
    }
    return mapping;
}

static inline int llam_windows_mprotect(void *addr, size_t len, int prot) {
    DWORD old_protect = 0;
    DWORD protect = llam_windows_protect_from_posix(prot);

    if (addr == NULL || len == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (!VirtualProtect(addr, len, protect, &old_protect)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline int llam_windows_munmap(void *addr, size_t len) {
    (void)len;
    if (addr == NULL || addr == MAP_FAILED) {
        errno = EINVAL;
        return -1;
    }
    if (!VirtualFree(addr, 0U, MEM_RELEASE)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

#define mmap(addr, len, prot, flags, fd, offset) llam_windows_mmap((addr), (len), (prot), (flags), (fd), (offset))
#define mprotect(addr, len, prot) llam_windows_mprotect((addr), (len), (prot))
#define munmap(addr, len) llam_windows_munmap((addr), (len))

#ifndef LLAM_WINDOWS_SIGSET_T_DEFINED
typedef int sigset_t;
#endif
typedef struct siginfo {
    void *si_addr;
} siginfo_t;

struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
};

typedef struct stack_t {
    void *ss_sp;
    size_t ss_size;
    int ss_flags;
} stack_t;

#ifndef SA_RESTART
#define SA_RESTART 0
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0
#endif
#ifndef SS_DISABLE
#define SS_DISABLE 1
#endif

static inline int sigemptyset(sigset_t *set) {
    if (set != NULL) {
        *set = 0;
    }
    return 0;
}

static inline int sigaction(int signo, const struct sigaction *act, struct sigaction *oldact) {
    (void)signo;
    (void)act;
    if (oldact != NULL) {
        memset(oldact, 0, sizeof(*oldact));
    }
    return 0;
}

static inline int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    (void)ss;
    if (old_ss != NULL) {
        old_ss->ss_sp = NULL;
        old_ss->ss_size = 0U;
        old_ss->ss_flags = SS_DISABLE;
    }
    return 0;
}

static inline int llam_windows_posix_memalign(void **memptr, size_t alignment, size_t size) {
    void *ptr;

    if (memptr == NULL || alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        return EINVAL;
    }
    ptr = _aligned_malloc(size, alignment);
    if (ptr == NULL) {
        return ENOMEM;
    }
    *memptr = ptr;
    return 0;
}

static inline int llam_windows_dprintf(int fd, const char *fmt, ...) {
    char stack_buf[1024];
    char *buf = stack_buf;
    va_list ap;
    int needed;
    ssize_t written;

    va_start(ap, fmt);
    needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (needed < 0) {
        return -1;
    }
    if ((size_t)needed >= sizeof(stack_buf)) {
        va_start(ap, fmt);
        buf = (char *)malloc((size_t)needed + 1U);
        if (buf == NULL) {
            va_end(ap);
            errno = ENOMEM;
            return -1;
        }
        needed = vsnprintf(buf, (size_t)needed + 1U, fmt, ap);
        va_end(ap);
        if (needed < 0) {
            free(buf);
            return -1;
        }
    }
    written = write(fd, buf, (size_t)needed);
    if (buf != stack_buf) {
        free(buf);
    }
    return written == (ssize_t)needed ? needed : -1;
}

#define posix_memalign(memptr, alignment, size) llam_windows_posix_memalign((memptr), (alignment), (size))
#define dprintf(...) llam_windows_dprintf(__VA_ARGS__)

#endif

#endif
