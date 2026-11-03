/**
 * @file examples/cpu_spike.c
 * @brief Measure timer, I/O, and control dispatch latency under unpolled CPU load.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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

#include "llam/runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CPU_SPIKE_DEFAULT_ROUNDS 7U
#define CPU_SPIKE_MAX_ROUNDS 100U
#define CPU_SPIKE_DEFAULT_MS 200U
#define CPU_SPIKE_MAX_MS 10000U
#define CPU_SPIKE_DEFAULT_SETTLE_MS 100U
#define CPU_SPIKE_MAX_SETTLE_MS 5000U
#define CPU_SPIKE_READY_TIMEOUT_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
#define CPU_SPIKE_HOG_CHUNK 65536U

typedef struct cpu_spike_state {
    llam_runtime_t *runtime;
    atomic_uint timer_ready;
    atomic_uint timer_armed;
    atomic_uint io_armed;
    atomic_uint timer_done;
    atomic_uint io_done;
    atomic_uint control_done;
    atomic_uint hogs_started;
    atomic_uint failures;
    atomic_uint_fast64_t trigger_ns;
    atomic_uint_fast64_t spike_end_ns;
    atomic_uint_fast64_t io_written_ns;
    atomic_uint_fast64_t control_trigger_ns;
    atomic_uint_fast64_t timer_latency_ns;
    atomic_uint_fast64_t io_latency_ns;
    atomic_uint_fast64_t control_latency_ns;
    atomic_uint_fast64_t hog_sink;
    int read_fd;
    int runner_rc;
    int runner_errno;
} cpu_spike_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[cpu-spike] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return -1;
}

static unsigned env_u32(const char *name, unsigned default_value, unsigned max_value) {
    const char *raw = getenv(name);
    const char *cursor;
    char *end = NULL;
    unsigned long value;

    if (raw == NULL || raw[0] == '\0') {
        return default_value;
    }
    for (cursor = raw; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            fprintf(stderr, "[cpu-spike] invalid %s=%s (expected 1..%u)\n", name, raw, max_value);
            return 0U;
        }
    }
    errno = 0;
    value = strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value == 0UL || value > max_value) {
        fprintf(stderr, "[cpu-spike] invalid %s=%s (expected 1..%u)\n", name, raw, max_value);
        return 0U;
    }
    return (unsigned)value;
}

static uint64_t saturating_add_ns(uint64_t base, uint64_t delta) {
    return delta > UINT64_MAX - base ? UINT64_MAX : base + delta;
}

static uint64_t overdue_ns(uint64_t now_ns, uint64_t expected_ns) {
    return now_ns > expected_ns ? now_ns - expected_ns : 0U;
}

static void sleep_until_external(uint64_t deadline_ns) {
    for (;;) {
        uint64_t now_ns = llam_now_ns();
        uint64_t remaining_ns;
        struct timespec delay;

        if (now_ns >= deadline_ns) {
            return;
        }
        remaining_ns = deadline_ns - now_ns;
        if (remaining_ns > 10000000ULL) {
            remaining_ns = 10000000ULL;
        }
        delay.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
        delay.tv_nsec = (long)(remaining_ns % 1000000000ULL);
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
}

static int wait_for_flag(atomic_uint *flag, const char *name) {
    uint64_t deadline_ns = saturating_add_ns(llam_now_ns(), CPU_SPIKE_READY_TIMEOUT_NS);

    while (atomic_load_explicit(flag, memory_order_acquire) == 0U) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};

        if (llam_now_ns() >= deadline_ns) {
            fprintf(stderr, "[cpu-spike] timed out waiting for %s\n", name);
            errno = ETIMEDOUT;
            return -1;
        }
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}

static int wait_for_count_until(atomic_uint *count,
                                unsigned expected,
                                uint64_t deadline_ns,
                                const char *name) {
    while (atomic_load_explicit(count, memory_order_acquire) < expected) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};

        if (llam_now_ns() >= deadline_ns) {
            fprintf(stderr,
                    "[cpu-spike] only %u/%u %s started before trigger; increase LLAM_CPU_SPIKE_SETTLE_MS\n",
                    atomic_load_explicit(count, memory_order_relaxed),
                    expected,
                    name);
            errno = ETIMEDOUT;
            return -1;
        }
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void record_task_failure(cpu_spike_state_t *state) {
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static void timer_probe_task(void *arg) {
    cpu_spike_state_t *state = arg;
    uint64_t trigger_ns;

    atomic_store_explicit(&state->timer_ready, 1U, memory_order_release);
    do {
        trigger_ns = atomic_load_explicit(&state->trigger_ns, memory_order_acquire);
        if (trigger_ns == 0U) {
            llam_yield();
        }
    } while (trigger_ns == 0U);

    atomic_store_explicit(&state->timer_armed, 1U, memory_order_release);
    if (llam_sleep_until(trigger_ns) != 0) {
        record_task_failure(state);
        return;
    }
    atomic_store_explicit(&state->timer_latency_ns,
                          overdue_ns(llam_now_ns(), trigger_ns),
                          memory_order_release);
    atomic_store_explicit(&state->timer_done, 1U, memory_order_release);
}

static void io_probe_task(void *arg) {
    cpu_spike_state_t *state = arg;
    char byte = 0;
    ssize_t received;
    uint64_t written_ns;

    atomic_store_explicit(&state->io_armed, 1U, memory_order_release);
    received = llam_read((llam_fd_t)state->read_fd, &byte, 1U);
    if (received != 1 || byte != 's') {
        record_task_failure(state);
        return;
    }
    written_ns = atomic_load_explicit(&state->io_written_ns, memory_order_acquire);
    atomic_store_explicit(&state->io_latency_ns,
                          overdue_ns(llam_now_ns(), written_ns),
                          memory_order_release);
    atomic_store_explicit(&state->io_done, 1U, memory_order_release);
}

static void control_probe_task(void *arg) {
    cpu_spike_state_t *state = arg;
    uint64_t trigger_ns = atomic_load_explicit(&state->control_trigger_ns, memory_order_acquire);

    atomic_store_explicit(&state->control_latency_ns,
                          overdue_ns(llam_now_ns(), trigger_ns),
                          memory_order_release);
    atomic_store_explicit(&state->control_done, 1U, memory_order_release);
}

static void cpu_hog_task(void *arg) {
    cpu_spike_state_t *state = arg;
    uint64_t deadline_ns = atomic_load_explicit(&state->spike_end_ns, memory_order_acquire);
    uint64_t value = (uint64_t)(uintptr_t)pthread_self() | 1U;

    /* Deliberately no LLAM_PREEMPT_POLL, safepoint, yield, wait, or I/O. */
    atomic_fetch_add_explicit(&state->hogs_started, 1U, memory_order_release);
    while (llam_now_ns() < deadline_ns) {
        for (unsigned i = 0U; i < CPU_SPIKE_HOG_CHUNK; ++i) {
            value = value * UINT64_C(2862933555777941757) + UINT64_C(3037000493);
        }
    }
    atomic_fetch_xor_explicit(&state->hog_sink, value, memory_order_relaxed);
}

