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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "llam/managed.h"
#include "managed_async_adapter.h"
#include "managed_bench.h"
#include <inttypes.h>
#include <stdio.h>

#if defined(LLAM_ENABLE_TEST_HOOKS)
#error "Performance measurements require production runtime objects with test hooks disabled"
#endif

#if !LLAM_ASYNC_CONTEXT_SHARED || LLAM_PLATFORM_WINDOWS
int main(void) { puts("managed benchmark: shared native profile unavailable"); return 77; }
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

static bool load_entry(void *image, const char *name, llam_task_fn *out) {
    void *address = dlsym(image, name);
    _Static_assert(sizeof(address) == sizeof(*out), "native function pointer ABI");
    memcpy(out, &address, sizeof(*out));
    return address != NULL;
}

static void *io_source(void *argument) {
    llam_managed_bench_work_t *work = argument;
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 1000000L};
    while (!__atomic_load_n(&work->io_started, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&work->io_stop, __ATOMIC_ACQUIRE)) (void)nanosleep(&interval, NULL);
    for (unsigned i = 0U; i < LLAM_MANAGED_BENCH_SAMPLES; ++i) {
        if (__atomic_load_n(&work->io_stop, __ATOMIC_ACQUIRE)) break;
        (void)nanosleep(&interval, NULL);
        unsigned char byte = (unsigned char)i;
        __atomic_store_n(&work->sent_ns[i], llam_now_ns(), __ATOMIC_RELEASE);
        for (;;) {
            if (write(work->io_write_fd, &byte, 1U) == 1) break;
            int error = errno;
            if (error == EINTR) continue;
            if (error != EAGAIN && error != EWOULDBLOCK) {
                work->io_source_error = error != 0 ? error : EIO;
                goto complete;
            }
            /* This unmanaged source owns no M/P. Backpressure is accounted
             * for and retried, while the stop check bounds error cleanup. */
            ++work->io_source_waits;
            struct pollfd descriptor = {.fd = work->io_write_fd, .events = POLLOUT};
            if (poll(&descriptor, 1U, 10) < 0 && errno != EINTR) {
                work->io_source_error = errno;
                goto complete;
            }
            if (__atomic_load_n(&work->io_stop, __ATOMIC_ACQUIRE)) goto complete;
        }
    }
