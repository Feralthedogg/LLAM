/**
 * @file examples/bench_internal.h
 * @brief Shared benchmark state, declarations, and case-level helper contracts.
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

#ifndef LLAM_EXAMPLES_BENCH_INTERNAL_H
#define LLAM_EXAMPLES_BENCH_INTERNAL_H

#include "llam/runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if LLAM_PLATFORM_WINDOWS
#include <io.h>
#include <windows.h>
#if defined(__has_include)
#if __has_include(<afunix.h>)
#include <afunix.h>
#define LLAM_BENCH_HAVE_AFUNIX 1
#endif
#endif
#ifndef LLAM_BENCH_HAVE_AFUNIX
#define LLAM_BENCH_HAVE_AFUNIX 0
#endif
#ifndef POLLIN
#define POLLIN 0x0100
#endif
#ifndef POLLERR
#define POLLERR 0x0001
#endif
#ifndef POLLHUP
#define POLLHUP 0x0002
#endif
#else
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include "env_compat.h"

typedef struct bench_spawn_state {
    unsigned rounds;
    unsigned tasks_per_round;
    unsigned yields_per_task;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_spawn_state_t;

typedef struct bench_ping_state {
    unsigned rounds;
    unsigned messages_per_round;
    llam_channel_t *request;
    llam_channel_t *response;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_ping_state_t;

typedef enum bench_select_mode {
    BENCH_SELECT_READY = 1,
    BENCH_SELECT_PARK_WAKE = 2,
    BENCH_SELECT_TIMEOUT = 3,
} bench_select_mode_t;

typedef struct bench_select_state {
    unsigned rounds;
    unsigned ops_per_round;
    bench_select_mode_t mode;
    llam_channel_t *primary;
    llam_channel_t *secondary;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_select_state_t;

typedef struct bench_echo_state {
    llam_fd_t fd;
    unsigned messages_per_round;
} bench_echo_state_t;

typedef struct bench_io_state {
    unsigned rounds;
    unsigned messages_per_round;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_io_state_t;

typedef struct bench_poll_writer_state {
    llam_fd_t fd;
    unsigned events_per_round;
} bench_poll_writer_state_t;

typedef struct bench_poll_state {
    unsigned rounds;
    unsigned events_per_round;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_poll_state_t;

typedef struct bench_opaque_state {
    unsigned rounds;
    unsigned scopes_per_round;
    unsigned companion_yields;
    unsigned sleep_us;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_opaque_state_t;

typedef struct bench_sleep_state {
    unsigned rounds;
    unsigned tasks_per_round;
    unsigned pre_sleep_yields;
    uint64_t sleep_ns;
    uint64_t *samples_ns;
    atomic_uint failures;
} bench_sleep_state_t;

unsigned bench_env_u32(const char *name, unsigned default_value, unsigned max_value);
unsigned bench_env_u32_allow_zero(const char *name, unsigned default_value, unsigned max_value);
unsigned bench_env_flag_default(const char *name, unsigned default_value);
int bench_env_i32(const char *name, int default_value, int min_value, int max_value);
bool bench_case_selected(const char *name);
bool bench_platform_supports_darwin_io_cases(void);
void bench_print_report(const char *name,
                        unsigned total_rounds,
                        unsigned warmup_rounds,
                        unsigned ops_per_round,
                        uint64_t *samples_ns,
                        const llam_runtime_stats_t *stats);

void bench_spawn_task(void *arg);
void bench_channel_task(void *arg);
void bench_select_task(void *arg);
void bench_io_task(void *arg);
void bench_poll_task(void *arg);
void bench_opaque_task(void *arg);
void bench_sleep_task(void *arg);

#endif