static void *runtime_runner(void *arg) {
    cpu_spike_state_t *state = arg;

    errno = 0;
    state->runner_rc = llam_runtime_run_handle(state->runtime);
    state->runner_errno = errno;
    return NULL;
}

static int spawn_detached(cpu_spike_state_t *state,
                          llam_task_fn fn,
                          const llam_spawn_opts_t *opts) {
    llam_task_t *task = llam_runtime_spawn_ex(state->runtime,
                                              fn,
                                              state,
                                              opts,
                                              opts != NULL ? LLAM_SPAWN_OPTS_CURRENT_SIZE : 0U);

    if (task == NULL) {
        return -1;
    }
    return llam_detach(task);
}

static void init_state(cpu_spike_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->read_fd = -1;
    atomic_init(&state->timer_ready, 0U);
    atomic_init(&state->timer_armed, 0U);
    atomic_init(&state->io_armed, 0U);
    atomic_init(&state->timer_done, 0U);
    atomic_init(&state->io_done, 0U);
    atomic_init(&state->control_done, 0U);
    atomic_init(&state->hogs_started, 0U);
    atomic_init(&state->failures, 0U);
    atomic_init(&state->trigger_ns, 0U);
    atomic_init(&state->spike_end_ns, 0U);
    atomic_init(&state->io_written_ns, 0U);
    atomic_init(&state->control_trigger_ns, 0U);
    atomic_init(&state->timer_latency_ns, 0U);
    atomic_init(&state->io_latency_ns, 0U);
    atomic_init(&state->control_latency_ns, 0U);
    atomic_init(&state->hog_sink, 0U);
}