complete:
    (void)shutdown(work->io_write_fd, SHUT_WR);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) return 2;
    uint32_t policy;
    if (strcmp(argv[2], "cooperative") == 0) policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    else if (strcmp(argv[2], "auto") == 0) policy = LLAM_PREEMPT_POLICY_ASYNC_AUTO;
    else if (strcmp(argv[2], "required") == 0) policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    else return 2;
    bool yielding = strcmp(argv[3], "yield") == 0;
    bool timer = strcmp(argv[3], "timer") == 0 || strcmp(argv[3], "timer-hog") == 0;
    bool io = strcmp(argv[3], "io") == 0 || strcmp(argv[3], "io-hog") == 0;
    bool hog = strcmp(argv[3], "timer-hog") == 0 || strcmp(argv[3], "io-hog") == 0;
    if (!yielding && !timer && !io && strcmp(argv[3], "cpu") != 0) return 2;
    char *tail;
    errno = 0;
    uint64_t iterations = strtoull(argv[4], &tail, 10);
    if (errno != 0 || tail == argv[4] || *tail != '\0' || argv[4][0] == '-' ||
        iterations == 0U || iterations > UINT64_C(1000000000)) return 2;
    uint64_t quantum_ns = 0U;
    if (argc == 6) {
        errno = 0;
        quantum_ns = strtoull(argv[5], &tail, 10);
        if (errno != 0 || tail == argv[5] || *tail != '\0' || argv[5][0] == '-' ||
            quantum_ns > UINT64_C(1000000000)) return 2;
    }
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    llam_task_fn cpu_entry, timer_entry, io_entry;
    if (image == NULL || !load_entry(image, "llam_managed_bench_cpu", &cpu_entry) ||
        !load_entry(image, "llam_managed_bench_timer", &timer_entry) ||
        !load_entry(image, "llam_managed_bench_io", &io_entry)) return 1;
    llam_runtime_opts_t options;
    llam_preempt_opts_t preempt;
    llam_runtime_t *runtime = NULL;
    llam_preempt_info_t before, after;
    llam_runtime_opts_init(&options, sizeof(options));
    llam_preempt_opts_init(&preempt, sizeof(preempt));
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    options.preempt_quantum_ns = quantum_ns;
    preempt.policy = policy;
    preempt.native_affine_capacity = 0U;
    if (llam_runtime_create_managed(&options, sizeof(options), &preempt, sizeof(preempt), &runtime) != 0) {
        perror("managed benchmark create"); dlclose(image); return 1;
    }
    dlclose(image); /* The catalog's independent loader pin remains. */
    llam_managed_bench_work_t work[LLAM_MANAGED_BENCH_TASKS] = {0};
    llam_managed_bench_work_t timer_work = {0};
    int io_pair[2] = {-1, -1};
    pthread_t sender;
    bool sender_started = false;
    llam_task_t *tasks[LLAM_MANAGED_BENCH_TASKS + 1U];
    unsigned count = 0U, cpu_count = timer || io ? (hog ? 2U : 0U) : LLAM_MANAGED_BENCH_TASKS;
    int result = 1;
    int failure_errno;
    const char *stage = "spawn";
    if (io) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, io_pair) != 0 ||
            fcntl(io_pair[0], F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(io_pair[1], F_SETFL, O_NONBLOCK) != 0) goto done;
        timer_work.io_read_fd = io_pair[0];
        timer_work.io_write_fd = io_pair[1];
        if (pthread_create(&sender, NULL, io_source, &timer_work) != 0) goto done;
        sender_started = true;
    }
    if (timer || io) {
        if (hog) { timer_work.hogs[0] = &work[0]; timer_work.hogs[1] = &work[1]; }
        tasks[count] = llam_runtime_spawn_ex(runtime, io ? io_entry : timer_entry, &timer_work, NULL, 0U);
        if (tasks[count++] == NULL) goto done;
    }
    for (unsigned i = 0U; i < cpu_count; ++i) {
        work[i].iterations = iterations;
        work[i].seed = 11U + i;
        work[i].yield_every = yielding ? 1024U : 0U;
        tasks[count] = llam_runtime_spawn_ex(runtime, cpu_entry, &work[i], NULL, 0U);
        if (tasks[count++] == NULL) goto done;
    }
    stage = "initial policy info";
    if (llam_runtime_preempt_info_ex(runtime, &before, sizeof(before)) != 0) goto done;
    uint64_t run_start = llam_now_ns();
    stage = "runtime run";
    if (llam_runtime_run_handle(runtime) != 0) goto done;
    uint64_t run_ns = llam_now_ns() - run_start;
    uint64_t first = UINT64_MAX, last = 0U;
    stage = "CPU result";
    for (unsigned i = 0U; i < cpu_count; ++i) {
        uint64_t n = work[i].iterations;
        uint64_t expected = n * (3U * work[i].seed + 7U) + 3U * (n * (n - 1U) / 2U);
        if (work[i].result != expected || work[i].finished != 1U) goto done;
        if (work[i].begin_ns < first) first = work[i].begin_ns;
        if (work[i].end_ns > last) last = work[i].end_ns;
    }
    stage = "probe result";
    if ((timer || io) && (timer_work.error != 0 || timer_work.samples != LLAM_MANAGED_BENCH_SAMPLES)) goto done;
    stage = "I/O source join";
    if (sender_started) {
        if (pthread_join(sender, NULL) != 0) goto done;
        sender_started = false;
        if (timer_work.io_source_error != 0) goto done;
    }
    stage = "task join";
    for (unsigned i = 0U; i < count; ++i) if (llam_join(tasks[i]) != 0) goto done;
    stage = "final policy info";
    if (llam_runtime_preempt_info_ex(runtime, &after, sizeof(after)) != 0 || after.capsule_charge_bytes != 0U)
        goto done;
    llam_async_shared_stats_t stats = {0};
    stage = "native drain";
    bool native = llam_async_shared_stats(runtime, &stats);
    if (policy != LLAM_PREEMPT_POLICY_COOPERATIVE &&
        (!native || stats.captures != stats.restores || stats.restores != stats.witnesses ||
         stats.requests < stats.captures || stats.binding_ops != 0U || stats.execution_debt != 0U ||
         stats.charged_bytes != 0U || stats.retire_depth != 0U || stats.finalizer_depth != 0U ||
         stats.bindings_created != stats.bindings_closed ||
         (!timer && !io && !yielding && stats.captures == 0U))) goto done;
    struct rusage usage;
    stage = "resource usage";
    if (getrusage(RUSAGE_SELF, &usage) != 0) goto done;
    uint64_t rss = (uint64_t)usage.ru_maxrss;
#if !LLAM_PLATFORM_DARWIN
    rss *= 1024U;
