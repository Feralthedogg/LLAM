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

#include "managed_bench.h"
#include "llam/io.h"

void llam_managed_bench_cpu(void *argument) {
    llam_managed_bench_work_t *work = argument;
    volatile uint64_t local[2];
    local[0] = work->seed;
    local[1] = 7U;
    uint64_t result = 0U;
    work->begin_ns = llam_now_ns();
    for (uint64_t i = 0U; i < work->iterations; ++i) {
        local[0] = work->seed + i;
        result += local[0] * 3U + local[1];
        if (work->yield_every != 0U && i % work->yield_every == 0U) llam_yield();
    }
    work->result = result;
    work->end_ns = llam_now_ns();
    __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
}

void llam_managed_bench_timer(void *argument) {
    llam_managed_bench_work_t *work = argument;
    work->begin_ns = llam_now_ns();
    for (unsigned i = 0U; i < LLAM_MANAGED_BENCH_SAMPLES; ++i) {
        bool overlap = work->hogs[0] != (void *)0 &&
            (!__atomic_load_n(&work->hogs[0]->finished, __ATOMIC_ACQUIRE) ||
             !__atomic_load_n(&work->hogs[1]->finished, __ATOMIC_ACQUIRE));
        uint64_t start = llam_now_ns();
        if (llam_sleep_ns(1000000U) != 0) { work->error = 1; break; }
        uint64_t elapsed = llam_now_ns() - start;
        work->lag_ns[i] = elapsed > 1000000U ? elapsed - 1000000U : 0U;
        work->samples = i + 1U;
        if (overlap) ++work->overlapping_samples;
    }
    work->end_ns = llam_now_ns();
    __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
}

void llam_managed_bench_io(void *argument) {
    llam_managed_bench_work_t *work = argument;
    work->begin_ns = llam_now_ns();
    __atomic_store_n(&work->io_started, 1U, __ATOMIC_RELEASE);
    for (unsigned i = 0U; i < LLAM_MANAGED_BENCH_SAMPLES; ++i) {
        unsigned char byte;
        if (llam_read(work->io_read_fd, &byte, 1U) != 1 || byte != (unsigned char)i) {
            work->error = 1;
            break;
        }
        uint64_t sent = __atomic_load_n(&work->sent_ns[i], __ATOMIC_ACQUIRE);
        uint64_t now = llam_now_ns();
        if (sent == 0U || now < sent) { work->error = 1; break; }
        work->lag_ns[i] = now - sent;
        work->samples = i + 1U;
        if (work->hogs[0] != (void *)0 &&
            (!__atomic_load_n(&work->hogs[0]->finished, __ATOMIC_ACQUIRE) ||
             !__atomic_load_n(&work->hogs[1]->finished, __ATOMIC_ACQUIRE)))
            ++work->overlapping_samples;
    }
    work->end_ns = llam_now_ns();
    __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
}
