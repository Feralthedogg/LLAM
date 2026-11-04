/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"
#include "io/runtime_io_api_internal.h"

#include <stdio.h>
#if LLAM_PLATFORM_DARWIN
#include <mach/mach_vm.h>
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d)\n", __func__, __LINE__, #expr, errno); \
        return 1; \
    } \
} while (0)

static atomic_bool tests_done;

static void native_pause(void) {
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000L };
    (void)nanosleep(&delay, NULL);
}

static void *watchdog(void *arg) {
    uint64_t deadline = llam_now_ns() + UINT64_C(10000000000);

    (void)arg;
    while (!atomic_load_explicit(&tests_done, memory_order_acquire)) {
        if (llam_now_ns() >= deadline) {
            fputs("test_runtime_regressions: deadlock or unbounded wait\n", stderr);
            _Exit(1);
        }
        native_pause();
    }
    return NULL;
}

static int runtime_opts(llam_runtime_opts_t *opts) {
    int rc = llam_runtime_opts_init(opts, sizeof(*opts));

    opts->deterministic = 1U;
    opts->profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    return rc;
}

#if LLAM_PLATFORM_POSIX
static bool mapping_present(void *mapping) {
#if LLAM_PLATFORM_DARWIN
    mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)mapping;
    mach_vm_size_t size = 0U;
    vm_region_basic_info_data_64_t info = {0};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t rc = mach_vm_region(mach_task_self(), &address, &size,
                                      VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) {
        (void)mach_port_deallocate(mach_task_self(), object);
    }
    return rc == KERN_SUCCESS && address <= (uintptr_t)mapping &&
           (uintptr_t)mapping - address < size;
#else
    unsigned char resident;
    return mincore(mapping, (size_t)llam_page_size(), (void *)&resident) == 0;
#endif
}
#endif

static void count_task(void *arg) {
    atomic_uint *calls = arg;
    atomic_fetch_add_explicit(calls, 1U, memory_order_relaxed);
}

static int shutdown_unlisted_pending(void) {
    llam_runtime_opts_t opts;
    llam_spawn_opts_t spawn_opts;
    llam_cancel_token_t *token = NULL;
    llam_task_t *handle;
    llam_task_t *raw;
    atomic_uint calls = 0U;
#if LLAM_PLATFORM_POSIX
    void *mapping;
#endif

    CHECK(runtime_opts(&opts) == 0);
    CHECK(llam_runtime_init_compat_ex(&opts, sizeof(opts)) == 0);
    g_llam_runtime.task_list_eager = 0U;
    token = llam_cancel_token_create();
    CHECK(token != NULL);
    CHECK(llam_spawn_opts_init(&spawn_opts, sizeof(spawn_opts)) == 0);
    spawn_opts.cancel_token = token;
    handle = llam_spawn_ex(count_task, &calls, &spawn_opts, sizeof(spawn_opts));
    CHECK(handle != NULL);
    raw = llam_task_resolve_public_handle(handle);
    CHECK(raw != NULL && raw->stack_mapping != NULL);
    CHECK(atomic_load_explicit(&raw->task_listed, memory_order_acquire) == 0U);
#if LLAM_PLATFORM_POSIX
    mapping = raw->stack_mapping;
    CHECK(mapping_present(mapping));
#endif
    llam_task_end_public_op(raw);
    llam_runtime_shutdown();
    CHECK(atomic_load_explicit(&calls, memory_order_relaxed) == 0U);
#if LLAM_PLATFORM_POSIX
    CHECK(!mapping_present(mapping));
#endif
    CHECK(llam_cancel_token_destroy(token) == 0);
    return 0;
}

typedef struct child_state {
    llam_cancel_token_t *token;
    atomic_uint calls;
    int error;
} child_state_t;

static void spawn_unjoined_children(void *arg) {
    child_state_t *state = arg;
    llam_spawn_opts_t opts;

    if (llam_spawn_opts_init(&opts, sizeof(opts)) != 0) {
        state->error = errno;
        return;
    }
    opts.cancel_token = state->token;
    for (unsigned i = 0U; i < 4U; ++i) {
        if (llam_spawn_ex(count_task, &state->calls, &opts, sizeof(opts)) == NULL) {
            state->error = errno;
            return;
        }
    }
}

