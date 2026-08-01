// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

#include "lcrs_model.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct lcrs_ranked_candidate {
    uint32_t shard_id;
    uint32_t depth;
} lcrs_ranked_candidate_t;

typedef struct lcrs_sampled_candidate {
    uint32_t shard_id;
    uint64_t score;
} lcrs_sampled_candidate_t;

static uint64_t lcrs_sim_mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value;
}

static void lcrs_result_reset(lcrs_model_select_result_t *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->victim_shard = LCRS_MODEL_NO_SHARD;
}

static int lcrs_validate_input(const lcrs_model_topology_t *topology,
                               const lcrs_model_select_input_t *input,
                               lcrs_model_topology_info_t *info,
                               lcrs_model_config_t *config) {
    size_t index;

    if (topology == NULL || input == NULL || info == NULL || config == NULL ||
        input->shards == NULL ||
        input->policy < LCRS_MODEL_GLOBAL_DEEPEST ||
        input->policy > LCRS_MODEL_CODED_WITH_FULLSCAN ||
        lcrs_model_topology_info(topology, info) != 0 ||
        lcrs_model_topology_config(topology, config) != 0 ||
        input->shard_count != (size_t)info->shard_count ||
        input->thief_shard >= info->shard_count) {
        return EINVAL;
    }
    for (index = 0U; index < input->shard_count; ++index) {
        const lcrs_model_shard_snapshot_t *snapshot = &input->shards[index];

        if (snapshot->shard_id != (uint32_t)index || snapshot->online > 1U ||
            snapshot->paused > 1U || snapshot->eligible > 1U ||
            snapshot->race_lost > 1U) {
            return EINVAL;
        }
    }
    return 0;
}

static int lcrs_snapshot_eligible(
    const lcrs_model_shard_snapshot_t *snapshot) {
    return snapshot->online != 0U && snapshot->paused == 0U &&
           snapshot->eligible != 0U;
}

static int lcrs_try_ranked(const lcrs_model_select_input_t *input,
                           const uint32_t *candidate_ids,
                           uint32_t candidate_count,
                           lcrs_model_select_result_t *result) {
    lcrs_ranked_candidate_t ranked[LCRS_MODEL_MAX_CANDIDATES];
    uint32_t ranked_count = 0U;
    uint32_t index;

    for (index = 0U; index < candidate_count; ++index) {
        const uint32_t candidate = candidate_ids[index];
        const lcrs_model_shard_snapshot_t *snapshot = &input->shards[candidate];
        uint32_t position;

        result->probes += 1U;
        if (lcrs_snapshot_eligible(snapshot) == 0) {
            result->invalid_candidates += 1U;
            continue;
        }
        if (snapshot->depth == 0U) {
            continue;
        }
        position = ranked_count;
        while (position > 0U &&
               (ranked[position - 1U].depth < snapshot->depth ||
                (ranked[position - 1U].depth == snapshot->depth &&
                 ranked[position - 1U].shard_id > candidate))) {
            ranked[position] = ranked[position - 1U];
            position -= 1U;
        }
        ranked[position].shard_id = candidate;
        ranked[position].depth = snapshot->depth;
        ranked_count += 1U;
    }
    for (index = 0U; index < ranked_count; ++index) {
        const uint32_t candidate = ranked[index].shard_id;

        result->candidate_tries += 1U;
        if (input->shards[candidate].race_lost != 0U) {
            continue;
        }
        result->victim_shard = candidate;
        result->found = 1U;
        return 1;
    }
    return 0;
}

static int lcrs_try_row(const lcrs_model_select_input_t *input,
                        const lcrs_model_row_t *row,
                        lcrs_model_select_result_t *result) {
    if (lcrs_try_ranked(input,
                        row->candidate_ids,
                        (uint32_t)row->local_count,
                        result) != 0) {
        return 1;
    }
    return lcrs_try_ranked(input,
                           &row->candidate_ids[row->local_count],
                           (uint32_t)row->remote_count,
                           result);
}

static void lcrs_fullscan(const lcrs_model_topology_info_t *info,
                          const lcrs_model_select_input_t *input,
                          lcrs_model_select_result_t *result) {
    uint32_t best = LCRS_MODEL_NO_SHARD;
    uint32_t best_depth = 0U;
    uint32_t shard;

    result->used_fullscan = 1U;
    for (shard = 0U; shard < info->shard_count; ++shard) {
        const lcrs_model_shard_snapshot_t *snapshot;

        if (shard == input->thief_shard) {
            continue;
        }
        snapshot = &input->shards[shard];
        result->probes += 1U;
        if (lcrs_snapshot_eligible(snapshot) == 0) {
            result->invalid_candidates += 1U;
            continue;
        }
        if (snapshot->depth > best_depth ||
            (snapshot->depth == best_depth && snapshot->depth != 0U &&
             shard < best)) {
            best = shard;
            best_depth = snapshot->depth;
        }
    }
    if (best == LCRS_MODEL_NO_SHARD || best_depth == 0U) {
        return;
    }
    result->candidate_tries += 1U;
    if (input->shards[best].race_lost != 0U) {
        return;
    }
    result->victim_shard = best;
    result->found = 1U;
}