static int run_round(unsigned spike_ms,
                     unsigned settle_ms,
                     unsigned *workers_out,
                     uint64_t *timer_latency_out,
                     uint64_t *io_latency_out,
                     uint64_t *control_latency_out,
                     uint64_t *preempt_requests_out,
                     uint64_t *preempt_yields_out) {
    cpu_spike_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_stats_t stats;
    llam_spawn_opts_t hog_opts;
    llam_spawn_opts_t control_opts;
    pthread_t runner;
    int fds[2] = {-1, -1};
    uint64_t trigger_ns;
    const char byte = 's';
    bool runner_started = false;
    int rc = -1;

    init_state(&state);
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&hog_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&control_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("option initialization failed");
    }
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    runtime_opts.preempt_mode = LLAM_PREEMPT_AUTO;
    runtime_opts.preempt_quantum_ns = 1000000ULL;
    runtime_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    hog_opts.task_class = LLAM_TASK_CLASS_BATCH;
    hog_opts.flags = LLAM_SPAWN_F_PINNED;
    control_opts.task_class = LLAM_TASK_CLASS_LATENCY;
    control_opts.flags = LLAM_SPAWN_F_LATENCY_CRITICAL;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0 ||
        set_nonblocking(fds[0]) != 0 ||
        set_nonblocking(fds[1]) != 0) {
        (void)fail_errno("socketpair setup failed");
        goto cleanup;
    }
    state.read_fd = fds[0];
    if (llam_runtime_create(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
        (void)fail_errno("runtime create failed");
        goto cleanup;
    }
    if (llam_runtime_collect_stats_ex_handle(state.runtime, &stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0 ||
        stats.active_workers == 0U) {
        (void)fail_errno("initial runtime stats failed");
        goto cleanup;
    }
    *workers_out = stats.active_workers;
    if (spawn_detached(&state, timer_probe_task, NULL) != 0 ||
        spawn_detached(&state, io_probe_task, NULL) != 0) {
        (void)fail_errno("probe spawn failed");
        goto cleanup;
    }
    {
        int thread_rc = pthread_create(&runner, NULL, runtime_runner, &state);

        if (thread_rc != 0) {
            errno = thread_rc;
            (void)fail_errno("runtime runner thread create failed");
            goto cleanup;
        }
    }
    runner_started = true;
    if (wait_for_flag(&state.timer_ready, "timer task") != 0 ||
        wait_for_flag(&state.io_armed, "I/O task") != 0) {
        goto cleanup;
    }

    trigger_ns = saturating_add_ns(llam_now_ns(), (uint64_t)settle_ms * 1000000ULL);
    atomic_store_explicit(&state.spike_end_ns,
                          saturating_add_ns(trigger_ns, (uint64_t)spike_ms * 1000000ULL),
                          memory_order_release);
    atomic_store_explicit(&state.trigger_ns, trigger_ns, memory_order_release);
    if (wait_for_flag(&state.timer_armed, "armed timer") != 0) {
        goto cleanup;
    }
    {
        struct timespec park_delay = {.tv_sec = 0, .tv_nsec = 5000000L};
        (void)nanosleep(&park_delay, NULL);
    }
    for (unsigned i = 0U; i < stats.active_workers; ++i) {
        if (spawn_detached(&state, cpu_hog_task, &hog_opts) != 0) {
            (void)fail_errno("CPU hog spawn failed");
            goto cleanup;
        }
    }
    if (wait_for_count_until(&state.hogs_started,
                             stats.active_workers,
                             trigger_ns,
                             "CPU hogs") != 0) {
        goto cleanup;
    }

    sleep_until_external(trigger_ns);
    atomic_store_explicit(&state.io_written_ns, llam_now_ns(), memory_order_release);
    if (write(fds[1], &byte, 1U) != 1) {
        (void)fail_errno("I/O trigger write failed");
        goto cleanup;
    }
    atomic_store_explicit(&state.control_trigger_ns, trigger_ns, memory_order_release);
    if (spawn_detached(&state, control_probe_task, &control_opts) != 0) {
        (void)fail_errno("control probe injection failed");
        goto cleanup;
    }

    if (pthread_join(runner, NULL) != 0) {
        errno = EIO;
        (void)fail_errno("runtime runner join failed");
        runner_started = false;
        goto cleanup;
    }
    runner_started = false;
    if (state.runner_rc != 0) {
        errno = state.runner_errno;
        (void)fail_errno("runtime run failed");
        goto cleanup;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.timer_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.io_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.control_done, memory_order_acquire) == 0U) {
        fprintf(stderr, "[cpu-spike] one or more latency probes did not complete\n");
        goto cleanup;
    }
    if (llam_runtime_collect_stats_ex_handle(state.runtime, &stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        (void)fail_errno("final runtime stats failed");
        goto cleanup;
    }
    *timer_latency_out = atomic_load_explicit(&state.timer_latency_ns, memory_order_acquire);
    *io_latency_out = atomic_load_explicit(&state.io_latency_ns, memory_order_acquire);
    *control_latency_out = atomic_load_explicit(&state.control_latency_ns, memory_order_acquire);
    *preempt_requests_out += stats.preempt_requests;
    *preempt_yields_out += stats.preempt_yields;
    rc = 0;

cleanup:
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    if (runner_started) {
        (void)pthread_join(runner, NULL);
    }
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    return rc;
}

static int compare_u64(const void *lhs, const void *rhs) {
    uint64_t a = *(const uint64_t *)lhs;
    uint64_t b = *(const uint64_t *)rhs;

    return (a > b) - (a < b);
}

static void print_report(const char *metric,
                         uint64_t *samples,
                         unsigned rounds,
                         unsigned workers,
                         unsigned spike_ms,
                         unsigned settle_ms) {
    size_t p50_index = (((size_t)rounds * 50U + 99U) / 100U) - 1U;
    size_t p99_index = (((size_t)rounds * 99U + 99U) / 100U) - 1U;

    qsort(samples, rounds, sizeof(*samples), compare_u64);
    printf("[cpu-spike] runtime=LLAM metric=%s workers=%u rounds=%u spike_ms=%u settle_ms=%u "
           "p50_us=%.2f p99_us=%.2f max_us=%.2f\n",
           metric,
           workers,
           rounds,
           spike_ms,
           settle_ms,
           (double)samples[p50_index] / 1000.0,
           (double)samples[p99_index] / 1000.0,
           (double)samples[rounds - 1U] / 1000.0);
}

int main(void) {
    unsigned rounds = env_u32("LLAM_CPU_SPIKE_ROUNDS", CPU_SPIKE_DEFAULT_ROUNDS, CPU_SPIKE_MAX_ROUNDS);
    unsigned spike_ms = env_u32("LLAM_CPU_SPIKE_MS", CPU_SPIKE_DEFAULT_MS, CPU_SPIKE_MAX_MS);
    unsigned settle_ms = env_u32("LLAM_CPU_SPIKE_SETTLE_MS", CPU_SPIKE_DEFAULT_SETTLE_MS, CPU_SPIKE_MAX_SETTLE_MS);
    uint64_t *timer_samples;
    uint64_t *io_samples;
    uint64_t *control_samples;
    uint64_t preempt_requests = 0U;
    uint64_t preempt_yields = 0U;
    unsigned workers = 0U;

    if (rounds == 0U || spike_ms == 0U || settle_ms == 0U) {
        return 2;
    }
    timer_samples = calloc(rounds, sizeof(*timer_samples));
    io_samples = calloc(rounds, sizeof(*io_samples));
    control_samples = calloc(rounds, sizeof(*control_samples));
    if (timer_samples == NULL || io_samples == NULL || control_samples == NULL) {
        free(timer_samples);
        free(io_samples);
        free(control_samples);
        return fail_errno("sample allocation failed") != 0;
    }

    for (unsigned round = 0U; round < rounds; ++round) {
        unsigned round_workers = 0U;

        if (run_round(spike_ms,
                      settle_ms,
                      &round_workers,
                      &timer_samples[round],
                      &io_samples[round],
                      &control_samples[round],
                      &preempt_requests,
                      &preempt_yields) != 0) {
            free(timer_samples);
            free(io_samples);
            free(control_samples);
            return 1;
        }
        if (workers == 0U) {
            workers = round_workers;
        } else if (workers != round_workers) {
            fprintf(stderr, "[cpu-spike] runtime worker count changed between rounds\n");
            free(timer_samples);
            free(io_samples);
            free(control_samples);
            return 1;
        }
    }

    fprintf(stderr,
            "[cpu-spike-config] runtime=LLAM workers=%u rounds=%u spike_ms=%u settle_ms=%u "
            "preempt_requests=%llu preempt_yields=%llu polls=0\n",
            workers,
            rounds,
            spike_ms,
            settle_ms,
            (unsigned long long)preempt_requests,
            (unsigned long long)preempt_yields);
    print_report("timer", timer_samples, rounds, workers, spike_ms, settle_ms);
    print_report("io", io_samples, rounds, workers, spike_ms, settle_ms);
    print_report("control", control_samples, rounds, workers, spike_ms, settle_ms);
    free(timer_samples);
    free(io_samples);
    free(control_samples);
    return 0;
}