static int shutdown_unlisted_completed(void) {
    llam_runtime_opts_t opts;
    child_state_t state = {0};

    CHECK(runtime_opts(&opts) == 0);
    CHECK(llam_runtime_init_compat_ex(&opts, sizeof(opts)) == 0);
    g_llam_runtime.task_list_eager = 0U;
    state.token = llam_cancel_token_create();
    CHECK(state.token != NULL);
    CHECK(llam_spawn(spawn_unjoined_children, &state, NULL) != NULL);
    CHECK(llam_run() == 0);
    CHECK(state.error == 0 && atomic_load_explicit(&state.calls, memory_order_relaxed) == 4U);
    errno = 0;
    CHECK(llam_cancel_token_destroy(state.token) == -1 && errno == EBUSY);
    llam_runtime_shutdown();
    CHECK(llam_cancel_token_destroy(state.token) == 0);
    return 0;
}

typedef struct timer_state {
    llam_timer_t *timer;
    int error;
} timer_state_t;

static void create_timer(void *arg) {
    timer_state_t *state = arg;
    llam_timer_opts_t opts = {0};

    opts.interval_ns = UINT64_C(1000000);
    opts.first_deadline_ns = llam_now_ns() + UINT64_C(1000000000);
    if (llam_timer_create_ex(&opts, sizeof(opts), &state->timer) != 0) {
        state->error = errno;
    }
}

static int explicit_timer_host_wait(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *rt = NULL;
    llam_task_t *task;
    timer_state_t state = {0};
    uint64_t ticks;

    CHECK(runtime_opts(&opts) == 0);
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &rt) == 0);
    task = llam_runtime_spawn_ex(rt, create_timer, &state, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(rt) == 0 && llam_join(task) == 0);
    CHECK(state.error == 0 && state.timer != NULL);
    CHECK(llam_timer_reset(state.timer, llam_now_ns() + UINT64_C(20000000), UINT64_C(1000000)) == 0);
    CHECK(llam_timer_wait_until(state.timer, llam_now_ns() + UINT64_C(1000000000), &ticks) == 0);
    CHECK(ticks > 0U);
    CHECK(llam_timer_reset(state.timer, llam_now_ns() + UINT64_C(1000000000), UINT64_C(1000000)) == 0);
    errno = 0;
    CHECK(llam_timer_wait_until(state.timer, llam_now_ns() + UINT64_C(20000000), &ticks) == -1 &&
          errno == ETIMEDOUT && ticks == 0U);
    CHECK(llam_timer_destroy(state.timer) == 0);
    llam_runtime_destroy(rt);
    return 0;
}

typedef struct create_during_destroy_state {
    llam_runtime_t *rt;
    llam_runtime_t *created;
    atomic_bool started;
    int error;
    int run_result;
} create_during_destroy_state_t;

static void create_during_destroy_task(void *arg) {
    create_during_destroy_state_t *state = arg;
    llam_runtime_opts_t opts;

    atomic_store_explicit(&state->started, true, memory_order_release);
    /* The run driver's public pin keeps rt alive until this task returns. */
    while (!atomic_load_explicit(&state->rt->destroy_claimed, memory_order_acquire)) {
        llam_pause_cpu();
    }
    if (runtime_opts(&opts) != 0 ||
        llam_runtime_create_compat(&opts, sizeof(opts), &state->created) != 0) {
        state->error = errno;
    }
}

static void *run_during_destroy(void *arg) {
    create_during_destroy_state_t *state = arg;
    state->run_result = llam_runtime_run_handle(state->rt);
    return NULL;
}

static int create_during_destroy(void) {
    llam_runtime_opts_t opts;
    create_during_destroy_state_t state = {0};
    pthread_t runner;

    CHECK(runtime_opts(&opts) == 0);
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &state.rt) == 0);
    CHECK(llam_runtime_spawn_ex(state.rt, create_during_destroy_task, &state, NULL, 0U) != NULL);
    CHECK(pthread_create(&runner, NULL, run_during_destroy, &state) == 0);
    while (!atomic_load_explicit(&state.started, memory_order_acquire)) {
        native_pause();
    }
    llam_runtime_destroy(state.rt);
    CHECK(pthread_join(runner, NULL) == 0);
    CHECK(state.error == 0 && state.created != NULL && state.run_result == 0);
    llam_runtime_destroy(state.created);
    return 0;
}

