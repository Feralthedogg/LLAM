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

#ifndef LLAM_BENCH_ASYNC_COMPARE_H
#define LLAM_BENCH_ASYNC_COMPARE_H
#include "llam/runtime.h"

typedef struct bench_async_evidence {
    uint64_t qualification_captures;
    uint64_t qualification_restores;
} bench_async_evidence_t;

void bench_async_options(llam_runtime_opts_t *options);
int bench_async_prepare(llam_runtime_t *runtime, bench_async_evidence_t *evidence);
int bench_async_report(llam_runtime_t *runtime, const char *name, const bench_async_evidence_t *evidence);
#endif
