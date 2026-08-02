// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// See LICENSES/OLD-LICENSE/Apache-2.0.txt.

#ifndef LLAM_EXPERIMENTS_LCWE_MODEL_H
#define LLAM_EXPERIMENTS_LCWE_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCWE_MODEL_MAX_LANES 32U
#define LCWE_MODEL_MAX_SITES 32U

typedef enum lcwe_model_mode {
    LCWE_MODEL_SCALAR = 0,
    LCWE_MODEL_COHORT = 1,
    LCWE_MODEL_WAVE_POINTERS = 2,
    LCWE_MODEL_WAVE_CAPSULE = 3,
    LCWE_MODEL_WAVE_AOSOA = 4,
} lcwe_model_mode_t;

typedef enum lcwe_model_workload {
    LCWE_MODEL_EXEC_IO_PIPELINE = 0,
    LCWE_MODEL_EXEC_RPC_STATE = 1,
    LCWE_MODEL_EXEC_EVENT_FANOUT = 2,
} lcwe_model_workload_t;

typedef enum lcwe_model_lifecycle {
    LCWE_MODEL_WAITING = 1,
    LCWE_MODEL_READY = 2,
    LCWE_MODEL_RUNNING = 3,
    LCWE_MODEL_TERMINAL = 4,
} lcwe_model_lifecycle_t;

typedef struct lcwe_model_metrics {
    uint64_t admissions;
    uint64_t tickets;
    uint64_t scalar_calls;
    uint64_t wave_calls;
    uint64_t pointer_lanes;
    uint64_t capsule_lanes;
    uint64_t aosoa_lanes;
    uint64_t hot_allocations;
} lcwe_model_metrics_t;

typedef struct lcwe_model_batch lcwe_model_batch_t;

int lcwe_model_batch_create(lcwe_model_workload_t workload,
                            lcwe_model_mode_t mode,
                            size_t instance_count,
                            unsigned site_count,
                            uint64_t seed,
                            lcwe_model_batch_t **out_batch);
void lcwe_model_batch_destroy(lcwe_model_batch_t *batch);
int lcwe_model_run_round(lcwe_model_batch_t *batch,
                         unsigned lane_width,
                         lcwe_model_metrics_t *metrics);
uint64_t lcwe_model_checksum(const lcwe_model_batch_t *batch);
bool lcwe_model_batch_equal(const lcwe_model_batch_t *lhs,
                            const lcwe_model_batch_t *rhs);
const char *lcwe_model_mode_name(lcwe_model_mode_t mode);
const char *lcwe_model_workload_name(lcwe_model_workload_t workload);
int lcwe_model_parse_mode(const char *text, lcwe_model_mode_t *out);
int lcwe_model_parse_workload(const char *text,
                              lcwe_model_workload_t *out);

#endif