#endif
    printf("{\"policy\":\"%s\",\"case\":\"%s\",\"iterations\":%" PRIu64
        ",\"tasks\":%u,\"portable_processors\":%u,\"requested_quantum_ns\":%" PRIu64
        ",\"quantum_ns\":%" PRIu64 ",\"run_ns\":%" PRIu64
        ",\"work_ns\":%" PRIu64 ",\"max_rss_bytes\":%" PRIu64 ",\"captures\":%" PRIu64
        ",\"requests\":%" PRIu64 ",\"capture_request_refusals\":%" PRIu64
        ",\"prepared_capsules\":%" PRIu64 ",\"reserved_capsule_bytes\":%" PRIu64
        ",\"charge_per_task\":%" PRIu64 ",\"overlapping_timer_samples\":%u,\"timer_lag_ns\":[",
        argv[2], argv[3], iterations, count, runtime->portable_processor_count, quantum_ns,
        native ? llam_managed_async_effective_quantum(runtime, LLAM_TASK_CLASS_DEFAULT) : 0U, run_ns,
        cpu_count != 0U ? last - first : timer_work.end_ns - timer_work.begin_ns,
        rss, stats.captures, stats.requests, stats.requests - stats.captures,
        before.prepared_capsules, before.reserved_capsule_bytes,
        before.conservative_task_charge_bytes, timer ? timer_work.overlapping_samples : 0U);
    for (unsigned i = 0U; !io && i < timer_work.samples; ++i)
        printf("%s%" PRIu64, i ? "," : "", timer_work.lag_ns[i]);
    printf("],\"io_source_waits\":%u,\"overlapping_io_samples\":%u,\"io_lag_ns\":[",
        timer_work.io_source_waits, io ? timer_work.overlapping_samples : 0U);
    for (unsigned i = 0U; io && i < timer_work.samples; ++i)
        printf("%s%" PRIu64, i ? "," : "", timer_work.lag_ns[i]);
    printf("],\"request_to_landing_ns\":[");
    llam_async_shared_event_t events[512];
    size_t event_count = native ? llam_async_shared_profile_events(runtime, events, 512U) : 0U;
    bool comma = false;
    for (size_t i = 0U; i < event_count; ++i) {
        const llam_async_shared_event_t *event = &events[i];
        if (event->kind != LLAM_ASYNC_SHARED_TEST_LANDING || event->requested_ns == 0U ||
            event->observed_ns < event->requested_ns) continue;
        printf("%s%" PRIu64, comma ? "," : "", event->observed_ns - event->requested_ns);
        comma = true;
    }
    puts("]}");
    result = 0;
done:
    failure_errno = errno;
    __atomic_store_n(&timer_work.io_stop, 1U, __ATOMIC_RELEASE);
    if (sender_started) (void)pthread_join(sender, NULL);
    if (result != 0) {
        fprintf(stderr, "managed benchmark failed at %s: errno=%d probe_error=%d samples=%u source_error=%d\n",
            stage, failure_errno, timer_work.error, timer_work.samples, timer_work.io_source_error);
        llam_preempt_info_t failure_info;
        if (llam_runtime_preempt_info_ex(runtime, &failure_info, sizeof(failure_info)) == 0)
            fprintf(stderr, "policy=%u effective=%u reason=%u flags=%u\n", failure_info.requested_policy,
                failure_info.effective_policy, failure_info.reason, failure_info.flags);
        llam_async_shared_stats_t failure_stats;
        if (llam_async_shared_stats(runtime, &failure_stats))
            fprintf(stderr, "native captures=%" PRIu64 " restores=%" PRIu64 " witnesses=%" PRIu64
                " debt=%" PRIu64 " refs=%" PRIu64 " charge=%" PRIu64 "\n", failure_stats.captures,
                failure_stats.restores, failure_stats.witnesses, failure_stats.execution_debt,
                failure_stats.binding_ops, failure_stats.charged_bytes);
        for (unsigned i = 0U; i < count; ++i) {
            if (tasks[i] == NULL) continue;
            llam_task_t *task = llam_task_resolve_public_handle(tasks[i]);
            if (task == NULL) continue;
            fprintf(stderr, "task=%" PRIu64 " state=%u saved_errno=%d\n", task->id,
                (unsigned)atomic_load_explicit(&task->state, memory_order_acquire), task->saved_errno);
            llam_task_end_public_op(task);
        }
    }
    llam_runtime_destroy(runtime);
    for (unsigned i = 0U; i < 2U; ++i) if (io_pair[i] >= 0) (void)close(io_pair[i]);
    return result;
}
#endif