static void lcrs_sample_insert(lcrs_sampled_candidate_t *samples,
                               uint32_t *count,
                               uint32_t width,
                               uint32_t shard_id,
                               uint64_t score) {
    uint32_t position;

    if (width == 0U) {
        return;
    }
    if (*count < width) {
        position = *count;
        *count += 1U;
    } else {
        const uint32_t last = width - 1U;
        if (samples[last].score < score ||
            (samples[last].score == score &&
             samples[last].shard_id < shard_id)) {
            return;
        }
        position = last;
    }
    while (position > 0U &&
           (samples[position - 1U].score > score ||
            (samples[position - 1U].score == score &&
             samples[position - 1U].shard_id > shard_id))) {
        samples[position] = samples[position - 1U];
        position -= 1U;
    }
    samples[position].shard_id = shard_id;
    samples[position].score = score;
}

static void lcrs_build_random_row(const lcrs_model_topology_t *topology,
                                  const lcrs_model_topology_info_t *info,
                                  const lcrs_model_config_t *config,
                                  const lcrs_model_select_input_t *input,
                                  lcrs_model_row_t *row) {
    lcrs_sampled_candidate_t local[LCRS_MODEL_MAX_CANDIDATES];
    lcrs_sampled_candidate_t remote[LCRS_MODEL_MAX_CANDIDATES];
    uint32_t local_count = 0U;
    uint32_t remote_count = 0U;
    const uint32_t thief_node =
        lcrs_model_topology_node(topology, input->thief_shard);
    uint32_t shard;

    memset(row, 0, sizeof(*row));
    for (shard = 0U; shard < info->shard_count; ++shard) {
        const uint32_t node = lcrs_model_topology_node(topology, shard);
        const int is_local = node == thief_node;
        const uint64_t class_tag = is_local ? UINT64_C(0x11) : UINT64_C(0x29);
        const uint64_t score = lcrs_sim_mix64(
            input->random_seed ^ config->runtime_id ^
            ((uint64_t)input->thief_shard << 32U) ^
            ((uint64_t)input->epoch << 16U) ^ ((uint64_t)shard << 1U) ^
            class_tag);

        if (shard == input->thief_shard) {
            continue;
        }
        if (is_local) {
            lcrs_sample_insert(local,
                               &local_count,
                               (uint32_t)config->local_width,
                               shard,
                               score);
        } else {
            lcrs_sample_insert(remote,
                               &remote_count,
                               (uint32_t)config->remote_width,
                               shard,
                               score);
        }
    }
    row->local_count = (uint16_t)local_count;
    row->remote_count = (uint16_t)remote_count;
    for (shard = 0U; shard < local_count; ++shard) {
        row->candidate_ids[shard] = local[shard].shard_id;
    }
    for (shard = 0U; shard < remote_count; ++shard) {
        row->candidate_ids[local_count + shard] = remote[shard].shard_id;
    }
}

int lcrs_model_select(const lcrs_model_topology_t *topology,
                      const lcrs_model_select_input_t *input,
                      lcrs_model_select_result_t *out) {
    lcrs_model_topology_info_t info;
    lcrs_model_config_t config;

    if (out == NULL) {
        return EINVAL;
    }
    lcrs_result_reset(out);
    if (lcrs_validate_input(topology, input, &info, &config) != 0) {
        return EINVAL;
    }
    if (input->deterministic_runtime != 0U) {
        return 0;
    }
    if (input->policy == LCRS_MODEL_GLOBAL_DEEPEST) {
        lcrs_fullscan(&info, input, out);
        return 0;
    }
    if (input->policy == LCRS_MODEL_RANDOM_K) {
        lcrs_model_row_t row;

        lcrs_build_random_row(topology, &info, &config, input, &row);
        (void)lcrs_try_row(input, &row, out);
        return 0;
    }
    if (input->policy == LCRS_MODEL_CODED_WITH_FULLSCAN &&
        ((config.small_runtime_cutoff != 0U &&
          info.shard_count < config.small_runtime_cutoff) ||
         input->pressure != 0U || input->overflow_seen != 0U ||
         input->miss_streak >= config.fullscan_miss_limit)) {
        lcrs_fullscan(&info, input, out);
        return 0;
    }
    {
        const lcrs_model_row_t *row = lcrs_model_topology_row(
            topology,
            input->thief_shard,
            input->epoch);
        const uint32_t row_count =
            (uint32_t)row->local_count + (uint32_t)row->remote_count;

        if (lcrs_try_row(input, row, out) != 0) {
            return 0;
        }
        if (input->policy == LCRS_MODEL_CODED_ROTATION) {
            return 0;
        }
        if (row_count != 0U && out->invalid_candidates * 2U >= row_count) {
            lcrs_fullscan(&info, input, out);
            return 0;
        }
    }
    if (input->miss_streak == 0U) {
        const lcrs_model_row_t *next = lcrs_model_topology_row(
            topology,
            input->thief_shard,
            input->epoch + 1U);
        const uint32_t before_invalid = out->invalid_candidates;
        const uint32_t next_count =
            (uint32_t)next->local_count + (uint32_t)next->remote_count;

        out->checked_next_epoch = 1U;
        if (lcrs_try_row(input, next, out) != 0) {
            return 0;
        }
        if (next_count != 0U &&
            (out->invalid_candidates - before_invalid) * 2U >= next_count) {
            lcrs_fullscan(&info, input, out);
        }
    }
    return 0;
}