#if LLAM_PLATFORM_POSIX
static void *late_write(void *arg) {
    int fd = *(int *)arg;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 150000000L };
    (void)nanosleep(&delay, NULL);
    ssize_t written = write(fd, "x", 1U);
    if (written != 1) {
        _Exit(1);
    }
    return NULL;
}

static int blocking_read_deadline(void) {
    int pair[2];
    pthread_t writer;
    llam_io_req_t req = {0};
    char byte;
    int saved_errno;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    req.fd = pair[0];
    req.buf = &byte;
    req.count = 1U;
    req.deadline_ns = llam_now_ns() + UINT64_C(20000000);
    CHECK(pthread_create(&writer, NULL, late_write, &pair[1]) == 0);
    (void)llam_blocking_read_impl(&req);
    saved_errno = errno;
    CHECK(pthread_join(writer, NULL) == 0);
    CHECK(req.result == -1 && saved_errno == ETIMEDOUT);
    CHECK(read(pair[0], &byte, 1U) == 1 && byte == 'x');
    CHECK((fcntl(pair[0], F_GETFL, 0) & O_NONBLOCK) == 0);
    close(pair[0]);
    close(pair[1]);
    return 0;
}
#endif

#if LLAM_PLATFORM_LINUX
static int signal_mask_preserved(void) {
    sigset_t original, mask, after;
    int signals[] = {SIGUSR1, SIGUSR2};
    llam_signal_set_t *set = NULL;

    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &original) == 0);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &mask, NULL) == 0);
    CHECK(llam_signal_set_create_ex(signals, 2U, NULL, 0U, &set) == 0);
    CHECK(llam_signal_set_destroy(set) == 0);
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &after) == 0);
    CHECK(sigismember(&after, SIGUSR1) == 1 && sigismember(&after, SIGUSR2) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    return 0;
}

typedef struct signal_cancel_state {
    llam_signal_set_t *set;
    llam_cancel_token_t *token;
    uint64_t elapsed;
    int error;
} signal_cancel_state_t;

typedef struct signal_mask_thread_state { llam_signal_set_t *set; int error; } signal_mask_thread_state_t;

static void *create_signal_on_thread(void *arg) {
    signal_mask_thread_state_t *state = arg;
    sigset_t mask;
    int signo = SIGUSR1;

    sigemptyset(&mask);
    sigaddset(&mask, signo);
    if (pthread_sigmask(SIG_UNBLOCK, &mask, NULL) != 0 ||
        llam_signal_set_create_ex(&signo, 1U, NULL, 0U, &state->set) != 0) {
        state->error = 1;
    }
    return NULL;
}

static void *destroy_signal_on_thread(void *arg) {
    signal_mask_thread_state_t *state = arg;
    sigset_t mask;

    if (llam_signal_set_destroy(state->set) != 0 ||
        pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0 || sigismember(&mask, SIGUSR1) != 1) {
        state->error = 1;
    }
    return NULL;
}

static int signal_cross_thread_mask_preserved(void) {
    signal_mask_thread_state_t state = {0};
    sigset_t original, mask;
    pthread_t creator, destroyer;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, &original) == 0);
    CHECK(pthread_create(&creator, NULL, create_signal_on_thread, &state) == 0);
    CHECK(pthread_join(creator, NULL) == 0 && state.error == 0);
    /* pthread implementations may recycle the exited creator's native identity. */
    CHECK(pthread_create(&destroyer, NULL, destroy_signal_on_thread, &state) == 0);
    CHECK(pthread_join(destroyer, NULL) == 0 && state.error == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    return 0;
}

static void signal_wait_task(void *arg) {
    signal_cancel_state_t *state = arg;
    llam_signal_event_t event;
    uint64_t start = llam_now_ns();
    int rc = llam_signal_wait_until(state->set, start + UINT64_C(400000000), &event);
    state->error = rc == -1 ? errno : 0;
    state->elapsed = llam_now_ns() - start;
}

static void signal_cancel_task(void *arg) {
    signal_cancel_state_t *state = arg;
    (void)llam_sleep_ns(UINT64_C(20000000));
    (void)llam_cancel_token_cancel(state->token);
}

