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

#ifndef LLAM_MANAGED_BENCH_H
#define LLAM_MANAGED_BENCH_H

#include "llam/runtime.h"
#include <stdbool.h>

enum { LLAM_MANAGED_BENCH_SAMPLES = 256, LLAM_MANAGED_BENCH_TASKS = 8 };
typedef struct llam_managed_bench_work {
    uint64_t iterations;
    uint64_t seed;
    uint64_t result;
    uint64_t begin_ns;
    uint64_t end_ns;
    uint64_t lag_ns[LLAM_MANAGED_BENCH_SAMPLES];
    uint64_t sent_ns[LLAM_MANAGED_BENCH_SAMPLES];
    struct llam_managed_bench_work *hogs[2];
    uint32_t yield_every;
    uint32_t samples;
    uint32_t overlapping_samples;
    uint32_t finished;
    uint32_t io_started;
    uint32_t io_stop;
    int32_t io_read_fd;
    int32_t io_write_fd;
    int32_t io_source_error;
    int32_t error;
    uint32_t io_source_waits;
} llam_managed_bench_work_t;

#endif
