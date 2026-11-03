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

#ifndef LLAM_TEST_MANAGED_CPU_WORKLOAD_H
#define LLAM_TEST_MANAGED_CPU_WORKLOAD_H

/* Fixed native C layout shared by the authentic SDK-built DSO and its host. */
typedef struct llam_managed_cpu_workload {
    unsigned long long iterations;
    unsigned long long seed;
    unsigned long long result;
    unsigned started;
    unsigned finished;
} llam_managed_cpu_workload_t;

typedef struct llam_managed_cpu_observation {
    llam_managed_cpu_workload_t *first;
    llam_managed_cpu_workload_t *second;
    unsigned both_started;
    unsigned both_unfinished;
    unsigned first_unfinished;
} llam_managed_cpu_observation_t;

typedef struct llam_managed_cleanup_workload {
    void *cancel_token;
    void *tls_address;
    unsigned long long result;
    unsigned key;
    unsigned cleaned;
    unsigned same_thread;
    unsigned error;
} llam_managed_cleanup_workload_t;

#endif
