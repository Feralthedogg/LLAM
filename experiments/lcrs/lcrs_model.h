// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

#ifndef LLAM_EXPERIMENTS_LCRS_MODEL_H
#define LLAM_EXPERIMENTS_LCRS_MODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCRS_MODEL_MAX_CANDIDATES 8U
#define LCRS_MODEL_NO_SHARD UINT32_MAX

typedef enum lcrs_model_policy {
    LCRS_MODEL_GLOBAL_DEEPEST = 0,
    LCRS_MODEL_RANDOM_K,
    LCRS_MODEL_CODED_ROTATION,
    LCRS_MODEL_CODED_WITH_FULLSCAN,
} lcrs_model_policy_t;

typedef struct lcrs_model_config {
    uint32_t palette_epochs;
    uint16_t local_width;
    uint16_t remote_width;
    uint32_t small_runtime_cutoff;
    uint32_t fullscan_miss_limit;
    uint64_t runtime_id;
} lcrs_model_config_t;

typedef struct lcrs_model_shard_desc {
    uint32_t shard_id;
    uint32_t node_id;
} lcrs_model_shard_desc_t;

typedef struct lcrs_model_row {
    uint16_t local_count;
    uint16_t remote_count;
    uint32_t candidate_ids[LCRS_MODEL_MAX_CANDIDATES];
} lcrs_model_row_t;

typedef struct lcrs_model_topology_info {
    uint32_t version;
    uint32_t epoch_count;
    uint32_t shard_count;
    uint32_t node_count;
    uint32_t max_local_indegree;
    uint32_t max_remote_indegree;
    size_t allocation_count;
} lcrs_model_topology_info_t;

typedef struct lcrs_model_topology lcrs_model_topology_t;

typedef struct lcrs_model_shard_snapshot {
    uint32_t shard_id;
    uint32_t depth;
    uint8_t online;
    uint8_t paused;
    uint8_t eligible;
    uint8_t race_lost;
} lcrs_model_shard_snapshot_t;

typedef struct lcrs_model_select_input {
    lcrs_model_policy_t policy;
    uint32_t thief_shard;
    uint32_t epoch;
    uint32_t miss_streak;
    uint64_t random_seed;
    const lcrs_model_shard_snapshot_t *shards;
    size_t shard_count;
    uint8_t pressure;
    uint8_t overflow_seen;
    uint8_t deterministic_runtime;
} lcrs_model_select_input_t;

typedef struct lcrs_model_select_result {
    uint32_t victim_shard;
    uint32_t probes;
    uint32_t candidate_tries;
    uint32_t invalid_candidates;
    uint8_t found;
    uint8_t used_fullscan;
    uint8_t checked_next_epoch;
} lcrs_model_select_result_t;

void lcrs_model_config_default(lcrs_model_config_t *config);

int lcrs_model_topology_create(const lcrs_model_shard_desc_t *shards,
                               size_t shard_count,
                               const lcrs_model_config_t *config,
                               lcrs_model_topology_t **out);
void lcrs_model_topology_destroy(lcrs_model_topology_t *topology);

int lcrs_model_topology_info(const lcrs_model_topology_t *topology,
                             lcrs_model_topology_info_t *out);
int lcrs_model_topology_config(const lcrs_model_topology_t *topology,
                               lcrs_model_config_t *out);
int lcrs_model_topology_validate(const lcrs_model_topology_t *topology);
const lcrs_model_row_t *lcrs_model_topology_row(
    const lcrs_model_topology_t *topology,
    uint32_t thief_shard,
    uint32_t epoch);
uint32_t lcrs_model_topology_node(const lcrs_model_topology_t *topology,
                                  uint32_t shard_id);

int lcrs_model_select(const lcrs_model_topology_t *topology,
                      const lcrs_model_select_input_t *input,
                      lcrs_model_select_result_t *out);

const char *lcrs_model_policy_name(lcrs_model_policy_t policy);

#ifdef __cplusplus
}
#endif

#endif