static int signal_wait_cancellation(void) {
    llam_runtime_opts_t opts;
    llam_spawn_opts_t spawn_opts;
    signal_cancel_state_t state = {0};
    int signo = SIGUSR2; /* SIGUSR1 belongs to runtime preemption. */
    llam_task_t *waiter, *canceler;

    CHECK(llam_signal_set_create_ex(&signo, 1U, NULL, 0U, &state.set) == 0);
    CHECK(runtime_opts(&opts) == 0 && llam_runtime_init_compat_ex(&opts, sizeof(opts)) == 0);
    state.token = llam_cancel_token_create();
    CHECK(state.token != NULL);
    CHECK(llam_spawn_opts_init(&spawn_opts, sizeof(spawn_opts)) == 0);
    spawn_opts.cancel_token = state.token;
    waiter = llam_spawn_ex(signal_wait_task, &state, &spawn_opts, sizeof(spawn_opts));
    canceler = llam_spawn(signal_cancel_task, &state, NULL);
    CHECK(waiter != NULL && canceler != NULL && llam_run() == 0);
    CHECK(llam_join(waiter) == 0 && llam_join(canceler) == 0);
    CHECK(state.error == ECANCELED && state.elapsed < UINT64_C(200000000));
    CHECK(llam_cancel_token_destroy(state.token) == 0);
    llam_runtime_shutdown();
    CHECK(llam_signal_set_destroy(state.set) == 0);
    return 0;
}
#endif

#if LLAM_PLATFORM_WINDOWS
static int windows_clock_conversion(void) {
    static const struct {
        uint64_t ticks, frequency, seconds;
        long nanoseconds;
    } cases[] = {
        {0U, 10000000U, 0U, 0L},
        {18446744073ULL, 10000000U, 1844U, 674407300L},
        {18446744074ULL, 10000000U, 1844U, 674407400L},
        {26784000000000ULL, 10000000U, 2678400U, 0L},
        {9223372036854775807ULL, 10000000U, 922337203685ULL, 477580700L},
        {9223372036854775806ULL, 9223372036854775807ULL, 0U, 999999999L},
        {UINT64_MAX, UINT64_MAX, 1U, 0L},
    };
    LARGE_INTEGER frequency, before, after;
    struct timespec actual, lower, upper;

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        llam_windows_counter_to_timespec(cases[i].ticks, cases[i].frequency, &actual);
        CHECK((uint64_t)actual.tv_sec == cases[i].seconds && actual.tv_nsec == cases[i].nanoseconds);
    }
    CHECK(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&before));
    CHECK(clock_gettime(CLOCK_MONOTONIC, &actual) == 0);
    CHECK(QueryPerformanceCounter(&after));
    llam_windows_counter_to_timespec((uint64_t)before.QuadPart, (uint64_t)frequency.QuadPart, &lower);
    llam_windows_counter_to_timespec((uint64_t)after.QuadPart, (uint64_t)frequency.QuadPart, &upper);
    CHECK(actual.tv_sec > lower.tv_sec ||
          (actual.tv_sec == lower.tv_sec && actual.tv_nsec >= lower.tv_nsec));
    CHECK(actual.tv_sec < upper.tv_sec ||
          (actual.tv_sec == upper.tv_sec && actual.tv_nsec <= upper.tv_nsec));
    return 0;
}

static int file_position_is(HANDLE handle, LONGLONG expected) {
    LARGE_INTEGER zero, position;
    zero.QuadPart = 0;
    CHECK(SetFilePointerEx(handle, zero, &position, FILE_CURRENT));
    CHECK(position.QuadPart == expected);
    return 0;
}

static int check_positional_file(HANDLE handle) {
    char bytes[4];
    llam_mut_iovec_t vectors[2] = {{bytes, 2U}, {bytes + 2, 2U}};
    llam_iovec_t writes[2] = {{"ab", 2U}, {"cd", 2U}};

    CHECK(llam_pread_handle((llam_handle_t)handle, bytes, 2U, 0U) == 2 && memcmp(bytes, "ab", 2U) == 0);
    CHECK(file_position_is(handle, 3) == 0);
    CHECK(llam_pwrite_handle((llam_handle_t)handle, "XY", 2U, 8U) == 2);
    CHECK(file_position_is(handle, 3) == 0);
    CHECK(llam_preadv_handle((llam_handle_t)handle, vectors, 2, 0U) == 4 && memcmp(bytes, "abcd", 4U) == 0);
    CHECK(llam_pwritev_handle((llam_handle_t)handle, writes, 2, 0U) == 4);
    CHECK(file_position_is(handle, 3) == 0);
    for (unsigned i = 0U; i < 16U; ++i) {
        CHECK(llam_pread_handle((llam_handle_t)handle, bytes, sizeof(bytes), 10U) == 0);
        CHECK(llam_pread_handle((llam_handle_t)handle, bytes, sizeof(bytes), UINT64_C(0x100000010)) == 0);
    }
    CHECK(file_position_is(handle, 3) == 0);
    return 0;
}

