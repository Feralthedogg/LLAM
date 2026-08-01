// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

#include "lcrs_model.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define LCRS_MODEL_TOPOLOGY_VERSION 1U

struct lcrs_model_topology {
    lcrs_model_config_t config;
    lcrs_model_topology_info_t info;
    lcrs_model_shard_desc_t *shards;
    uint32_t *node_ids;
    uint32_t *node_index;
    uint32_t *rank_in_node;
    uint32_t *node_sizes;
    uint32_t *node_offsets;
    uint32_t *members;
    lcrs_model_row_t *rows;
};

static uint64_t lcrs_mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value;
}

static uint32_t lcrs_gcd_u32(uint32_t left, uint32_t right) {
    while (right != 0U) {
        const uint32_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int lcrs_mul_overflow_size(size_t left,
                                  size_t right,
                                  size_t *out) {
    if (out == NULL) {
        return 1;
    }
    if (left != 0U && right > SIZE_MAX / left) {
        return 1;
    }
    *out = left * right;
    return 0;
}

static uint32_t lcrs_ceil_div_u32(uint32_t numerator,
                                  uint32_t denominator) {
    if (numerator == 0U) {
        return 0U;
    }
    return 1U + ((numerator - 1U) / denominator);
}

static int lcrs_row_contains(const lcrs_model_row_t *row,
                             uint32_t candidate) {
    const uint32_t count =
        (uint32_t)row->local_count + (uint32_t)row->remote_count;
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        if (row->candidate_ids[index] == candidate) {
            return 1;
        }
    }
    return 0;
}

static uint32_t lcrs_find_node(const lcrs_model_topology_t *topology,
                               uint32_t node_id) {
    uint32_t index;

    for (index = 0U; index < topology->info.node_count; ++index) {
        if (topology->node_ids[index] == node_id) {
            return index;
        }
    }
    return UINT32_MAX;
}

static int lcrs_allocate_arrays(lcrs_model_topology_t *topology,
                                size_t shard_count,
                                uint32_t epoch_count) {
    size_t row_count;

    if (lcrs_mul_overflow_size(shard_count,
                               (size_t)epoch_count,
                               &row_count) != 0 ||
        row_count > SIZE_MAX / sizeof(*topology->rows)) {
        return EOVERFLOW;
    }
    topology->shards = calloc(shard_count, sizeof(*topology->shards));
    topology->node_ids = calloc(shard_count, sizeof(*topology->node_ids));
    topology->node_index = calloc(shard_count, sizeof(*topology->node_index));
    topology->rank_in_node =
        calloc(shard_count, sizeof(*topology->rank_in_node));
    topology->node_sizes = calloc(shard_count, sizeof(*topology->node_sizes));
    topology->node_offsets =
        calloc(shard_count + 1U, sizeof(*topology->node_offsets));
    topology->members = calloc(shard_count, sizeof(*topology->members));
    topology->rows = calloc(row_count, sizeof(*topology->rows));
    if (topology->shards == NULL || topology->node_ids == NULL ||
        topology->node_index == NULL || topology->rank_in_node == NULL ||
        topology->node_sizes == NULL || topology->node_offsets == NULL ||
        topology->members == NULL || topology->rows == NULL) {
        return ENOMEM;
    }
    topology->info.allocation_count = 9U;
    return 0;
}

static void lcrs_release_arrays(lcrs_model_topology_t *topology) {
    if (topology == NULL) {
        return;
    }
    free(topology->rows);
    free(topology->members);
    free(topology->node_offsets);
    free(topology->node_sizes);
    free(topology->rank_in_node);
    free(topology->node_index);
    free(topology->node_ids);
    free(topology->shards);
}

void lcrs_model_config_default(lcrs_model_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->palette_epochs = 16U;
    config->local_width = 4U;
    config->remote_width = 4U;
    config->small_runtime_cutoff = 16U;
    config->fullscan_miss_limit = 2U;
}

static int lcrs_validate_config(const lcrs_model_config_t *config) {
    const uint32_t width = config == NULL
                               ? 0U
                               : (uint32_t)config->local_width +
                                     (uint32_t)config->remote_width;

    if (config == NULL || config->palette_epochs == 0U ||
        config->fullscan_miss_limit == 0U ||
        width > LCRS_MODEL_MAX_CANDIDATES) {
        return EINVAL;
    }
    return 0;
}

static int lcrs_index_shards(lcrs_model_topology_t *topology,
                             const lcrs_model_shard_desc_t *shards,
                             size_t shard_count) {
    size_t index;

    for (index = 0U; index < shard_count; ++index) {
        const uint32_t shard_id = shards[index].shard_id;
        uint32_t node_index;

        if ((size_t)shard_id >= shard_count ||
            topology->shards[shard_id].shard_id != UINT32_MAX) {
            return EINVAL;
        }
        topology->shards[shard_id] = shards[index];
        node_index = lcrs_find_node(topology, shards[index].node_id);
        if (node_index == UINT32_MAX) {
            node_index = topology->info.node_count;
            topology->node_ids[node_index] = shards[index].node_id;
            topology->info.node_count += 1U;
        }
        topology->node_index[shard_id] = node_index;
        topology->rank_in_node[shard_id] = topology->node_sizes[node_index];
        topology->node_sizes[node_index] += 1U;
    }

    topology->node_offsets[0] = 0U;
    for (index = 0U; index < (size_t)topology->info.node_count; ++index) {
        topology->node_offsets[index + 1U] =
            topology->node_offsets[index] + topology->node_sizes[index];
        topology->node_sizes[index] = 0U;
    }
    for (index = 0U; index < shard_count; ++index) {
        const uint32_t node_index = topology->node_index[index];
        const uint32_t member_index =
            topology->node_offsets[node_index] + topology->node_sizes[node_index];
        topology->members[member_index] = (uint32_t)index;
        topology->rank_in_node[index] = topology->node_sizes[node_index];
        topology->node_sizes[node_index] += 1U;
    }
    return 0;
}

static uint32_t lcrs_required_epochs(const lcrs_model_topology_t *topology) {
    uint32_t required = topology->config.palette_epochs;
    uint32_t node;

    if (topology->config.local_width != 0U) {
        for (node = 0U; node < topology->info.node_count; ++node) {
            const uint32_t peers = topology->node_sizes[node] == 0U
                                       ? 0U
                                       : topology->node_sizes[node] - 1U;
            const uint32_t epochs = lcrs_ceil_div_u32(
                peers,
                (uint32_t)topology->config.local_width);
            if (epochs > required) {
                required = epochs;
            }
        }
    }
    if (topology->config.remote_width != 0U) {
        for (node = 0U; node < topology->info.node_count; ++node) {
            const uint32_t peers =
                topology->info.shard_count - topology->node_sizes[node];
            const uint32_t epochs = lcrs_ceil_div_u32(
                peers,
                (uint32_t)topology->config.remote_width);
            if (epochs > required) {
                required = epochs;
            }
        }
    }
    return required;
}

static uint32_t lcrs_local_step(const lcrs_model_topology_t *topology,
                                uint32_t node_size) {
    const uint32_t modulus = node_size - 1U;
    uint32_t step = 1U + (uint32_t)(topology->config.runtime_id % modulus);

    while (lcrs_gcd_u32(step, modulus) != 1U) {
        step += 1U;
        if (step > modulus) {
            step = 1U;
        }
    }
    return step;
}

static void lcrs_build_local_rows(lcrs_model_topology_t *topology,
                                  uint32_t epoch,
                                  uint32_t *indegree) {
    uint32_t thief;

    memset(indegree, 0, (size_t)topology->info.shard_count * sizeof(*indegree));
    for (thief = 0U; thief < topology->info.shard_count; ++thief) {
        const uint32_t node = topology->node_index[thief];
        const uint32_t node_size = topology->node_sizes[node];
        const uint32_t peer_count = node_size - 1U;
        const uint32_t count = peer_count < (uint32_t)topology->config.local_width
                                   ? peer_count
                                   : (uint32_t)topology->config.local_width;
        lcrs_model_row_t *row =
            &topology->rows[(size_t)epoch * topology->info.shard_count + thief];
        uint32_t slot;

        row->local_count = (uint16_t)count;
        if (count == 0U) {
            continue;
        }
        for (slot = 0U; slot < count; ++slot) {
            const uint32_t modulus = node_size - 1U;
            const uint32_t step = lcrs_local_step(topology, node_size);
            const uint32_t phase =
                (uint32_t)(topology->config.runtime_id % modulus);
            const uint64_t sequence =
                (uint64_t)epoch * (uint64_t)count + (uint64_t)slot;
            const uint32_t offset =
                1U + (uint32_t)(((sequence * step) + phase) % modulus);
            const uint32_t rank =
                (topology->rank_in_node[thief] + offset) % node_size;
            const uint32_t candidate =
                topology->members[topology->node_offsets[node] + rank];

            row->candidate_ids[slot] = candidate;
            indegree[candidate] += 1U;
            if (indegree[candidate] > topology->info.max_local_indegree) {
                topology->info.max_local_indegree = indegree[candidate];
            }
        }
    }
}

static int lcrs_remote_seen(const uint8_t *seen,
                            uint32_t shard_count,
                            uint32_t thief,
                            uint32_t candidate) {
    return seen[(size_t)thief * shard_count + candidate] != 0U;
}

static void lcrs_remote_mark(uint8_t *seen,
                             uint32_t shard_count,
                             uint32_t thief,
                             uint32_t candidate,
                             uint32_t *seen_count) {
    seen[(size_t)thief * shard_count + candidate] = 1U;
    seen_count[thief] += 1U;
}

static void lcrs_remote_reset(lcrs_model_topology_t *topology,
                              uint8_t *seen,
                              uint32_t thief,
                              uint32_t *seen_count) {
    uint32_t candidate;

    for (candidate = 0U; candidate < topology->info.shard_count; ++candidate) {
        if (topology->node_index[candidate] != topology->node_index[thief]) {
            seen[(size_t)thief * topology->info.shard_count + candidate] = 0U;
        }
    }
    seen_count[thief] = 0U;
}

static uint32_t lcrs_choose_remote_in_node(
    const lcrs_model_topology_t *topology,
    const lcrs_model_row_t *row,
    uint32_t thief,
    uint32_t node,
    uint32_t epoch,
    uint32_t slot,
    const uint8_t *seen,
    const uint32_t *indegree) {
    uint32_t best = UINT32_MAX;
    uint32_t best_degree = UINT32_MAX;
    uint64_t best_tie = UINT64_MAX;
    uint32_t rank;

    for (rank = 0U; rank < topology->node_sizes[node]; ++rank) {
        const uint32_t candidate =
            topology->members[topology->node_offsets[node] + rank];
        const uint64_t tie = lcrs_mix64(
            topology->config.runtime_id ^ ((uint64_t)thief << 32U) ^
            ((uint64_t)epoch << 16U) ^ ((uint64_t)slot << 8U) ^ candidate);

        if (candidate == thief || lcrs_row_contains(row, candidate) != 0 ||
            lcrs_remote_seen(seen,
                             topology->info.shard_count,
                             thief,
                             candidate) != 0) {
            continue;
        }
        if (indegree[candidate] < best_degree ||
            (indegree[candidate] == best_degree && tie < best_tie)) {
            best = candidate;
            best_degree = indegree[candidate];
            best_tie = tie;
        }
    }
    return best;
}

static int lcrs_build_remote_rows(lcrs_model_topology_t *topology,
                                  uint8_t *seen,
                                  uint32_t *seen_count,
                                  uint32_t epoch,
                                  uint32_t *indegree) {
    uint32_t thief;

    memset(indegree, 0, (size_t)topology->info.shard_count * sizeof(*indegree));
    for (thief = 0U; thief < topology->info.shard_count; ++thief) {
        const uint32_t source_node = topology->node_index[thief];
        const uint32_t peer_count =
            topology->info.shard_count - topology->node_sizes[source_node];
        const uint32_t count = peer_count < (uint32_t)topology->config.remote_width
                                   ? peer_count
                                   : (uint32_t)topology->config.remote_width;
        lcrs_model_row_t *row =
            &topology->rows[(size_t)epoch * topology->info.shard_count + thief];
        uint32_t slot;

        row->remote_count = 0U;
        if (count == 0U) {
            continue;
        }
        if (seen_count[thief] >= peer_count) {
            lcrs_remote_reset(topology, seen, thief, seen_count);
        }
        for (slot = 0U; slot < count; ++slot) {
            const uint64_t sequence =
                (uint64_t)epoch * (uint64_t)count + (uint64_t)slot;
            const uint32_t node_offset = topology->info.node_count <= 1U
                                             ? 0U
                                             : 1U + (uint32_t)(
                                                        sequence %
                                                        (topology->info.node_count -
                                                         1U));
            uint32_t node = topology->info.node_count <= 1U
                                ? source_node
                                : (source_node + node_offset) %
                                      topology->info.node_count;
            uint32_t attempts;
            uint32_t candidate = UINT32_MAX;

            if (seen_count[thief] >= peer_count) {
                lcrs_remote_reset(topology, seen, thief, seen_count);
            }
            for (attempts = 0U; attempts < topology->info.node_count; ++attempts) {
                if (node != source_node) {
                    candidate = lcrs_choose_remote_in_node(topology,
                                                           row,
                                                           thief,
                                                           node,
                                                           epoch,
                                                           slot,
                                                           seen,
                                                           indegree);
                    if (candidate != UINT32_MAX) {
                        break;
                    }
                }
                node = (node + 1U) % topology->info.node_count;
            }
            if (candidate == UINT32_MAX) {
                lcrs_remote_reset(topology, seen, thief, seen_count);
                for (attempts = 0U;
                     attempts < topology->info.node_count;
                     ++attempts) {
                    if (node != source_node) {
                        candidate = lcrs_choose_remote_in_node(topology,
                                                               row,
                                                               thief,
                                                               node,
                                                               epoch,
                                                               slot,
                                                               seen,
                                                               indegree);
                        if (candidate != UINT32_MAX) {
                            break;
                        }
                    }
                    node = (node + 1U) % topology->info.node_count;
                }
            }
            if (candidate == UINT32_MAX) {
                return EINVAL;
            }
            row->candidate_ids[(uint32_t)row->local_count + slot] = candidate;
            row->remote_count += 1U;
            lcrs_remote_mark(seen,
                             topology->info.shard_count,
                             thief,
                             candidate,
                             seen_count);
            indegree[candidate] += 1U;
            if (indegree[candidate] > topology->info.max_remote_indegree) {
                topology->info.max_remote_indegree = indegree[candidate];
            }
        }
    }
    return 0;
}

int lcrs_model_topology_create(const lcrs_model_shard_desc_t *shards,
                               size_t shard_count,
                               const lcrs_model_config_t *config,
                               lcrs_model_topology_t **out) {
    lcrs_model_topology_t *topology;
    uint8_t *seen = NULL;
    uint32_t *seen_count = NULL;
    uint32_t *indegree = NULL;
    size_t seen_size;
    uint32_t epoch;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    *out = NULL;
    if (shards == NULL || shard_count == 0U || shard_count > UINT32_MAX ||
        lcrs_validate_config(config) != 0) {
        return EINVAL;
    }
    topology = calloc(1U, sizeof(*topology));
    if (topology == NULL) {
        return ENOMEM;
    }
    topology->config = *config;
    topology->info.version = LCRS_MODEL_TOPOLOGY_VERSION;
    topology->info.shard_count = (uint32_t)shard_count;

    rc = lcrs_allocate_arrays(topology, shard_count, config->palette_epochs);
    if (rc != 0) {
        lcrs_release_arrays(topology);
        free(topology);
        return rc;
    }
    memset(topology->shards, 0xff, shard_count * sizeof(*topology->shards));
    rc = lcrs_index_shards(topology, shards, shard_count);
    if (rc != 0) {
        lcrs_release_arrays(topology);
        free(topology);
        return rc;
    }
    topology->info.epoch_count = lcrs_required_epochs(topology);
    if (topology->info.epoch_count != config->palette_epochs) {
        lcrs_model_row_t *expanded;
        size_t row_count;

        if (lcrs_mul_overflow_size(shard_count,
                                   topology->info.epoch_count,
                                   &row_count) != 0 ||
            row_count > SIZE_MAX / sizeof(*expanded)) {
            lcrs_release_arrays(topology);
            free(topology);
            return EOVERFLOW;
        }
        expanded = calloc(row_count, sizeof(*expanded));
        if (expanded == NULL) {
            lcrs_release_arrays(topology);
            free(topology);
            return ENOMEM;
        }
        free(topology->rows);
        topology->rows = expanded;
    }

    if (lcrs_mul_overflow_size(shard_count, shard_count, &seen_size) != 0) {
        lcrs_release_arrays(topology);
        free(topology);
        return EOVERFLOW;
    }
    seen = calloc(seen_size, sizeof(*seen));
    seen_count = calloc(shard_count, sizeof(*seen_count));
    indegree = calloc(shard_count, sizeof(*indegree));
    if (seen == NULL || seen_count == NULL || indegree == NULL) {
        free(indegree);
        free(seen_count);
        free(seen);
        lcrs_release_arrays(topology);
        free(topology);
        return ENOMEM;
    }
    for (epoch = 0U; epoch < topology->info.epoch_count; ++epoch) {
        lcrs_build_local_rows(topology, epoch, indegree);
        rc = lcrs_build_remote_rows(topology,
                                    seen,
                                    seen_count,
                                    epoch,
                                    indegree);
        if (rc != 0) {
            free(indegree);
            free(seen_count);
            free(seen);
            lcrs_release_arrays(topology);
            free(topology);
            return rc;
        }
    }
    free(indegree);
    free(seen_count);
    free(seen);
    if (lcrs_model_topology_validate(topology) != 0) {
        lcrs_release_arrays(topology);
        free(topology);
        return EINVAL;
    }
    *out = topology;
    return 0;
}

void lcrs_model_topology_destroy(lcrs_model_topology_t *topology) {
    if (topology == NULL) {
        return;
    }
    lcrs_release_arrays(topology);
    free(topology);
}

int lcrs_model_topology_info(const lcrs_model_topology_t *topology,
                             lcrs_model_topology_info_t *out) {
    if (topology == NULL || out == NULL) {
        return EINVAL;
    }
    *out = topology->info;
    return 0;
}

int lcrs_model_topology_config(const lcrs_model_topology_t *topology,
                               lcrs_model_config_t *out) {
    if (topology == NULL || out == NULL) {
        return EINVAL;
    }
    *out = topology->config;
    return 0;
}

const lcrs_model_row_t *lcrs_model_topology_row(
    const lcrs_model_topology_t *topology,
    uint32_t thief_shard,
    uint32_t epoch) {
    if (topology == NULL || thief_shard >= topology->info.shard_count ||
        topology->info.epoch_count == 0U) {
        return NULL;
    }
    return &topology->rows[(size_t)(epoch % topology->info.epoch_count) *
                               topology->info.shard_count +
                           thief_shard];
}

uint32_t lcrs_model_topology_node(const lcrs_model_topology_t *topology,
                                  uint32_t shard_id) {
    if (topology == NULL || shard_id >= topology->info.shard_count) {
        return UINT32_MAX;
    }
    return topology->shards[shard_id].node_id;
}

int lcrs_model_topology_validate(const lcrs_model_topology_t *topology) {
    uint32_t *local_indegree;
    uint32_t *remote_indegree;
    uint32_t max_local = 0U;
    uint32_t max_remote = 0U;
    uint32_t epoch;

    if (topology == NULL || topology->info.version != 1U ||
        topology->info.shard_count == 0U || topology->info.node_count == 0U ||
        topology->info.epoch_count == 0U) {
        return EINVAL;
    }
    local_indegree =
        calloc(topology->info.shard_count, sizeof(*local_indegree));
    remote_indegree =
        calloc(topology->info.shard_count, sizeof(*remote_indegree));
    if (local_indegree == NULL || remote_indegree == NULL) {
        free(remote_indegree);
        free(local_indegree);
        return ENOMEM;
    }
    for (epoch = 0U; epoch < topology->info.epoch_count; ++epoch) {
        uint32_t thief;

        memset(local_indegree,
               0,
               (size_t)topology->info.shard_count * sizeof(*local_indegree));
        memset(remote_indegree,
               0,
               (size_t)topology->info.shard_count * sizeof(*remote_indegree));
        for (thief = 0U; thief < topology->info.shard_count; ++thief) {
            const lcrs_model_row_t *row =
                lcrs_model_topology_row(topology, thief, epoch);
            const uint32_t total =
                (uint32_t)row->local_count + (uint32_t)row->remote_count;
            uint32_t index;

            if (total > LCRS_MODEL_MAX_CANDIDATES) {
                goto invalid;
            }
            for (index = 0U; index < total; ++index) {
                const uint32_t candidate = row->candidate_ids[index];
                uint32_t other;

                if (candidate >= topology->info.shard_count ||
                    candidate == thief) {
                    goto invalid;
                }
                if (index < (uint32_t)row->local_count) {
                    if (topology->node_index[candidate] !=
                        topology->node_index[thief]) {
                        goto invalid;
                    }
                    local_indegree[candidate] += 1U;
                    if (local_indegree[candidate] > max_local) {
                        max_local = local_indegree[candidate];
                    }
                } else if (topology->node_index[candidate] ==
                           topology->node_index[thief]) {
                    goto invalid;
                } else {
                    remote_indegree[candidate] += 1U;
                    if (remote_indegree[candidate] > max_remote) {
                        max_remote = remote_indegree[candidate];
                    }
                }
                for (other = index + 1U; other < total; ++other) {
                    if (candidate == row->candidate_ids[other]) {
                        goto invalid;
                    }
                }
            }
        }
    }
    if (max_local != topology->info.max_local_indegree ||
        max_remote != topology->info.max_remote_indegree ||
        max_local > (uint32_t)topology->config.local_width) {
        goto invalid;
    }
    free(remote_indegree);
    free(local_indegree);
    return 0;

invalid:
    free(remote_indegree);
    free(local_indegree);
    return EINVAL;
}

const char *lcrs_model_policy_name(lcrs_model_policy_t policy) {
    switch (policy) {
        case LCRS_MODEL_GLOBAL_DEEPEST:
            return "global_deepest";
        case LCRS_MODEL_RANDOM_K:
            return "deterministic_random_k";
        case LCRS_MODEL_CODED_ROTATION:
            return "coded_rotation_k";
        case LCRS_MODEL_CODED_WITH_FULLSCAN:
            return "coded_rotation_k_plus_fullscan";
        default:
            return NULL;
    }
}