typedef struct file_state { HANDLE handle; int result; } file_state_t;

static void positional_file_task(void *arg) {
    file_state_t *state = arg;
    state->result = check_positional_file(state->handle);
}

static int windows_file_io(void) {
    char directory[MAX_PATH], path[MAX_PATH], byte;
    llam_runtime_opts_t opts;
    LARGE_INTEGER position;
    HANDLE handle;
    DWORD transferred;

    CHECK(GetTempPathA(MAX_PATH, directory) != 0U);
    CHECK(GetTempFileNameA(directory, "llm", 0U, path) != 0U);
    handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(handle, "abcdefghij", 10U, &transferred, NULL) && transferred == 10U);
    CHECK(CloseHandle(handle));
    for (unsigned overlapped = 0U; overlapped < 2U; ++overlapped) {
        file_state_t state;
        llam_task_t *task;

        handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0U), NULL);
        CHECK(handle != INVALID_HANDLE_VALUE);
        position.QuadPart = 3;
        CHECK(SetFilePointerEx(handle, position, NULL, FILE_BEGIN));
        CHECK(check_positional_file(handle) == 0);
        if (!overlapped) {
            CHECK(ReadFile(handle, &byte, 1U, &transferred, NULL) && transferred == 1U && byte == 'd');
            CHECK(SetFilePointerEx(handle, position, NULL, FILE_BEGIN));
        }
        CHECK(runtime_opts(&opts) == 0 && llam_runtime_init_compat_ex(&opts, sizeof(opts)) == 0);
        state.handle = handle;
        state.result = -1;
        task = llam_spawn(positional_file_task, &state, NULL);
        CHECK(task != NULL && llam_run() == 0 && llam_join(task) == 0 && state.result == 0);
        /* A direct positional call on an already associated handle must suppress stack IOCP packets. */
        CHECK(check_positional_file(handle) == 0);
        CHECK(llam_close_handle((llam_handle_t)handle) == 0);
        llam_runtime_shutdown();
    }
    handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    CHECK(llam_pwrite_handle((llam_handle_t)handle, "!", 1U, 0U) == -1 && errno == EBADF);
    CHECK(file_position_is(handle, 0) == 0);
    CHECK(CloseHandle(handle) && DeleteFileA(path));
    return 0;
}
#endif

int main(int argc, char **argv) {
    static const struct { const char *name; int (*run)(void); } tests[] = {
        {"shutdown_unlisted_pending", shutdown_unlisted_pending},
        {"shutdown_unlisted_completed", shutdown_unlisted_completed},
        {"explicit_timer_host_wait", explicit_timer_host_wait},
        {"create_during_destroy", create_during_destroy},
#if LLAM_PLATFORM_WINDOWS
        {"windows_clock_conversion", windows_clock_conversion},
        {"windows_file_io", windows_file_io},
#endif
#if LLAM_PLATFORM_POSIX
        {"blocking_read_deadline", blocking_read_deadline},
#endif
#if LLAM_PLATFORM_LINUX
        {"signal_mask_preserved", signal_mask_preserved},
        {"signal_cross_thread_mask_preserved", signal_cross_thread_mask_preserved},
        {"signal_wait_cancellation", signal_wait_cancellation},
#endif
    };
    pthread_t guard;
    unsigned ran = 0U;

    setvbuf(stdout, NULL, _IONBF, 0);
    atomic_init(&tests_done, false);
    CHECK(pthread_create(&guard, NULL, watchdog, NULL) == 0);
    for (size_t i = 0U; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) {
            continue;
        }
        printf("test_runtime_regressions: %s\n", tests[i].name);
        CHECK(tests[i].run() == 0);
        ++ran;
    }
    CHECK(ran != 0U);
    atomic_store_explicit(&tests_done, true, memory_order_release);
    CHECK(pthread_join(guard, NULL) == 0);
    return 0;
}
